/* SPDX-License-Identifier: Apache-2.0 — mgmtd protocol: parsing, errors, fuzzing, replies */
#include <math.h>

#include "ck_test.h"
#include "coldkeep.h"

static int parse(const char *s, ck_req_t *r) { return ck_proto_parse(s, strlen(s), r); }

static void valid_requests(void) {
  ck_req_t r;
  CHECK_EQ_INT(parse("{\"op\":\"login\",\"user\":\"admin\",\"pass\":\"admin\"}", &r), 0);
  CHECK(r.kind == REQ_LOGIN && !strcmp(r.user, "admin") && !strcmp(r.pass, "admin"));
  CHECK_EQ_INT(parse("  {\"op\" : \"status\"}  \r\n", &r), 0);
  CHECK(r.kind == REQ_STATUS);
  CHECK_EQ_INT(parse("{\"op\":\"set_setpoint\",\"case\":3,\"value\":-4.5}", &r), 0);
  CHECK(r.kind == REQ_CMD && r.cmd == CMD_SET_SETPOINT && r.case_id == 3 && r.value == -4.5f);
  CHECK_EQ_INT(parse("{\"op\":\"start_defrost\",\"case\":1,\"seconds\":86400}", &r), 0);
  CHECK(r.cmd == CMD_START_DEFROST && r.value == 86400.0f);
  CHECK_EQ_INT(parse("{\"op\":\"start_defrost\",\"case\":1,\"value\":60}", &r), 0); /* generic key */
  CHECK(r.value == 60.0f);
  CHECK_EQ_INT(parse("{\"op\":\"set_fans\",\"case\":2,\"on\":false}", &r), 0);
  CHECK(r.cmd == CMD_SET_FANS && r.value == 0.0f);
  CHECK_EQ_INT(parse("{\"op\":\"set_fans\",\"case\":2,\"on\":true}", &r), 0);
  CHECK(r.value == 1.0f);
  CHECK_EQ_INT(parse("{\"op\":\"disable_alarms\"}", &r), 0);
  CHECK(r.cmd == CMD_DISABLE_ALARMS && r.case_id == 0);
  CHECK_EQ_INT(parse("{\"op\":\"set_defrost_schedule\",\"case\":1,\"hours\":2}", &r), 0);
  CHECK(r.cmd == CMD_SET_DEFROST_SCHEDULE && r.value == 2.0f);
  CHECK_EQ_INT(parse("{\"op\":\"debug_exec\",\"action\":\"spawn\"}", &r), 0);
  CHECK(r.kind == REQ_DEBUG_EXEC && !strcmp(r.action, "spawn"));
  CHECK_EQ_INT(parse("{\"op\":\"stop_defrost\",\"case\":1,\"extra\":[1]}", &r), -1); /* arrays rejected */
  CHECK_EQ_INT(parse("{\"op\":\"stop_defrost\",\"case\":1,\"note\":\"x\",\"n\":null}", &r), 0); /* unknown ignored */
  CHECK_EQ_INT(parse("{\"user\":\"a\\u0041\\\"\",\"pass\":\"p\",\"op\":\"login\"}", &r), 0);
  CHECK(!strcmp(r.user, "aA\""));
}

static void invalid_requests(void) {
  static const char *bad[] = {
    "", "[]", "{}", "{", "{\"op\":\"status\"", "{\"op\":\"status\"}x", "{\"op\":status}",
    "{\"op\":\"nope\"}", "{\"op\":\"set_setpoint\",\"value\":1}", "{\"op\":\"set_setpoint\",\"case\":1}",
    "{\"op\":\"set_setpoint\",\"case\":1.5,\"value\":1}", "{\"op\":\"set_setpoint\",\"case\":-1,\"value\":1}",
    "{\"op\":\"set_setpoint\",\"case\":1,\"value\":\"4\"}", "{\"op\":\"set_setpoint\",\"case\":1,\"value\":1e999}",
    "{\"op\":\"set_setpoint\",\"case\":1,\"value\":01}", "{\"op\":\"set_setpoint\",\"case\":1,\"value\":-}",
    "{\"op\":\"set_setpoint\",\"case\":1,\"value\":NaN}", "{\"op\":\"login\",\"user\":\"admin\"}",
    "{\"op\":\"status\",\"x\":{\"y\":1}}", "{\"op\":\"st\\u0000atus\"}", "{\"op\":\"a\tb\"}",
    "{\"op\":\"login\",\"user\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"pass\":\"x\"}",
    "{\"op\":1}", "{\"op\":\"status\",}", "{,\"op\":\"status\"}", "{\"op\":\"debug_exec\"}",
  };
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    ck_req_t r;
    int rc = parse(bad[i], &r);
    CHECK_MSG(rc == -1 && r.kind == REQ_INVALID && r.error[0], "accepted: %s", bad[i]);
  }
  char longline[CK_PROTO_MAX_LINE + 10];
  memset(longline, ' ', sizeof longline);
  ck_req_t r;
  CHECK_EQ_INT(ck_proto_parse(longline, sizeof longline, &r), -1);
}

static void fuzz_no_crash(void) {
  /* random bytes and mutated valid lines: must never crash or overrun (run under ASan) */
  static const char *seeds[] = {
    "{\"op\":\"set_setpoint\",\"case\":1,\"value\":4.5}",
    "{\"op\":\"login\",\"user\":\"admin\",\"pass\":\"admin\"}",
    "{\"op\":\"start_defrost\",\"case\":1,\"seconds\":86400}",
  };
  ck_rng_t rng; ck_rng_seed(&rng, 77);
  char buf[256];
  int ok_count = 0;
  for (int i = 0; i < 200000; i++) {
    size_t len;
    if (i & 1) {
      len = ck_rng_below(&rng, sizeof buf);
      for (size_t k = 0; k < len; k++) buf[k] = (char)ck_rng_below(&rng, 256);
    } else {
      const char *s = seeds[ck_rng_below(&rng, 3)];
      len = strlen(s);
      memcpy(buf, s, len);
      int muts = 1 + (int)ck_rng_below(&rng, 4);
      for (int m = 0; m < muts; m++) buf[ck_rng_below(&rng, (uint32_t)len)] = (char)ck_rng_below(&rng, 128);
      if (ck_rng_below(&rng, 4) == 0) len = ck_rng_below(&rng, (uint32_t)len + 1);
    }
    ck_req_t r;
    if (ck_proto_parse(buf, len, &r) == 0) {
      ok_count++;
      if (r.kind == REQ_CMD) CHECK(isfinite(r.value));
    } else {
      CHECK(r.kind == REQ_INVALID);
    }
  }
  CHECK(ok_count > 0);
}

static void replies(void) {
  ck_decision_t d = {0};
  d.cmd.id = 9; d.cmd.type = CMD_START_DEFROST; d.cmd.case_id = 1;
  d.verdict = VERDICT_CLAMP; d.rule_id = CK_RULE_G3; d.applied_value = 2700;
  ck_strlcpy(d.reason, "max \"defrost\" 45 min", sizeof d.reason);
  char buf[512];
  ck_proto_reply_decision(buf, sizeof buf, &d);
  CHECK(!strcmp(buf, "{\"ok\":true,\"id\":9,\"cmd\":\"start_defrost\",\"case\":1,\"verdict\":\"CLAMP\","
                     "\"rule\":\"G3\",\"applied\":2700.000,\"reason\":\"max \\\"defrost\\\" 45 min\"}"));
  d.applied_value = NAN;
  ck_proto_reply_decision(buf, sizeof buf, &d);
  CHECK(strstr(buf, "\"applied\":null") != NULL);
  ck_proto_reply_error(buf, sizeof buf, "bad\nline");
  CHECK(!strcmp(buf, "{\"ok\":false,\"error\":\"bad\\u000aline\"}"));
  char small[16];
  int n = ck_proto_reply_ok(small, sizeof small, "a long message that does not fit");
  CHECK(n == 15 && strlen(small) == 15);
}

void suite_proto(void) {
  RUN(valid_requests);
  RUN(invalid_requests);
  RUN(fuzz_no_crash);
  RUN(replies);
}
