/* SPDX-License-Identifier: Apache-2.0
 *
 * mgmtd's JSON-lines protocol. Every byte here comes from the network, so the
 * parser is deliberately small and strict: one flat object per line, scalar
 * values only, bounded string lengths, no allocation.
 *
 *   {"op":"login","user":"admin","pass":"admin"}
 *   {"op":"status"}
 *   {"op":"set_setpoint","case":1,"value":4.5}
 *   {"op":"start_defrost","case":1,"seconds":86400}
 *   {"op":"stop_defrost","case":1}
 *   {"op":"set_fans","case":1,"on":false}
 *   {"op":"disable_alarms"}                       (store-wide; "case" optional)
 *   {"op":"set_defrost_schedule","case":1,"hours":2}
 *   {"op":"change_envelope","case":1,"value":12}
 *   {"op":"raw_write","case":1,"value":1}
 *   {"op":"debug_exec","action":"devctl_case"}    (demo builds only)
 */
#include "coldkeep.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char *p, *end; } jp_t;

typedef enum { JV_NONE = 0, JV_STR, JV_NUM, JV_TRUE, JV_FALSE, JV_NULL } jv_kind_t;
typedef struct { jv_kind_t k; char s[64]; double n; } jv_t;

static void ws(jp_t *j) {
  while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\r' || *j->p == '\n')) j->p++;
}

static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/* Parses a JSON string into out (cap bytes). Over-long strings are an error. */
static int jstring(jp_t *j, char *out, size_t cap) {
  if (j->p >= j->end || *j->p != '"') return -1;
  j->p++;
  size_t n = 0;
  while (j->p < j->end) {
    unsigned char c = (unsigned char)*j->p++;
    if (c == '"') { out[n] = '\0'; return 0; }
    if (c < 0x20) return -1;
    if (c == '\\') {
      if (j->p >= j->end) return -1;
      char e = *j->p++;
      switch (e) {
        case '"': c = '"'; break;
        case '\\': c = '\\'; break;
        case '/': c = '/'; break;
        case 'b': c = '\b'; break;
        case 'f': c = '\f'; break;
        case 'n': c = '\n'; break;
        case 'r': c = '\r'; break;
        case 't': c = '\t'; break;
        case 'u': {
          if (j->end - j->p < 4) return -1;
          int v = 0;
          for (int i = 0; i < 4; i++) {
            int h = hexval(j->p[i]);
            if (h < 0) return -1;
            v = v * 16 + h;
          }
          j->p += 4;
          if (v == 0) return -1;
          c = v < 0x80 ? (unsigned char)v : '?';
          break;
        }
        default: return -1;
      }
    }
    if (n + 1 >= cap) return -1;
    out[n++] = (char)c;
  }
  return -1;
}

static int jnumber(jp_t *j, double *out) {
  const char *s = j->p;
  if (j->p < j->end && *j->p == '-') j->p++;
  if (j->p >= j->end) return -1;
  if (*j->p == '0') j->p++;
  else if (*j->p >= '1' && *j->p <= '9') while (j->p < j->end && *j->p >= '0' && *j->p <= '9') j->p++;
  else return -1;
  if (j->p < j->end && *j->p == '.') {
    j->p++;
    if (j->p >= j->end || *j->p < '0' || *j->p > '9') return -1;
    while (j->p < j->end && *j->p >= '0' && *j->p <= '9') j->p++;
  }
  if (j->p < j->end && (*j->p == 'e' || *j->p == 'E')) {
    j->p++;
    if (j->p < j->end && (*j->p == '+' || *j->p == '-')) j->p++;
    if (j->p >= j->end || *j->p < '0' || *j->p > '9') return -1;
    while (j->p < j->end && *j->p >= '0' && *j->p <= '9') j->p++;
  }
  char tmp[40];
  size_t len = (size_t)(j->p - s);
  if (len >= sizeof tmp) return -1;
  memcpy(tmp, s, len);
  tmp[len] = '\0';
  *out = strtod(tmp, NULL);
  return isfinite(*out) ? 0 : -1;
}

static int jliteral(jp_t *j, const char *lit) {
  size_t n = strlen(lit);
  if ((size_t)(j->end - j->p) < n || memcmp(j->p, lit, n) != 0) return -1;
  j->p += n;
  return 0;
}

static int jvalue(jp_t *j, jv_t *v) {
  memset(v, 0, sizeof *v);
  if (j->p >= j->end) return -1;
  switch (*j->p) {
    case '"': v->k = JV_STR; return jstring(j, v->s, sizeof v->s);
    case 't': v->k = JV_TRUE; return jliteral(j, "true");
    case 'f': v->k = JV_FALSE; return jliteral(j, "false");
    case 'n': v->k = JV_NULL; return jliteral(j, "null");
    default: v->k = JV_NUM; return jnumber(j, &v->n);
  }
}

static const struct { const char *op; ck_req_kind_t kind; ck_cmd_type_t cmd; int needs_case; const char *value_key; } k_ops[] = {
  {"login", REQ_LOGIN, 0, 0, NULL},
  {"status", REQ_STATUS, CMD_STATUS, 0, NULL},
  {"debug_exec", REQ_DEBUG_EXEC, 0, 0, NULL},
  {"set_setpoint", REQ_CMD, CMD_SET_SETPOINT, 1, "value"},
  {"start_defrost", REQ_CMD, CMD_START_DEFROST, 1, "seconds"},
  {"stop_defrost", REQ_CMD, CMD_STOP_DEFROST, 1, NULL},
  {"set_fans", REQ_CMD, CMD_SET_FANS, 1, "on"},
  {"disable_alarms", REQ_CMD, CMD_DISABLE_ALARMS, 0, NULL},
  {"set_defrost_schedule", REQ_CMD, CMD_SET_DEFROST_SCHEDULE, 1, "hours"},
  {"change_envelope", REQ_CMD, CMD_CHANGE_ENVELOPE, 1, "value"},
  {"raw_write", REQ_CMD, CMD_RAW_WRITE, 1, "value"},
};

int ck_proto_parse(const char *line, size_t len, ck_req_t *req) {
  memset(req, 0, sizeof *req);
#define BAD(msg) do { req->kind = REQ_INVALID; ck_strlcpy(req->error, msg, sizeof req->error); return -1; } while (0)
  if (len > CK_PROTO_MAX_LINE) BAD("line too long");
  jp_t j = {line, line + len};
  char op[32] = {0};
  int have_case = 0, have_value = 0, have_user = 0, have_pass = 0, have_action = 0;
  double case_n = 0, value_n = 0;
  jv_t named[4] = {{0}};           /* value, seconds, hours, on */
  int have_named[4] = {0};
  static const char *named_keys[4] = {"value", "seconds", "hours", "on"};

  ws(&j);
  if (j.p >= j.end || *j.p != '{') BAD("expected a JSON object");
  j.p++;
  ws(&j);
  if (j.p < j.end && *j.p == '}') BAD("empty object");
  for (;;) {
    char key[32];
    jv_t v;
    ws(&j);
    if (jstring(&j, key, sizeof key) != 0) BAD("bad key");
    ws(&j);
    if (j.p >= j.end || *j.p != ':') BAD("expected ':'");
    j.p++;
    ws(&j);
    if (jvalue(&j, &v) != 0) BAD("bad value (objects/arrays not allowed)");
    if (!strcmp(key, "op")) { if (v.k != JV_STR) BAD("op must be a string"); ck_strlcpy(op, v.s, sizeof op); }
    else if (!strcmp(key, "case")) { if (v.k != JV_NUM) BAD("case must be a number"); case_n = v.n; have_case = 1; }
    else if (!strcmp(key, "user")) { if (v.k != JV_STR) BAD("user must be a string"); ck_strlcpy(req->user, v.s, sizeof req->user); have_user = 1; }
    else if (!strcmp(key, "pass")) { if (v.k != JV_STR) BAD("pass must be a string"); ck_strlcpy(req->pass, v.s, sizeof req->pass); have_pass = 1; }
    else if (!strcmp(key, "action")) { if (v.k != JV_STR) BAD("action must be a string"); ck_strlcpy(req->action, v.s, sizeof req->action); have_action = 1; }
    else {
      for (int k = 0; k < 4; k++)
        if (!strcmp(key, named_keys[k])) { named[k] = v; have_named[k] = 1; }
      /* unknown keys are ignored */
    }
    ws(&j);
    if (j.p < j.end && *j.p == ',') { j.p++; continue; }
    if (j.p < j.end && *j.p == '}') { j.p++; break; }
    BAD("expected ',' or '}'");
  }
  ws(&j);
  if (j.p != j.end) BAD("trailing data after object");
  if (!op[0]) BAD("missing op");

  for (size_t i = 0; i < sizeof k_ops / sizeof k_ops[0]; i++) {
    if (strcmp(op, k_ops[i].op) != 0) continue;
    req->kind = k_ops[i].kind;
    req->cmd = k_ops[i].cmd;
    if (req->kind == REQ_LOGIN && (!have_user || !have_pass)) BAD("login needs user and pass");
    if (req->kind == REQ_DEBUG_EXEC && !have_action) BAD("debug_exec needs action");
    if (have_case) {
      if (case_n < 0 || case_n > 65535 || case_n != (double)(uint16_t)case_n) BAD("case must be an integer 0..65535");
      req->case_id = (uint16_t)case_n;
    } else if (k_ops[i].needs_case) {
      BAD("missing case");
    }
    if (k_ops[i].value_key) {
      /* accept the op's own key, or the generic "value" */
      int idx = -1;
      for (int k = 0; k < 4; k++)
        if (!strcmp(named_keys[k], k_ops[i].value_key) && have_named[k]) idx = k;
      if (idx < 0 && have_named[0]) idx = 0;
      if (idx < 0) { char m[64]; snprintf(m, sizeof m, "missing %s", k_ops[i].value_key); BAD(m); }
      const jv_t *v = &named[idx];
      if (v->k == JV_NUM) value_n = v->n;
      else if (v->k == JV_TRUE && req->cmd == CMD_SET_FANS) value_n = 1;
      else if (v->k == JV_FALSE && req->cmd == CMD_SET_FANS) value_n = 0;
      else BAD("value must be a number");
      have_value = 1;
    }
    req->value = have_value ? (float)value_n : 0.0f;
    if (have_value && !isfinite(req->value)) BAD("value out of range");
    return 0;
  }
  BAD("unknown op");
#undef BAD
}

size_t ck_json_escape(char *buf, size_t cap, const char *s) {
  size_t n = 0;
  if (cap == 0) return 0;
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
    char tmp[8];
    const char *out = tmp;
    if (c == '"') out = "\\\"";
    else if (c == '\\') out = "\\\\";
    else if (c < 0x20) snprintf(tmp, sizeof tmp, "\\u%04x", c);
    else { tmp[0] = (char)c; tmp[1] = '\0'; }
    size_t l = strlen(out);
    if (n + l >= cap) break;
    memcpy(buf + n, out, l);
    n += l;
  }
  buf[n] = '\0';
  return n;
}

static int clamp_len(int w, size_t cap) {
  if (w < 0) return 0;
  return (size_t)w >= cap ? (int)cap - 1 : w;
}

int ck_proto_reply_decision(char *buf, size_t cap, const ck_decision_t *d) {
  char reason[200], applied[32];
  ck_json_escape(reason, sizeof reason, d->reason);
  if (isfinite(d->applied_value)) snprintf(applied, sizeof applied, "%.3f", (double)d->applied_value);
  else ck_strlcpy(applied, "null", sizeof applied);
  int w = snprintf(buf, cap,
                   "{\"ok\":true,\"id\":%llu,\"cmd\":\"%s\",\"case\":%u,\"verdict\":\"%s\",\"rule\":\"%s\","
                   "\"applied\":%s,\"reason\":\"%s\"}",
                   (unsigned long long)d->cmd.id, ck_cmd_type_name(d->cmd.type), d->cmd.case_id,
                   ck_verdict_name(d->verdict), ck_rule_name(d->rule_id), applied, reason);
  return clamp_len(w, cap);
}

int ck_proto_reply_error(char *buf, size_t cap, const char *msg) {
  char m[200];
  ck_json_escape(m, sizeof m, msg);
  return clamp_len(snprintf(buf, cap, "{\"ok\":false,\"error\":\"%s\"}", m), cap);
}

int ck_proto_reply_ok(char *buf, size_t cap, const char *msg) {
  char m[200];
  ck_json_escape(m, sizeof m, msg);
  return clamp_len(snprintf(buf, cap, "{\"ok\":true,\"message\":\"%s\"}", m), cap);
}
