/* SPDX-License-Identifier: Apache-2.0 */
#include "coldkeep.h"

#include <string.h>

const char *ck_case_type_name(ck_case_type_t t) {
  switch (t) {
    case CASE_CHILLED: return "chilled";
    case CASE_FROZEN: return "frozen";
    case CASE_COLDROOM: return "coldroom";
  }
  return "unknown";
}

const char *ck_cmd_type_name(ck_cmd_type_t t) {
  switch (t) {
    case CMD_SET_SETPOINT: return "set_setpoint";
    case CMD_START_DEFROST: return "start_defrost";
    case CMD_STOP_DEFROST: return "stop_defrost";
    case CMD_SET_FANS: return "set_fans";
    case CMD_DISABLE_ALARMS: return "disable_alarms";
    case CMD_SET_DEFROST_SCHEDULE: return "set_defrost_schedule";
    case CMD_CHANGE_ENVELOPE: return "change_envelope";
    case CMD_RAW_WRITE: return "raw_write";
    case CMD_STATUS: return "status";
  }
  return "unknown";
}

const char *ck_verdict_name(ck_verdict_t v) {
  switch (v) {
    case VERDICT_ALLOW: return "ALLOW";
    case VERDICT_CLAMP: return "CLAMP";
    case VERDICT_DENY: return "DENY";
    case VERDICT_NEEDS_KEY: return "NEEDS_KEY";
  }
  return "?";
}

const char *ck_rule_name(uint32_t rule_id) {
  switch (rule_id) {
    case CK_RULE_NONE: return "-";
    case CK_RULE_G1: return "G1";
    case CK_RULE_G2: return "G2";
    case CK_RULE_G3: return "G3";
    case CK_RULE_G4: return "G4";
    case CK_RULE_G5: return "G5";
    case CK_RULE_G6: return "G6";
    case CK_RULE_SCHEMA: return "SCHEMA";
    case CK_RULE_RATE: return "RATE";
    case CK_RULE_UNAVAILABLE: return "UNAVAILABLE";
    case CK_RULE_AUTH: return "AUTH";
    case CK_RULE_LEGACY: return "LEGACY";
  }
  return "?";
}

const char *ck_mode_name(ck_mode_t m) {
  return m == MODE_LEGACY ? "legacy" : m == MODE_PROTECTED ? "protected" : "?";
}

void ck_strlcpy(char *dst, const char *src, size_t cap) {
  if (cap == 0) return;
  size_t n = src ? strlen(src) : 0;
  if (n >= cap) n = cap - 1;
  if (n) memcpy(dst, src, n);
  dst[n] = '\0';
}
