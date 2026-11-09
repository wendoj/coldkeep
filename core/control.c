/* SPDX-License-Identifier: Apache-2.0
 *
 * casectl control logic: thermostat with hysteresis, defrost sequencing
 * (pump-down -> heat -> drip -> recover), fans, and the G6 fail-safe hold.
 *
 * Defrost timing: duration_s is the WHOLE defrost cycle (pump-down + heat + drip),
 * so a cycle never exceeds what the governor approved (invariant 2). The heat phase
 * also ends early when the coil reaches term_c, unless term_c == CK_TERM_NONE
 * ("forced" defrost — only the legacy flat architecture ever sends that).
 */
#include "coldkeep.h"

#include <errno.h>

void ck_ctl_init(ck_ctl_t *c, uint16_t id, ck_case_type_t t, bool protected_mode, uint64_t now) {
  const ck_thermal_params_t *p = ck_thermal_defaults(t);
  *c = (ck_ctl_t){0};
  c->id = id; c->type = t;
  c->setpoint_c = p->setpoint;
  c->hyst = p->hyst;
  c->phase = DF_IDLE;
  c->fans_enabled = 1;
  c->protected_mode = protected_mode;
  c->last_hb_ns = now;
  c->hb_timeout_ns = CK_HB_TIMEOUT_NS;
}

bool ck_ctl_in_defrost(const ck_ctl_t *c) {
  return c->phase == DF_PUMPDOWN || c->phase == DF_HEAT || c->phase == DF_DRIP;
}

static void enter(ck_ctl_t *c, ck_df_phase_t ph, uint64_t now) {
  c->phase = ph; c->phase_start_ns = now;
}

static void end_heat(ck_ctl_t *c, uint64_t now) {
  /* go to drip; drip never extends past the approved deadline */
  enter(c, DF_DRIP, now);
}

int ck_ctl_command(ck_ctl_t *c, const ck_case_cmd_t *cmd, uint64_t now) {
  switch (cmd->type) {
    case CASE_CMD_HEARTBEAT:
      c->last_hb_ns = now;
      c->hold = false;
      return 0;
    case CASE_CMD_SET_SETPOINT:
      if (!(cmd->setpoint_c == cmd->setpoint_c)) return EINVAL; /* NaN */
      c->setpoint_c = cmd->setpoint_c;
      return 0;
    case CASE_CMD_START_DEFROST:
      /* G6: never start a defrost while the governor is gone */
      if (c->hold) return EPERM;
      if (c->phase != DF_IDLE && c->phase != DF_RECOVER) return EBUSY;
      if (cmd->duration_s == 0) return EINVAL;
      c->defrost_start_ns = now;
      c->defrost_deadline_ns = now + CK_S(cmd->duration_s);
      c->term_c = cmd->term_c;
      c->defrosts_started++;
      enter(c, DF_PUMPDOWN, now);
      return 0;
    case CASE_CMD_STOP_DEFROST:
      if (c->phase == DF_PUMPDOWN || c->phase == DF_HEAT) end_heat(c, now);
      return 0;
    case CASE_CMD_SET_FANS:
      c->fans_enabled = cmd->fans ? 1 : 0;
      return 0;
    case CASE_CMD_RAW_ACTUATORS:
      c->raw_override = cmd->raw_override;
      c->raw_compressor = cmd->raw_compressor;
      c->raw_heater = cmd->raw_heater;
      c->raw_fans = cmd->raw_fans;
      return 0;
  }
  return EINVAL;
}

void ck_ctl_tick(ck_ctl_t *c, const ck_probe_t *pr, uint64_t now, ck_case_state_t *out) {
  /* G6 fail-safe: governor heartbeat lost -> hold last-known-good config */
  if (c->protected_mode && now - c->last_hb_ns > c->hb_timeout_ns && !c->hold) {
    c->hold = true;
    c->raw_override = 0;
    c->fans_enabled = 1;
    if (c->phase == DF_PUMPDOWN || c->phase == DF_HEAT) end_heat(c, now);
  }

  /* defrost sequencing */
  const uint64_t drip_ns = CK_S(CK_DRIP_S);
  switch (c->phase) {
    case DF_PUMPDOWN:
      if (now - c->phase_start_ns >= CK_S(CK_PUMPDOWN_S) ||
          now + drip_ns >= c->defrost_deadline_ns)
        enter(c, DF_HEAT, now);
      if (c->phase != DF_HEAT) break;
      /* fallthrough */
    case DF_HEAT:
      if (now + drip_ns >= c->defrost_deadline_ns ||
          (c->term_c < CK_TERM_NONE && pr->evap_c >= c->term_c))
        end_heat(c, now);
      break;
    case DF_DRIP: {
      uint64_t end = c->phase_start_ns + drip_ns;
      if (end > c->defrost_deadline_ns) end = c->defrost_deadline_ns;
      if (now >= end) {
        out->last_defrost_end_ns = now;
        enter(c, DF_RECOVER, now);
      }
      break;
    }
    case DF_RECOVER:
      if (pr->evap_c < pr->air_c - 4.0f || now - c->phase_start_ns >= CK_S(CK_RECOVER_MAX_S))
        enter(c, DF_IDLE, now);
      break;
    case DF_IDLE:
      break;
  }

  uint8_t comp = out->compressor_on, heater = 0, fans = c->fans_enabled;
  switch (c->phase) {
    case DF_IDLE:
      if (pr->air_c > c->setpoint_c + c->hyst) comp = 1;
      else if (pr->air_c < c->setpoint_c - c->hyst) comp = 0;
      break;
    case DF_PUMPDOWN: comp = 0; fans = 1; break;
    case DF_HEAT:     comp = 0; heater = 1; fans = 0; break;
    case DF_DRIP:     comp = 0; fans = 0; break;
    case DF_RECOVER:  comp = 1; fans = 0; break;
  }
  if (c->raw_override) {
    comp = c->raw_compressor; heater = c->raw_heater; fans = c->raw_fans;
  }

  out->compressor_on = comp;
  out->heater_on = heater;
  out->fans_on = fans;
  out->setpoint_c = c->setpoint_c;
  const uint8_t was = out->defrost_on;
  out->defrost_on = ck_ctl_in_defrost(c) ? 1 : 0;
  if (out->defrost_on && !was) out->defrost_started_ns = c->defrost_start_ns;
}

bool ck_ctl_legacy_map(const ck_cmd_t *cmd, ck_case_cmd_t *out) {
  *out = (ck_case_cmd_t){0};
  switch (cmd->type) {
    case CMD_SET_SETPOINT:
      out->type = CASE_CMD_SET_SETPOINT; out->setpoint_c = cmd->value;
      return true;
    case CMD_START_DEFROST:
      out->type = CASE_CMD_START_DEFROST;
      out->duration_s = cmd->value >= 1.0f ? (uint32_t)cmd->value : 1u;
      out->term_c = CK_TERM_NONE;
      return true;
    case CMD_STOP_DEFROST:
      out->type = CASE_CMD_STOP_DEFROST;
      return true;
    case CMD_SET_FANS:
      out->type = CASE_CMD_SET_FANS; out->fans = cmd->value != 0.0f;
      return true;
    case CMD_RAW_WRITE:
      out->type = CASE_CMD_RAW_ACTUATORS;
      out->raw_override = cmd->value != 0.0f;
      out->raw_heater = out->raw_override;
      return true;
    default:
      return false;
  }
}
