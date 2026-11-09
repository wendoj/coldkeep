/* SPDX-License-Identifier: Apache-2.0
 *
 * In-process wiring of one store: plant + case controllers + (protected mode)
 * broker + governor + keyconsole queue. Used by sim-host and the property tests.
 * On QNX each piece is a separate process (target/); this file is the host model.
 *
 * LEGACY mode is a deliberately simplified model of a flat architecture: the
 * management API writes the case controllers directly, with no governor.
 */
#include "coldkeep.h"

#include <errno.h>
#include <math.h>
#include <string.h>


static void emit(ck_store_t *s, ck_ev_kind_t k, const void *p) {
  if (s->cb) s->cb(s, k, p, s->cb_ctx);
}

void ck_store_init(ck_store_t *s, ck_mode_t mode, uint64_t seed, uint16_t n,
                   const ck_case_type_t *types, float ambient_c, float dt_s) {
  memset(s, 0, sizeof *s);
  s->mode = mode;
  s->seed = seed;
  s->n = n > CK_MAX_CASES ? CK_MAX_CASES : n;
  s->next_cmd_id = 1;
  s->alarms_enabled = true;
  s->gov_up = (mode == MODE_PROTECTED);
  ck_plant_init(&s->plant, seed, s->n, types, ambient_c, dt_s);
  for (uint16_t i = 0; i < s->n; i++)
    ck_ctl_init(&s->ctl[i], (uint16_t)(i + 1), types[i], mode == MODE_PROTECTED, 0);
  ck_envelope_default(&s->env);
  ck_gov_init(&s->gov, &s->env, s->n, types, 0);
  ck_broker_init(&s->broker, &s->gov.env, s->n, types);
  ck_keyq_init(&s->keyq);
  ck_metrics_init(&s->m, s->n, s->plant.st);
  for (uint16_t i = 0; i < s->n; i++) s->legacy_next_sched_ns[i] = s->gov.c[i].next_sched_ns;
}

void ck_store_set_sink(ck_store_t *s, ck_ev_cb cb, void *ctx) { s->cb = cb; s->cb_ctx = ctx; }

/* The governor sees probe values, not ground truth. */
static void probe_view(const ck_store_t *s, ck_case_state_t *v) {
  for (uint16_t i = 0; i < s->n; i++) {
    v[i] = s->plant.st[i];
    v[i].air_c = s->plant.probe[i].air_c;
    v[i].product_c = s->plant.probe[i].product_c;
    v[i].evap_c = s->plant.probe[i].evap_c;
    /* defrost state straight from the controller, so several commands in the
     * same instant cannot all pass the G3 concurrency check */
    v[i].defrost_on = ck_ctl_in_defrost(&s->ctl[i]) ? 1 : 0;
  }
}

static void record(ck_store_t *s, const ck_decision_t *d) {
  ck_metrics_decision(&s->m, d->verdict);
  emit(s, EV_DECISION, d);
}

static void governor_tick(ck_store_t *s) {
  ck_case_state_t view[CK_MAX_CASES];
  ck_gov_action_t acts[3 * CK_MAX_CASES];
  ck_decision_t decs[3 * CK_MAX_CASES];
  probe_view(s, view);
  int k = ck_gov_tick(&s->gov, view, s->now, acts, decs, 3 * CK_MAX_CASES);
  for (int i = 0; i < k; i++) {
    if (acts[i].cmd.type != 0) {
      ck_ctl_command(&s->ctl[acts[i].case_id - 1], &acts[i].cmd, s->now);
      /* keep view in sync so a later schedule decision in this tick sees it */
    }
    record(s, &decs[i]);
  }
}

static void legacy_schedule(ck_store_t *s) {
  for (uint16_t i = 0; i < s->n; i++) {
    if (s->now < s->legacy_next_sched_ns[i]) continue;
    const ck_env_type_t *et = &s->env.t[s->plant.st[i].type];
    ck_case_cmd_t c = {.type = CASE_CMD_START_DEFROST, .duration_s = et->sched_defrost_s,
                       .term_c = et->term_c};
    ck_ctl_command(&s->ctl[i], &c, s->now);
    s->legacy_next_sched_ns[i] = s->now + CK_S(et->sched_interval_s);
  }
}

static bool alarms_on(const ck_store_t *s, uint16_t i) {
  return s->mode == MODE_LEGACY ? s->alarms_enabled : s->gov.c[i].alarms_enabled;
}

void ck_store_step(ck_store_t *s) {
  /* supervisor restarts the governor */
  if (s->mode == MODE_PROTECTED && !s->gov_up && s->now >= s->gov_restart_ns) {
    s->gov_up = true;
    s->next_gov_tick_ns = s->now;
    ck_ev_proc_t ev = {"governord", "restarted by supervisor"};
    emit(s, EV_PROC, &ev);
  }
  if (s->mode == MODE_PROTECTED && s->gov_up) {
    const ck_case_cmd_t hb = {.type = CASE_CMD_HEARTBEAT};
    for (uint16_t i = 0; i < s->n; i++) ck_ctl_command(&s->ctl[i], &hb, s->now);
  }

  ck_plant_sense(&s->plant, s->now);
  for (uint16_t i = 0; i < s->n; i++) ck_ctl_tick(&s->ctl[i], &s->plant.probe[i], s->now, &s->plant.st[i]);

  if (s->mode == MODE_PROTECTED && s->gov_up && s->now >= s->next_gov_tick_ns) {
    governor_tick(s);
    s->next_gov_tick_ns = s->now + CK_NS_PER_S;
  }
  if (s->mode == MODE_LEGACY) legacy_schedule(s);

  for (uint16_t i = 0; i < s->n; i++) {
    if (s->ctl[i].hold != s->hold_reported[i]) {
      s->hold_reported[i] = s->ctl[i].hold;
      ck_ev_g6_t ev = {.case_id = (uint16_t)(i + 1), .hold = s->ctl[i].hold};
      emit(s, EV_G6, &ev);
    }
  }

  ck_plant_advance(&s->plant);
  s->now += ck_plant_dt_ns(&s->plant);
  ck_metrics_sample(&s->m, s->plant.st, s->now, s->plant.dt_s);

  for (uint16_t i = 0; i < s->n; i++) {
    const float lim = s->m.c[i].limit_c;
    if (!s->alarm_latched[i] && s->plant.st[i].product_c > lim) {
      s->alarm_latched[i] = 1;
      ck_ev_alarm_t ev = {(uint16_t)(i + 1), s->plant.st[i].product_c, lim, (uint8_t)!alarms_on(s, i)};
      emit(s, EV_ALARM, &ev);
    } else if (s->alarm_latched[i] && s->plant.st[i].product_c < lim - 0.5f) {
      s->alarm_latched[i] = 0;
    }
  }
  if (ck_metrics_compliance_due(&s->m, s->now)) emit(s, EV_COMPLIANCE, NULL);
}

bool ck_store_login(ck_store_t *s, uint32_t session, const char *user, const char *pass) {
  /* Default credentials admin/admin, deliberately: they mirror the reported root cause. */
  const bool ok = user && pass && strcmp(user, "admin") == 0 && strcmp(pass, "admin") == 0;
  if (session < CK_MAX_SESSIONS) s->sessions_authed[session] = ok;
  return ok;
}

static ck_verdict_t legacy_apply(ck_store_t *s, ck_cmd_t *cmd, ck_decision_t *d) {
  int err = 0;
  ck_case_cmd_t cc;
  ck_ctl_t *ctl = (cmd->case_id >= 1 && cmd->case_id <= s->n) ? &s->ctl[cmd->case_id - 1] : NULL;
  if (ck_ctl_legacy_map(cmd, &cc)) {
    err = ctl ? ck_ctl_command(ctl, &cc, s->now) : EINVAL;
  } else switch (cmd->type) {
    case CMD_DISABLE_ALARMS:
      s->alarms_enabled = false;
      break;
    case CMD_SET_DEFROST_SCHEDULE:
      if (!ctl) err = EINVAL;
      else if (cmd->value >= 1.0f)
        s->env.t[s->plant.st[cmd->case_id - 1].type].sched_interval_s = (uint32_t)(cmd->value * 3600.0f);
      break;
    default: /* change_envelope: no envelope exists in the flat architecture; status */
      break;
  }
  d->rule_id = CK_RULE_LEGACY;
  d->applied_value = err ? NAN : cmd->value;
  if (err) {
    d->verdict = VERDICT_DENY;
    ck_strlcpy(d->reason, err == EBUSY ? "legacy: controller busy" : "legacy: controller rejected",
               sizeof d->reason);
  } else {
    d->verdict = VERDICT_ALLOW;
    ck_strlcpy(d->reason, "legacy: applied directly", sizeof d->reason);
  }
  return d->verdict;
}

ck_verdict_t ck_store_mgmt(ck_store_t *s, uint32_t session, ck_cmd_type_t type,
                           uint16_t case_id, float value, ck_decision_t *d) {
  ck_cmd_t cmd = {0};
  cmd.id = s->next_cmd_id++;
  cmd.recv_ns = s->now;
  ck_strlcpy(cmd.origin, "mgmt", sizeof cmd.origin);
  ck_strlcpy(cmd.user, "admin", sizeof cmd.user);
  cmd.type = type; cmd.case_id = case_id; cmd.value = value;
  memset(d, 0, sizeof *d);
  d->cmd = cmd;
  d->applied_value = NAN;

  if (session >= CK_MAX_SESSIONS || !s->sessions_authed[session]) {
    d->verdict = VERDICT_DENY; d->rule_id = CK_RULE_AUTH;
    ck_strlcpy(d->reason, "not logged in", sizeof d->reason);
    record(s, d);
    return d->verdict;
  }

  if (s->mode == MODE_LEGACY) {
    legacy_apply(s, &cmd, d);
    if (type != CMD_STATUS) record(s, d);
    return d->verdict;
  }

  switch (ck_broker_check(&s->broker, session, &cmd, s->now, d)) {
    case BROKER_READ:
      d->verdict = VERDICT_ALLOW;
      ck_strlcpy(d->reason, "read-only", sizeof d->reason);
      return d->verdict; /* reads are not audited as decisions */
    case BROKER_REJECT:
      if (d->rule_id != CK_RULE_RATE) record(s, d); /* rate-limit hits are counted, not logged */
      return d->verdict;
    case BROKER_NEEDS_KEY: {
      uint64_t req = ck_keyq_push(&s->keyq, &cmd);
      char why[96];
      if (req) snprintf(why, sizeof why, "dangerous: queued as key request #%llu", (unsigned long long)req);
      else snprintf(why, sizeof why, "dangerous: key queue full, dropped");
      ck_strlcpy(d->reason, why, sizeof d->reason);
      record(s, d);
      return d->verdict;
    }
    case BROKER_FORWARD:
      break;
  }

  if (!s->gov_up) {
    d->verdict = VERDICT_DENY; d->rule_id = CK_RULE_UNAVAILABLE;
    ck_strlcpy(d->reason, "governor unavailable; plant holds safe config", sizeof d->reason);
    record(s, d);
    return d->verdict;
  }
  ck_case_state_t view[CK_MAX_CASES];
  probe_view(s, view);
  ck_gov_action_t act;
  if (ck_gov_decide(&s->gov, &cmd, view, s->now, d, &act) && act.cmd.type != 0)
    ck_ctl_command(&s->ctl[act.case_id - 1], &act.cmd, s->now);
  record(s, d);
  return d->verdict;
}

int ck_store_compromised_attempt(ck_store_t *s, const char *action) {
  /* Host model of the target's access control. On QNX the errno comes from the
   * kernel / resource managers; here it is what the design says it must be. */
  static const struct { const char *name; int prot_errno; } table[] = {
    {"devctl_case", EPERM},        /* casectl checks coldkeep.plant.write */
    {"connect_governor", EACCES},  /* security policy: mgmt may only reach brokerd */
    {"regain_abilities", EPERM},   /* abilities dropped + locked */
    {"spawn", EPERM},              /* spawn ability dropped + locked */
    {"tamper_audit", EACCES},      /* journal owned by auditd, not writable */
  };
  for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) {
    if (strcmp(action, table[i].name) != 0) continue;
    if (s->mode == MODE_PROTECTED) return table[i].prot_errno;
    /* legacy: mgmtd runs as root with all abilities; everything succeeds */
    if (i == 0 && s->n > 0) {
      ck_case_cmd_t cc = {.type = CASE_CMD_RAW_ACTUATORS, .raw_override = 1, .raw_heater = 1};
      ck_ctl_command(&s->ctl[0], &cc, s->now);
    }
    return 0;
  }
  return EINVAL;
}

void ck_store_kill_governor(ck_store_t *s, float restart_s) {
  if (s->mode != MODE_PROTECTED) {
    emit(s, EV_NOTE, "kill governord: no governor in legacy mode (n/a)");
    return;
  }
  s->gov_up = false;
  s->gov_restart_ns = s->now + (uint64_t)(restart_s * 1000.0f) * 1000000ULL;
  ck_ev_proc_t ev = {"governord", "killed"};
  emit(s, EV_PROC, &ev);
}

bool ck_store_key_approve(ck_store_t *s, uint64_t req_id) {
  if (s->mode != MODE_PROTECTED) return false;
  ck_keyreq_t *r = ck_keyq_find(&s->keyq, req_id);
  if (!r || r->state != 0) return false;
  r->state = 1;
  ck_cmd_t cmd = r->cmd;
  ck_strlcpy(cmd.origin, "keyconsole", sizeof cmd.origin);
  ck_decision_t d;
  bool ok = ck_gov_apply_approved(&s->gov, &cmd, req_id, &d);
  record(s, &d);
  return ok;
}
