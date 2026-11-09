/* SPDX-License-Identifier: Apache-2.0 — broker schema, classification, rate limit, key queue */
#include <math.h>

#include "ck_test.h"
#include "coldkeep.h"

static ck_case_type_t types[4] = {CASE_CHILLED, CASE_CHILLED, CASE_FROZEN, CASE_COLDROOM};
static ck_envelope_t env;
static ck_broker_t b;

static void setup(void) {
  ck_envelope_default(&env);
  ck_broker_init(&b, &env, 4, types);
}

static ck_cmd_t mk(ck_cmd_type_t t, uint16_t case_id, float v) {
  ck_cmd_t c = {0};
  ck_strlcpy(c.origin, "mgmt", sizeof c.origin);
  ck_strlcpy(c.user, "admin", sizeof c.user);
  c.type = t; c.case_id = case_id; c.value = v;
  return c;
}

static void schema(void) {
  setup();
  char why[96];
  ck_cmd_t c = mk(CMD_SET_SETPOINT, 1, 3.0f);
  CHECK_EQ_INT(ck_broker_validate(&b, &c, why, sizeof why), 0);
  c.case_id = 5; CHECK(ck_broker_validate(&b, &c, why, sizeof why) != 0);
  c.case_id = 0; CHECK(ck_broker_validate(&b, &c, why, sizeof why) != 0);
  c = mk(CMD_DISABLE_ALARMS, 0, 0); CHECK_EQ_INT(ck_broker_validate(&b, &c, why, sizeof why), 0);
  c = mk(CMD_SET_SETPOINT, 1, NAN); CHECK(ck_broker_validate(&b, &c, why, sizeof why) != 0);
  c = mk(CMD_SET_SETPOINT, 1, 100.0f); CHECK(ck_broker_validate(&b, &c, why, sizeof why) != 0);
  c = mk(CMD_START_DEFROST, 1, 0.0f); CHECK(ck_broker_validate(&b, &c, why, sizeof why) != 0);
  c = mk(CMD_START_DEFROST, 1, 86400.0f); CHECK_EQ_INT(ck_broker_validate(&b, &c, why, sizeof why), 0);
  c = mk(CMD_SET_FANS, 1, 0.5f); CHECK(ck_broker_validate(&b, &c, why, sizeof why) != 0);
  c = mk((ck_cmd_type_t)42, 1, 0); CHECK(ck_broker_validate(&b, &c, why, sizeof why) != 0);
  c = mk(CMD_STATUS, 1, 0); memset(c.user, 'A', sizeof c.user); /* unterminated */
  CHECK(ck_broker_validate(&b, &c, why, sizeof why) != 0);
}

static void classify(void) {
  setup();
  ck_cmd_t c = mk(CMD_STATUS, 0, 0);            CHECK_EQ_INT(ck_broker_classify(&b, &c), CLASS_READ);
  c = mk(CMD_SET_SETPOINT, 1, 3.0f);            CHECK_EQ_INT(ck_broker_classify(&b, &c), CLASS_ROUTINE);
  c = mk(CMD_SET_SETPOINT, 1, 4.8f);            CHECK_EQ_INT(ck_broker_classify(&b, &c), CLASS_RISKY);
  c = mk(CMD_SET_SETPOINT, 1, 28.0f);           CHECK_EQ_INT(ck_broker_classify(&b, &c), CLASS_RISKY);
  c = mk(CMD_START_DEFROST, 1, 600);            CHECK_EQ_INT(ck_broker_classify(&b, &c), CLASS_RISKY);
  ck_strlcpy(c.origin, "schedule", sizeof c.origin);
  CHECK_EQ_INT(ck_broker_classify(&b, &c), CLASS_ROUTINE);
  c = mk(CMD_STOP_DEFROST, 1, 0);               CHECK_EQ_INT(ck_broker_classify(&b, &c), CLASS_ROUTINE);
  c = mk(CMD_SET_FANS, 1, 0);                   CHECK_EQ_INT(ck_broker_classify(&b, &c), CLASS_DANGEROUS);
  c = mk(CMD_DISABLE_ALARMS, 0, 0);             CHECK_EQ_INT(ck_broker_classify(&b, &c), CLASS_DANGEROUS);
  c = mk(CMD_CHANGE_ENVELOPE, 1, 9);            CHECK_EQ_INT(ck_broker_classify(&b, &c), CLASS_DANGEROUS);
  c = mk(CMD_RAW_WRITE, 1, 1);                  CHECK_EQ_INT(ck_broker_classify(&b, &c), CLASS_DANGEROUS);
  c = mk(CMD_SET_DEFROST_SCHEDULE, 1, 2);       CHECK_EQ_INT(ck_broker_classify(&b, &c), CLASS_DANGEROUS);
}

static void pipeline(void) {
  setup();
  ck_decision_t d;
  ck_cmd_t c = mk(CMD_SET_SETPOINT, 1, 3.0f);
  CHECK_EQ_INT(ck_broker_check(&b, 1, &c, 0, &d), BROKER_FORWARD);
  c = mk(CMD_DISABLE_ALARMS, 0, 0);
  CHECK_EQ_INT(ck_broker_check(&b, 1, &c, 0, &d), BROKER_NEEDS_KEY);
  CHECK_EQ_INT(d.verdict, VERDICT_NEEDS_KEY);
  CHECK_EQ_INT(d.rule_id, CK_RULE_G5);
  c = mk(CMD_SET_SETPOINT, 9, 3.0f);
  CHECK_EQ_INT(ck_broker_check(&b, 1, &c, 0, &d), BROKER_REJECT);
  CHECK_EQ_INT(d.rule_id, CK_RULE_SCHEMA);
  c = mk(CMD_STATUS, 0, 0);
  CHECK_EQ_INT(ck_broker_check(&b, 1, &c, 0, &d), BROKER_READ);
}

static void rate_limit(void) {
  setup();
  ck_decision_t d;
  ck_cmd_t c = mk(CMD_STATUS, 0, 0);
  int ok = 0, limited = 0;
  for (int i = 0; i < 1000; i++) {   /* 1000 requests in the same instant */
    ck_broker_out_t o = ck_broker_check(&b, 1, &c, CK_S(5), &d);
    if (o == BROKER_REJECT && d.rule_id == CK_RULE_RATE) limited++; else ok++;
  }
  CHECK_EQ_INT(ok, 10);             /* burst */
  CHECK_EQ_INT(limited, 990);
  /* another session is independent */
  CHECK_EQ_INT(ck_broker_check(&b, 2, &c, CK_S(5), &d), BROKER_READ);
  /* refill: 2/s */
  CHECK_EQ_INT(ck_broker_check(&b, 1, &c, CK_S(6), &d), BROKER_READ);
  CHECK_EQ_INT(ck_broker_check(&b, 1, &c, CK_S(6), &d), BROKER_READ);
  CHECK_EQ_INT(ck_broker_check(&b, 1, &c, CK_S(6), &d), BROKER_REJECT);
}

static void user_is_untrusted(void) {
  setup();
  ck_decision_t d;
  ck_cmd_t c = mk(CMD_CHANGE_ENVELOPE, 1, 20);
  ck_strlcpy(c.user, "technician", sizeof c.user);
  ck_strlcpy(c.origin, "keyconsole", sizeof c.origin);
  CHECK_EQ_INT(ck_broker_check(&b, 1, &c, 0, &d), BROKER_NEEDS_KEY);
}

static void keyq(void) {
  ck_keyq_t q; ck_keyq_init(&q);
  ck_cmd_t c = mk(CMD_DISABLE_ALARMS, 0, 0);
  uint64_t id = ck_keyq_push(&q, &c);
  CHECK(id == 1);
  CHECK_EQ_INT(ck_keyq_pending(&q), 1);
  CHECK(ck_keyq_find(&q, id) != NULL);
  CHECK(ck_keyq_find(&q, 99) == NULL);
  for (int i = 0; i < CK_KEYQ_MAX + 5; i++) ck_keyq_push(&q, &c);
  CHECK_EQ_INT(ck_keyq_pending(&q), CK_KEYQ_MAX);
  CHECK(q.dropped == 6);
}

void suite_broker(void) {
  RUN(schema);
  RUN(classify);
  RUN(pipeline);
  RUN(rate_limit);
  RUN(user_is_untrusted);
  RUN(keyq);
}
