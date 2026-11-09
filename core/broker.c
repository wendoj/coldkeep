/* SPDX-License-Identifier: Apache-2.0
 *
 * Command broker: schema validation, per-session rate limit,
 * classification. Dangerous commands never reach the governor: they become
 * NEEDS_KEY and wait in the keyconsole queue. cmd->user is logged, never trusted.
 */
#include "coldkeep.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

const char *ck_class_name(ck_class_t c) {
  switch (c) {
    case CLASS_READ: return "read";
    case CLASS_ROUTINE: return "routine";
    case CLASS_RISKY: return "risky";
    case CLASS_DANGEROUS: return "dangerous";
    case CLASS_INVALID: return "invalid";
  }
  return "?";
}

void ck_broker_init(ck_broker_t *b, const ck_envelope_t *env, uint16_t n, const ck_case_type_t *types) {
  memset(b, 0, sizeof *b);
  b->env = env;
  b->n = n > CK_MAX_CASES ? CK_MAX_CASES : n;
  for (uint16_t i = 0; i < b->n; i++) b->types[i] = types[i];
  b->rate_burst = 10.0f;
  b->rate_per_s = 2.0f;
}

static bool terminated(const char *s, size_t cap) { return memchr(s, '\0', cap) != NULL; }

static bool store_wide_ok(ck_cmd_type_t t) {
  return t == CMD_STATUS || t == CMD_DISABLE_ALARMS;
}

int ck_broker_validate(const ck_broker_t *b, const ck_cmd_t *c, char *why, size_t cap) {
#define BAD(msg) do { ck_strlcpy(why, msg, cap); return -1; } while (0)
  if (!terminated(c->origin, sizeof c->origin) || !terminated(c->user, sizeof c->user))
    BAD("unterminated string field");
  if ((int)c->type < CMD_SET_SETPOINT || (int)c->type > CK_CMD_MAX) BAD("unknown command type");
  if (!isfinite(c->value)) BAD("value not finite");
  if (c->case_id == 0) {
    if (!store_wide_ok(c->type)) BAD("command needs a case id");
  } else if (c->case_id > b->n) {
    BAD("unknown case id");
  }
  switch (c->type) {
    case CMD_SET_SETPOINT:
      if (c->value < -60.0f || c->value > 60.0f) BAD("setpoint outside -60..60");
      break;
    case CMD_START_DEFROST:
      if (c->value < 1.0f || c->value > 7.0f * 86400.0f) BAD("defrost seconds outside 1..604800");
      break;
    case CMD_SET_FANS:
      if (c->value != 0.0f && c->value != 1.0f) BAD("fans must be 0 or 1");
      break;
    default:
      break;
  }
  return 0;
#undef BAD
}

ck_class_t ck_broker_classify(const ck_broker_t *b, const ck_cmd_t *c) {
  switch (c->type) {
    case CMD_STATUS: return CLASS_READ;
    case CMD_SET_SETPOINT: {
      if (c->case_id < 1 || c->case_id > b->n) return CLASS_INVALID;
      const ck_env_type_t *e = &b->env->t[b->types[c->case_id - 1]];
      if (c->value > e->sp_min + 0.5f && c->value < e->sp_max - 0.5f) return CLASS_ROUTINE;
      return CLASS_RISKY;
    }
    case CMD_START_DEFROST:
      return strcmp(c->origin, "schedule") == 0 ? CLASS_ROUTINE : CLASS_RISKY;
    case CMD_STOP_DEFROST: return CLASS_ROUTINE;
    case CMD_SET_FANS: return c->value == 0.0f ? CLASS_DANGEROUS : CLASS_ROUTINE;
    case CMD_DISABLE_ALARMS:
    case CMD_SET_DEFROST_SCHEDULE:
    case CMD_CHANGE_ENVELOPE:
    case CMD_RAW_WRITE:
      return CLASS_DANGEROUS;
  }
  return CLASS_INVALID;
}

static ck_bucket_t *bucket(ck_broker_t *b, uint32_t session, uint64_t now) {
  ck_bucket_t *free_slot = NULL, *oldest = &b->b[0];
  for (int i = 0; i < CK_MAX_SESSIONS; i++) {
    ck_bucket_t *k = &b->b[i];
    if (k->used && k->id == session) return k;
    if (!k->used && !free_slot) free_slot = k;
    if (k->last_ns < oldest->last_ns) oldest = k;
  }
  ck_bucket_t *k = free_slot ? free_slot : oldest;
  *k = (ck_bucket_t){.id = session, .tokens = b->rate_burst, .last_ns = now, .used = true};
  return k;
}

static bool take_token(ck_broker_t *b, uint32_t session, uint64_t now) {
  ck_bucket_t *k = bucket(b, session, now);
  if (now > k->last_ns) {
    k->tokens += (float)((double)(now - k->last_ns) / (double)CK_NS_PER_S) * b->rate_per_s;
    if (k->tokens > b->rate_burst) k->tokens = b->rate_burst;
    k->last_ns = now;
  }
  if (k->tokens < 1.0f) return false;
  k->tokens -= 1.0f;
  return true;
}

ck_broker_out_t ck_broker_check(ck_broker_t *b, uint32_t session, const ck_cmd_t *cmd,
                                uint64_t now, ck_decision_t *d) {
  memset(d, 0, sizeof *d);
  d->cmd = *cmd;
  d->applied_value = NAN;
  if (!take_token(b, session, now)) {
    b->rate_limited++;
    d->verdict = VERDICT_DENY; d->rule_id = CK_RULE_RATE;
    ck_strlcpy(d->reason, "session rate limit", sizeof d->reason);
    return BROKER_REJECT;
  }
  char why[96];
  if (ck_broker_validate(b, cmd, why, sizeof why) != 0) {
    b->rejected++;
    d->verdict = VERDICT_DENY; d->rule_id = CK_RULE_SCHEMA;
    ck_strlcpy(d->reason, why, sizeof d->reason);
    return BROKER_REJECT;
  }
  switch (ck_broker_classify(b, cmd)) {
    case CLASS_READ:
      b->reads++;
      return BROKER_READ;
    case CLASS_DANGEROUS:
      b->needs_key++;
      d->verdict = VERDICT_NEEDS_KEY; d->rule_id = CK_RULE_G5;
      ck_strlcpy(d->reason, "dangerous: queued for local store-key approval", sizeof d->reason);
      return BROKER_NEEDS_KEY;
    case CLASS_INVALID:
      b->rejected++;
      d->verdict = VERDICT_DENY; d->rule_id = CK_RULE_SCHEMA;
      ck_strlcpy(d->reason, "unclassifiable command", sizeof d->reason);
      return BROKER_REJECT;
    default:
      b->forwarded++;
      return BROKER_FORWARD;
  }
}

void ck_keyq_init(ck_keyq_t *q) { memset(q, 0, sizeof *q); q->next_id = 1; }

uint64_t ck_keyq_push(ck_keyq_t *q, const ck_cmd_t *cmd) {
  if (q->n >= CK_KEYQ_MAX) {
    /* reuse the oldest decided slot; if all pending, drop the request */
    for (uint32_t i = 0; i < q->n; i++)
      if (q->r[i].state != 0) {
        q->r[i] = (ck_keyreq_t){.req_id = q->next_id++, .cmd = *cmd, .state = 0};
        return q->r[i].req_id;
      }
    q->dropped++;
    return 0;
  }
  q->r[q->n] = (ck_keyreq_t){.req_id = q->next_id++, .cmd = *cmd, .state = 0};
  return q->r[q->n++].req_id;
}

ck_keyreq_t *ck_keyq_find(ck_keyq_t *q, uint64_t id) {
  for (uint32_t i = 0; i < q->n; i++)
    if (q->r[i].req_id == id) return &q->r[i];
  return NULL;
}

uint32_t ck_keyq_pending(const ck_keyq_t *q) {
  uint32_t k = 0;
  for (uint32_t i = 0; i < q->n; i++) k += q->r[i].state == 0;
  return k;
}
