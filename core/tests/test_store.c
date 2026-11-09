/* SPDX-License-Identifier: Apache-2.0
 * End-to-end on the in-process store: A1 legacy vs protected, A3, A4/G6, keyconsole. */
#include <errno.h>

#include "ck_test.h"
#include "coldkeep.h"

static ck_case_type_t types[8] = {CASE_CHILLED, CASE_CHILLED, CASE_CHILLED, CASE_CHILLED,
                                  CASE_FROZEN,  CASE_FROZEN,  CASE_COLDROOM, CASE_COLDROOM};

static void run_for(ck_store_t *s, float seconds) {
  uint64_t end = s->now + (uint64_t)(seconds * 1000.0f) * 1000000ULL;
  while (s->now < end) ck_store_step(s);
}

/* A1, paced at one request per second (under the broker's 2/s rate limit) */
static void a1(ck_store_t *s) {
  ck_decision_t d;
  ck_store_login(s, 1, "admin", "admin");
  for (uint16_t c = 1; c <= s->n; c++) { ck_store_mgmt(s, 1, CMD_START_DEFROST, c, 86400, &d); run_for(s, 1); }
  for (uint16_t c = 1; c <= s->n; c++) {
    ck_store_mgmt(s, 1, CMD_SET_SETPOINT, c, s->plant.st[c - 1].setpoint_c + 25.0f, &d);
    run_for(s, 1);
  }
  ck_store_mgmt(s, 1, CMD_DISABLE_ALARMS, 0, 0, &d);
}

static void a1_legacy_fails_protected_holds(void) {
  static ck_store_t L, P;
  ck_store_init(&L, MODE_LEGACY, 7, 8, types, 20.0f, 0.1f);
  ck_store_init(&P, MODE_PROTECTED, 7, 8, types, 20.0f, 0.1f);
  run_for(&L, 600); run_for(&P, 600);
  a1(&L); a1(&P);
  run_for(&L, 3000); run_for(&P, 3000);
  CHECK_MSG(ck_metrics_total_minutes_above(&L.m) > 10.0, "legacy minutes above %.1f",
            ck_metrics_total_minutes_above(&L.m));
  CHECK_MSG(ck_metrics_total_minutes_above(&P.m) == 0.0, "protected minutes above %.1f",
            ck_metrics_total_minutes_above(&P.m));
  CHECK(!L.alarms_enabled);
  for (int i = 0; i < 8; i++) CHECK(P.gov.c[i].alarms_enabled);
  CHECK(P.m.blocked > 0);
  CHECK_EQ_INT(P.gov.env.version, 1);
  CHECK(ck_keyq_pending(&P.keyq) >= 1);
  /* G3 concurrency: at most 2 of the 8 manual defrosts went through */
  uint32_t started = 0;
  for (int i = 0; i < 8; i++) started += P.ctl[i].defrosts_started;
  CHECK_MSG(started <= 2, "started %u", started);
}

static void auth_required(void) {
  static ck_store_t P;
  ck_store_init(&P, MODE_PROTECTED, 1, 8, types, 20.0f, 0.1f);
  ck_decision_t d;
  CHECK_EQ_INT(ck_store_mgmt(&P, 1, CMD_SET_SETPOINT, 1, 4, &d), VERDICT_DENY);
  CHECK_EQ_INT(d.rule_id, CK_RULE_AUTH);
  CHECK(!ck_store_login(&P, 1, "admin", "wrong"));
  CHECK(ck_store_login(&P, 1, "admin", "admin"));
  CHECK_EQ_INT(ck_store_mgmt(&P, 1, CMD_SET_SETPOINT, 1, 4, &d), VERDICT_ALLOW);
}

static void a3_compromised_process(void) {
  static ck_store_t L, P;
  ck_store_init(&L, MODE_LEGACY, 3, 8, types, 20.0f, 0.1f);
  ck_store_init(&P, MODE_PROTECTED, 3, 8, types, 20.0f, 0.1f);
  const char *acts[] = {"devctl_case", "connect_governor", "regain_abilities", "spawn", "tamper_audit"};
  for (int i = 0; i < 5; i++) {
    CHECK_EQ_INT(ck_store_compromised_attempt(&L, acts[i]), 0);
    int e = ck_store_compromised_attempt(&P, acts[i]);
    CHECK_MSG(e == EPERM || e == EACCES, "%s -> %d", acts[i], e);
  }
  run_for(&L, 3600); run_for(&P, 3600);
  CHECK(L.m.c[0].s_above > 0);          /* raw heater on case 1 in legacy */
  CHECK(P.m.c[0].s_above == 0);
}

static void a4_governor_crash_g6(void) {
  static ck_store_t P;
  ck_store_init(&P, MODE_PROTECTED, 5, 8, types, 20.0f, 0.1f);
  ck_store_login(&P, 1, "admin", "admin");
  run_for(&P, 60);
  ck_decision_t d;
  ck_store_mgmt(&P, 1, CMD_START_DEFROST, 1, 2700, &d);
  run_for(&P, 30);
  CHECK(P.plant.st[0].defrost_on);
  ck_store_kill_governor(&P, 30.0f);
  ck_store_mgmt(&P, 1, CMD_START_DEFROST, 2, 2700, &d);
  CHECK_EQ_INT(d.rule_id, CK_RULE_UNAVAILABLE);
  run_for(&P, 5);
  for (int i = 0; i < 8; i++) CHECK(P.ctl[i].hold);
  CHECK(!P.plant.st[0].heater_on);
  run_for(&P, 30);
  CHECK(P.gov_up);
  for (int i = 0; i < 8; i++) CHECK(!P.ctl[i].hold);
  CHECK_EQ_INT(P.ctl[1].defrosts_started, 0);
}

static void keyconsole_is_only_way(void) {
  static ck_store_t P;
  ck_store_init(&P, MODE_PROTECTED, 9, 8, types, 20.0f, 0.1f);
  ck_store_login(&P, 1, "admin", "admin");
  ck_decision_t d;
  CHECK_EQ_INT(ck_store_mgmt(&P, 1, CMD_CHANGE_ENVELOPE, 1, 4.0f, &d), VERDICT_NEEDS_KEY);
  CHECK_EQ_INT(P.gov.env.version, 1);
  CHECK_EQ_INT(ck_keyq_pending(&P.keyq), 1);
  CHECK(ck_store_key_approve(&P, P.keyq.r[0].req_id));
  CHECK_EQ_INT(P.gov.env.version, 2);
  CHECK(!ck_store_key_approve(&P, P.keyq.r[0].req_id)); /* not twice */
}

static void same_seed_same_run(void) {
  static ck_store_t a, b;
  ck_store_init(&a, MODE_PROTECTED, 11, 8, types, 20.0f, 0.1f);
  ck_store_init(&b, MODE_PROTECTED, 11, 8, types, 20.0f, 0.1f);
  run_for(&a, 900); run_for(&b, 900);
  CHECK(memcmp(a.plant.st, b.plant.st, sizeof a.plant.st) == 0);
  /* shopper load identical across modes */
  static ck_store_t l;
  ck_store_init(&l, MODE_LEGACY, 11, 8, types, 20.0f, 0.1f);
  run_for(&l, 900);
  int same = 1;
  for (int i = 0; i < 8; i++) same &= l.plant.door_rng[i].s == a.plant.door_rng[i].s;
  CHECK(same);
}

void suite_store(void) {
  RUN(a1_legacy_fails_protected_holds);
  RUN(auth_required);
  RUN(a3_compromised_process);
  RUN(a4_governor_crash_g6);
  RUN(keyconsole_is_only_way);
  RUN(same_seed_same_run);
}
