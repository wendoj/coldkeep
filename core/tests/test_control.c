/* SPDX-License-Identifier: Apache-2.0
 * casectl logic: thermostat, defrost sequencing, G6 fail-safe hold. */
#include <errno.h>

#include "ck_test.h"
#include "coldkeep.h"

static void tick(ck_ctl_t *c, ck_case_state_t *s, uint64_t now, float air, float evap) {
  ck_probe_t pr = {air, s->product_c, evap};
  ck_ctl_tick(c, &pr, now, s);
}

static void thermostat_hysteresis(void) {
  ck_ctl_t c; ck_case_state_t s = {0};
  ck_ctl_init(&c, 1, CASE_CHILLED, false, 0);
  tick(&c, &s, 0, 3.5f, -2); CHECK(!s.compressor_on);     /* inside band: stays off */
  tick(&c, &s, 1, 4.1f, -2); CHECK(s.compressor_on);      /* above sp+h: on */
  tick(&c, &s, 2, 2.5f, -2); CHECK(s.compressor_on);      /* inside band: stays on */
  tick(&c, &s, 3, 1.9f, -2); CHECK(!s.compressor_on);     /* below sp-h: off */
  CHECK(s.fans_on);
}

static void defrost_sequence_and_duration(void) {
  ck_ctl_t c; ck_case_state_t s = {0};
  ck_ctl_init(&c, 1, CASE_CHILLED, false, 0);
  ck_case_cmd_t d = {.type = CASE_CMD_START_DEFROST, .duration_s = 600, .term_c = CK_TERM_NONE};
  CHECK_EQ_INT(ck_ctl_command(&c, &d, 0), 0);
  CHECK_EQ_INT(ck_ctl_command(&c, &d, 0), EBUSY);
  uint64_t t = 0, last_on = 0; int saw_heat = 0;
  for (; t <= CK_S(900); t += CK_TICK_NS) {
    tick(&c, &s, t, 5.0f, 5.0f);
    if (s.defrost_on) last_on = t;
    if (s.heater_on) { saw_heat = 1; CHECK(!s.compressor_on && !s.fans_on); }
  }
  CHECK(saw_heat);
  CHECK_MSG(last_on <= CK_S(600), "defrost_on until %.1f s", (double)last_on / 1e9);
  CHECK(last_on >= CK_S(599));
  CHECK_EQ_INT(c.phase, DF_IDLE); /* recover capped at 90 s, so idle by 900 s */
}

static void termination_ends_heat(void) {
  ck_ctl_t c; ck_case_state_t s = {0};
  ck_ctl_init(&c, 1, CASE_CHILLED, false, 0);
  ck_case_cmd_t d = {.type = CASE_CMD_START_DEFROST, .duration_s = 2700, .term_c = 10.0f};
  ck_ctl_command(&c, &d, 0);
  for (uint64_t t = 0; t < CK_S(60); t += CK_TICK_NS) tick(&c, &s, t, 4, 0.0f);
  CHECK_EQ_INT(c.phase, DF_HEAT);
  tick(&c, &s, CK_S(60), 4, 10.5f);
  CHECK_EQ_INT(c.phase, DF_DRIP);
  CHECK(!s.heater_on);
}

static void stop_defrost(void) {
  ck_ctl_t c; ck_case_state_t s = {0};
  ck_ctl_init(&c, 1, CASE_CHILLED, false, 0);
  ck_case_cmd_t d = {.type = CASE_CMD_START_DEFROST, .duration_s = 2700, .term_c = CK_TERM_NONE};
  ck_ctl_command(&c, &d, 0);
  for (uint64_t t = 0; t < CK_S(60); t += CK_TICK_NS) tick(&c, &s, t, 4, 0);
  ck_case_cmd_t st = {.type = CASE_CMD_STOP_DEFROST};
  ck_ctl_command(&c, &st, CK_S(60));
  tick(&c, &s, CK_S(60), 4, 0);
  CHECK(!s.heater_on);
}

static void g6_hold_when_governor_lost(void) {
  ck_ctl_t c; ck_case_state_t s = {0};
  ck_ctl_init(&c, 1, CASE_CHILLED, true, 0);
  ck_case_cmd_t hb = {.type = CASE_CMD_HEARTBEAT};
  ck_case_cmd_t d = {.type = CASE_CMD_START_DEFROST, .duration_s = 2700, .term_c = CK_TERM_NONE};
  ck_ctl_command(&c, &d, 0);
  for (uint64_t t = 0; t < CK_S(60); t += CK_TICK_NS) { ck_ctl_command(&c, &hb, t); tick(&c, &s, t, 4, 0); }
  CHECK(s.heater_on);
  /* heartbeats stop at 60 s; at 62 s + one tick the controller must hold */
  uint64_t t = CK_S(60);
  for (; t <= CK_S(61); t += CK_TICK_NS) tick(&c, &s, t, 4, 0);
  CHECK(!c.hold);
  for (; t <= CK_S(63); t += CK_TICK_NS) tick(&c, &s, t, 4, 0);
  CHECK(c.hold);
  CHECK(!s.heater_on);               /* in-progress defrost ended */
  for (; t <= CK_S(400); t += CK_TICK_NS) tick(&c, &s, t, 6.0f, -5);
  CHECK(s.compressor_on);            /* keeps cooling */
  CHECK(c.setpoint_c == 3.0f);       /* holds last-known-good setpoint */
  CHECK_EQ_INT(ck_ctl_command(&c, &d, t), EPERM); /* never starts a defrost while held */
  ck_ctl_command(&c, &hb, t);
  CHECK(!c.hold);
  CHECK_EQ_INT(ck_ctl_command(&c, &d, t), 0);
}

static void legacy_has_no_hold(void) {
  ck_ctl_t c; ck_case_state_t s = {0};
  ck_ctl_init(&c, 1, CASE_CHILLED, false, 0);
  for (uint64_t t = 0; t < CK_S(30); t += CK_TICK_NS) tick(&c, &s, t, 4, 0);
  CHECK(!c.hold);
}

static void raw_override(void) {
  ck_ctl_t c; ck_case_state_t s = {0};
  ck_ctl_init(&c, 1, CASE_CHILLED, false, 0);
  ck_case_cmd_t r = {.type = CASE_CMD_RAW_ACTUATORS, .raw_override = 1, .raw_heater = 1};
  ck_ctl_command(&c, &r, 0);
  tick(&c, &s, 0, 10, 0);
  CHECK(s.heater_on && !s.compressor_on);
}

void suite_control(void) {
  RUN(thermostat_hysteresis);
  RUN(defrost_sequence_and_duration);
  RUN(termination_ends_heat);
  RUN(stop_defrost);
  RUN(g6_hold_when_governor_lost);
  RUN(legacy_has_no_hold);
  RUN(raw_override);
}
