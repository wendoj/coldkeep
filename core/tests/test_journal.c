/* SPDX-License-Identifier: Apache-2.0
 * Journal: round trip, recovery after random truncation and corruption, file reopen. */
#include <stdlib.h>
#include <unistd.h>

#include "ck_test.h"
#include "coldkeep.h"

static ck_jrec_t make_rec(ck_rng_t *r, int i) {
  ck_jrec_t rec;
  memset(&rec, 0, sizeof rec);
  rec.t_ns = (uint64_t)i * 1000;
  if (ck_rng_below(r, 3) == 0) {
    rec.kind = JREC_EVENT;
    snprintf(rec.text, sizeof rec.text, "event %d %llu", i, (unsigned long long)ck_rng_next(r));
  } else {
    rec.kind = JREC_DECISION;
    rec.d.cmd.id = (uint64_t)i;
    ck_strlcpy(rec.d.cmd.origin, "mgmt", sizeof rec.d.cmd.origin);
    ck_strlcpy(rec.d.cmd.user, "admin", sizeof rec.d.cmd.user);
    rec.d.cmd.type = CMD_START_DEFROST; rec.d.cmd.case_id = (uint16_t)(1 + i % 8);
    rec.d.cmd.value = ck_rng_range(r, 0, 86400);
    rec.d.verdict = VERDICT_CLAMP; rec.d.rule_id = 3; rec.d.applied_value = 2700;
    snprintf(rec.d.reason, sizeof rec.d.reason, "max defrost 45 min #%d", i);
  }
  return rec;
}

typedef struct { int n; ck_jrec_t last; } collect_t;
static void collect(const ck_jrec_t *r, void *ctx) { collect_t *c = ctx; c->n++; c->last = *r; }

static void round_trip(void) {
  ck_rng_t r; ck_rng_seed(&r, 1);
  uint8_t buf[CK_JREC_MAX_BYTES];
  for (int i = 0; i < 50; i++) {
    ck_jrec_t a = make_rec(&r, i);
    size_t n = ck_journal_encode(&a, buf, sizeof buf);
    CHECK(n > 0);
    collect_t c = {0};
    CHECK_EQ_INT(ck_journal_scan(buf, n, collect, &c, NULL), n);
    CHECK_EQ_INT(c.n, 1);
    CHECK(c.last.kind == a.kind && c.last.t_ns == a.t_ns);
    if (a.kind == JREC_DECISION) {
      CHECK(c.last.d.cmd.value == a.d.cmd.value && strcmp(c.last.d.reason, a.d.reason) == 0);
      CHECK(c.last.d.verdict == a.d.verdict && c.last.d.cmd.case_id == a.d.cmd.case_id);
    } else {
      CHECK(strcmp(c.last.text, a.text) == 0);
    }
  }
  CHECK(ck_crc32("123456789", 9) == 0xCBF43926u); /* standard check value */
}

static void recovery_after_random_truncation(void) {
  ck_rng_t r; ck_rng_seed(&r, 2);
  enum { NREC = 200 };
  static uint8_t buf[NREC * CK_JREC_MAX_BYTES];
  size_t ends[NREC], len = 0;
  for (int i = 0; i < NREC; i++) {
    ck_jrec_t a = make_rec(&r, i);
    len += ck_journal_encode(&a, buf + len, sizeof buf - len);
    ends[i] = len;
  }
  int ok = 1;
  for (int trial = 0; trial < 2000; trial++) {
    size_t cut = ck_rng_below(&r, (uint32_t)len + 1);
    size_t expect = 0, expect_n = 0;
    for (int i = 0; i < NREC; i++) if (ends[i] <= cut) { expect = ends[i]; expect_n = (size_t)i + 1; }
    size_t count = 0;
    size_t valid = ck_journal_scan(buf, cut, NULL, NULL, &count);
    ok &= valid == expect && count == expect_n;
  }
  CHECK(ok);
}

static void corruption_stops_scan(void) {
  ck_rng_t r; ck_rng_seed(&r, 3);
  static uint8_t buf[100 * CK_JREC_MAX_BYTES];
  size_t ends[100], len = 0;
  for (int i = 0; i < 100; i++) {
    ck_jrec_t a = make_rec(&r, i);
    len += ck_journal_encode(&a, buf + len, sizeof buf - len);
    ends[i] = len;
  }
  int ok = 1;
  for (int trial = 0; trial < 500; trial++) {
    size_t pos = ck_rng_below(&r, (uint32_t)len);
    uint8_t save = buf[pos];
    buf[pos] ^= (uint8_t)(1u << ck_rng_below(&r, 8));
    size_t rec_start = 0;
    for (int i = 0; i < 100; i++) { if (ends[i] > pos) break; rec_start = ends[i]; }
    size_t valid = ck_journal_scan(buf, len, NULL, NULL, NULL);
    ok &= valid == rec_start;   /* everything before the damaged record survives */
    buf[pos] = save;
  }
  CHECK(ok);
}

static void file_recovery(void) {
  char path[] = "/tmp/ckjournalXXXXXX";
  int fd = mkstemp(path);
  CHECK(fd >= 0);
  close(fd);
  ck_journal_t j;
  CHECK_EQ_INT(ck_journal_open(&j, path), 0);
  ck_rng_t r; ck_rng_seed(&r, 4);
  for (int i = 0; i < 20; i++) { ck_jrec_t a = make_rec(&r, i); CHECK_EQ_INT(ck_journal_append(&j, &a, i % 5 == 0), 0); }
  size_t full = j.bytes;
  ck_journal_close(&j);
  /* tear the last record */
  CHECK_EQ_INT(truncate(path, (off_t)(full - 7)), 0);
  CHECK_EQ_INT(ck_journal_open(&j, path), 0);
  CHECK_EQ_INT(j.records, 19);
  CHECK(j.recovered_dropped > 0);
  ck_jrec_t a = make_rec(&r, 99);
  CHECK_EQ_INT(ck_journal_append(&j, &a, true), 0);
  ck_journal_close(&j);
  CHECK_EQ_INT(ck_journal_open(&j, path), 0);
  CHECK_EQ_INT(j.records, 20);
  CHECK_EQ_INT(j.recovered_dropped, 0);
  ck_journal_close(&j);
  unlink(path);
}

void suite_journal(void) {
  RUN(round_trip);
  RUN(recovery_after_random_truncation);
  RUN(corruption_stops_scan);
  RUN(file_recovery);
}
