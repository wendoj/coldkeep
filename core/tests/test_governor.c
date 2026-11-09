/* SPDX-License-Identifier: Apache-2.0
 * Governor rules G1–G5, positive and negative. (G6 is tested in test_control.c
 * and end-to-end in test_store.c.) */
#include <math.h>

#include "ck_test.h"
#include "coldkeep.h"

#define N 8
static ck_case_type_t types[N] = {CASE_CHILLED, CASE_CHILLED, CASE_CHILLED, CASE_CHILLED,
                                  CASE_FROZEN,  CASE_FROZEN,  CASE_COLDROOM, CASE_COLDROOM};
static ck_gov_t g;
static ck_case_state_t st[N];
static uint64_t next_id = 1;

static void setup(void) {
  ck_envelope_t e; ck_envelope_default(&e);
  ck_gov_init(&g, &e, N, types, 0);
  for (int i = 0; i < N; i++) ck_thermal_init(&st[i], (uint16_t)(i + 1), types[i], 20.0f);
}

static ck_cmd_t mk(ck_cmd_type_t t, uint16_t case_id, float v, const char *origin) {
  ck_cmd_t c = {0};
  c.id = next_id++;
  ck_strlcpy(c.origin, origin, sizeof c.origin);
  ck_strlcpy(c.user, "admin", sizeof c.user);
  c.type = t; c.case_id = case_id; c.value = v;
  return c;
}

static ck_decision_t decide(ck_cmd_type_t t, uint16_t case_id, float v, uint64_t now, ck_gov_action_t *act) {
  ck_cmd_t c = mk(t, case_id, v, "mgmt");
  ck_decision_t d; ck_gov_action_t a;
  ck_gov_decide(&g, &c, st, now, &d, act ? act : &a);
  return d;
}

static void g1_setpoint_band(void) {
  setup();
  ck_gov_action_t a;
  ck_decision_t d = decide(CMD_SET_SETPOINT, 1, 4.0f, 0, &a);
  CHECK_EQ_INT(d.verdict, VERDICT_ALLOW);
  CHECK(a.cmd.type == CASE_CMD_SET_SETPOINT && a.cmd.setpoint_c == 4.0f);
  d = decide(CMD_SET_SETPOINT, 2, 6.0f, 0, &a);           /* 1 °C above band: clamp */
  CHECK_EQ_INT(d.verdict, VERDICT_CLAMP);
  CHECK_EQ_INT(d.rule_id, CK_RULE_G1);
  CHECK(a.cmd.setpoint_c == 5.0f && d.applied_value == 5.0f);
  d = decide(CMD_SET_SETPOINT, 3, 28.0f, 0, &a);          /* A1-style +25: far outside */
  CHECK_EQ_INT(d.verdict, VERDICT_DENY);
  CHECK_EQ_INT(d.rule_id, CK_RULE_G1);
  d = decide(CMD_SET_SETPOINT, 5, -26.0f, 0, &a);         /* clamps to -25, but step 3 > 2 */
  CHECK_EQ_INT(d.verdict, VERDICT_DENY);
  CHECK_EQ_INT(d.rule_id, CK_RULE_G2);
  d = decide(CMD_SET_SETPOINT, 5, -24.0f, 0, &a);
  CHECK_EQ_INT(d.verdict, VERDICT_ALLOW);
  d = decide(CMD_SET_SETPOINT, 5, -26.0f, 0, &a);         /* frozen below band: clamp */
  CHECK_EQ_INT(d.verdict, VERDICT_CLAMP);
  CHECK(a.cmd.setpoint_c == -25.0f);
  d = decide(CMD_SET_SETPOINT, 6, -19.0f, 0, &a);         /* frozen above band (-20): clamp */
  CHECK_EQ_INT(d.verdict, VERDICT_CLAMP);
  CHECK(a.cmd.setpoint_c == -20.0f);
}

static void g2_rate_and_step(void) {
  setup();
  ck_decision_t d = decide(CMD_SET_SETPOINT, 1, 0.5f, 0, NULL);   /* 3 -> 0.5: step 2.5 */
  CHECK_EQ_INT(d.verdict, VERDICT_DENY);
  CHECK_EQ_INT(d.rule_id, CK_RULE_G2);
  d = decide(CMD_SET_SETPOINT, 1, 1.0f, CK_S(10), NULL);          /* step 2: ok */
  CHECK_EQ_INT(d.verdict, VERDICT_ALLOW);
  d = decide(CMD_SET_SETPOINT, 1, 2.0f, CK_S(20), NULL);          /* 2nd change: ok */
  CHECK_EQ_INT(d.verdict, VERDICT_ALLOW);
  d = decide(CMD_SET_SETPOINT, 1, 3.0f, CK_S(30), NULL);          /* 3rd in the hour */
  CHECK_EQ_INT(d.verdict, VERDICT_DENY);
  CHECK_EQ_INT(d.rule_id, CK_RULE_G2);
  d = decide(CMD_SET_SETPOINT, 1, 2.0f, CK_S(40), NULL);          /* no-op: not a change */
  CHECK_EQ_INT(d.verdict, VERDICT_ALLOW);
  d = decide(CMD_SET_SETPOINT, 1, 3.0f, CK_S(3611), NULL);        /* window slid */
  CHECK_EQ_INT(d.verdict, VERDICT_ALLOW);
  /* per case: case 2 unaffected */
  d = decide(CMD_SET_SETPOINT, 2, 4.0f, CK_S(41), NULL);
  CHECK_EQ_INT(d.verdict, VERDICT_ALLOW);
}

static void g3_defrost_envelope(void) {
  setup();
  ck_gov_action_t a;
  ck_decision_t d = decide(CMD_START_DEFROST, 1, 86400, 0, &a);   /* 24 h -> 45 min */
  CHECK_EQ_INT(d.verdict, VERDICT_CLAMP);
  CHECK_EQ_INT(d.rule_id, CK_RULE_G3);
  CHECK_EQ_INT(a.cmd.duration_s, 45 * 60);
  CHECK(a.cmd.term_c < CK_TERM_NONE);                             /* always terminated */
  st[0].defrost_on = 1;
  d = decide(CMD_START_DEFROST, 1, 600, CK_S(1), &a);             /* already defrosting */
  CHECK_EQ_INT(d.verdict, VERDICT_DENY);
  d = decide(CMD_START_DEFROST, 5, 86400, CK_S(1), &a);           /* frozen: 60 min max */
  CHECK_EQ_INT(d.verdict, VERDICT_CLAMP);
  CHECK_EQ_INT(a.cmd.duration_s, 60 * 60);
  st[4].defrost_on = 1;
  d = decide(CMD_START_DEFROST, 2, 600, CK_S(2), &a);             /* 25 % of 8 = 2 at once */
  CHECK_EQ_INT(d.verdict, VERDICT_DENY);
  CHECK_EQ_INT(d.rule_id, CK_RULE_G3);
  st[0].defrost_on = 0; st[4].defrost_on = 0;
  d = decide(CMD_START_DEFROST, 1, 600, CK_S(3600), &a);          /* < 4 h since last manual */
  CHECK_EQ_INT(d.verdict, VERDICT_DENY);
  CHECK_EQ_INT(d.rule_id, CK_RULE_G3);
  d = decide(CMD_START_DEFROST, 1, 600, CK_S(4 * 3600 + 1), &a);  /* gap respected */
  CHECK_EQ_INT(d.verdict, VERDICT_ALLOW);
  CHECK_EQ_INT(a.cmd.duration_s, 600);
  /* scheduled defrosts ignore the manual gap but not concurrency */
  ck_cmd_t c = mk(CMD_START_DEFROST, 1, 1800, "schedule");
  ck_decision_t dd;
  CHECK(ck_gov_decide(&g, &c, st, CK_S(4 * 3600 + 2), &dd, &a));
}

static void g4_product_override(void) {
  setup();
  /* raise setpoint legitimately, then product passes warning */
  decide(CMD_SET_SETPOINT, 1, 5.0f, 0, NULL);
  st[0].defrost_on = 1;
  st[0].product_c = 6.5f;
  ck_gov_action_t acts[32]; ck_decision_t decs[32];
  int k = ck_gov_tick(&g, st, CK_S(10), acts, decs, 32);
  int saw_sp = 0, saw_stop = 0;
  for (int i = 0; i < k; i++) {
    if (acts[i].case_id != 1) continue;
    CHECK_EQ_INT(decs[i].rule_id, CK_RULE_G4);
    if (acts[i].cmd.type == CASE_CMD_SET_SETPOINT) { saw_sp = 1; CHECK(acts[i].cmd.setpoint_c == 3.0f); }
    if (acts[i].cmd.type == CASE_CMD_STOP_DEFROST) saw_stop = 1;
  }
  CHECK(saw_sp && saw_stop);                       /* LKG = default 3.0 restored, defrost ended */
  CHECK(g.c[0].g4_active);
  st[0].defrost_on = 0;
  ck_decision_t d = decide(CMD_SET_SETPOINT, 1, 4.0f, CK_S(20), NULL);   /* raises blocked */
  CHECK_EQ_INT(d.verdict, VERDICT_DENY);
  CHECK_EQ_INT(d.rule_id, CK_RULE_G4);
  d = decide(CMD_START_DEFROST, 1, 600, CK_S(20), NULL);                 /* defrost blocked */
  CHECK_EQ_INT(d.verdict, VERDICT_DENY);
  d = decide(CMD_SET_SETPOINT, 1, 2.0f, CK_S(30), NULL);                 /* lowering ok */
  CHECK_EQ_INT(d.verdict, VERDICT_ALLOW);
  st[0].product_c = 4.9f;                                                /* clears below warn-1 */
  ck_gov_tick(&g, st, CK_S(40), acts, decs, 32);
  CHECK(!g.c[0].g4_active);
  /* negative: below warning, no G4 action */
  setup();
  k = ck_gov_tick(&g, st, CK_S(1), acts, decs, 32);
  CHECK_EQ_INT(k, 0);
}

static void g4_lkg_tracks_good_setpoint(void) {
  setup();
  decide(CMD_SET_SETPOINT, 1, 4.0f, 0, NULL);
  ck_gov_action_t acts[32]; ck_decision_t decs[32];
  for (uint64_t t = 0; t <= CK_S(3700); t += CK_S(60)) ck_gov_tick(&g, st, t, acts, decs, 32);
  CHECK(g.c[0].lkg_setpoint == 4.0f);
}

static void g5_dangerous_needs_key(void) {
  setup();
  ck_cmd_type_t dangerous[] = {CMD_DISABLE_ALARMS, CMD_SET_DEFROST_SCHEDULE, CMD_CHANGE_ENVELOPE, CMD_RAW_WRITE};
  for (size_t i = 0; i < 4; i++) {
    ck_gov_action_t a;
    ck_decision_t d = decide(dangerous[i], 1, 30.0f, 0, &a);
    CHECK_EQ_INT(d.verdict, VERDICT_NEEDS_KEY);
    CHECK_EQ_INT(d.rule_id, CK_RULE_G5);
    CHECK_EQ_INT(a.cmd.type, 0);
  }
  ck_decision_t d = decide(CMD_SET_FANS, 1, 0.0f, 0, NULL);
  CHECK_EQ_INT(d.verdict, VERDICT_NEEDS_KEY);
  d = decide(CMD_SET_FANS, 1, 1.0f, 0, NULL);
  CHECK_EQ_INT(d.verdict, VERDICT_ALLOW);
  /* claimed origin does not matter: "keyconsole" through decide() is still refused */
  ck_cmd_t c = mk(CMD_CHANGE_ENVELOPE, 1, 30.0f, "keyconsole");
  ck_gov_action_t a;
  ck_gov_decide(&g, &c, st, 0, &d, &a);
  CHECK_EQ_INT(d.verdict, VERDICT_NEEDS_KEY);
  CHECK_EQ_INT(g.env.version, 1);
  /* without an approval id nothing changes */
  CHECK(!ck_gov_apply_approved(&g, &c, 0, &d));
  CHECK_EQ_INT(g.env.version, 1);
  /* with one, the envelope changes and is counted */
  c = mk(CMD_CHANGE_ENVELOPE, 1, 4.0f, "keyconsole");
  CHECK(ck_gov_apply_approved(&g, &c, 17, &d));
  CHECK_EQ_INT(g.env.version, 2);
  CHECK(g.env.t[CASE_CHILLED].sp_max == 4.0f);
  CHECK_EQ_INT(g.key_approvals, 1);
}

static void stop_defrost_always_allowed(void) {
  setup();
  ck_decision_t d = decide(CMD_STOP_DEFROST, 3, 0, 0, NULL);
  CHECK_EQ_INT(d.verdict, VERDICT_ALLOW);
}

static void invalid_inputs(void) {
  setup();
  ck_decision_t d = decide(CMD_SET_SETPOINT, 0, 3, 0, NULL);
  CHECK_EQ_INT(d.verdict, VERDICT_DENY);
  d = decide(CMD_SET_SETPOINT, 99, 3, 0, NULL);
  CHECK_EQ_INT(d.verdict, VERDICT_DENY);
  d = decide(CMD_SET_SETPOINT, 1, NAN, 0, NULL);
  CHECK_EQ_INT(d.verdict, VERDICT_DENY);
}

static void schedule_runs(void) {
  setup();
  ck_gov_action_t acts[32]; ck_decision_t decs[32];
  int starts = 0;
  for (uint64_t t = 0; t <= CK_S(6 * 3600 + 120); t += CK_S(1)) {
    int k = ck_gov_tick(&g, st, t, acts, decs, 32);
    for (int i = 0; i < k; i++) if (acts[i].cmd.type == CASE_CMD_START_DEFROST) starts++;
  }
  CHECK_EQ_INT(starts, 1); /* first chilled case at 6 h */
}

static void status_text(void) {
  setup();
  decide(CMD_START_DEFROST, 1, 86400, 0, NULL);
  char buf[4096];
  int n = ck_gov_status_text(&g, buf, sizeof buf);
  CHECK(n > 0 && strstr(buf, "envelope v1") && strstr(buf, "CLAMP G3"));
}

void suite_governor(void) {
  RUN(g1_setpoint_band);
  RUN(g2_rate_and_step);
  RUN(g3_defrost_envelope);
  RUN(g4_product_override);
  RUN(g4_lkg_tracks_good_setpoint);
  RUN(g5_dangerous_needs_key);
  RUN(stop_defrost_always_allowed);
  RUN(invalid_inputs);
  RUN(schedule_runs);
  RUN(status_text);
}
