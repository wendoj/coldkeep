/* SPDX-License-Identifier: Apache-2.0
 *
 * Append-only audit journal. On-disk record format, little-endian:
 *
 *   magic   u32  'CKJ1'
 *   len     u16  payload length
 *   payload len bytes
 *   crc     u32  CRC-32 (IEEE) over len + payload
 *
 * Recovery scans from the start and keeps the longest prefix of intact records;
 * a torn or corrupt tail is truncated on open. POSIX only (portable to QNX).
 */
#include "coldkeep.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define JMAGIC 0x314A4B43u /* "CKJ1" little-endian */

static uint32_t crc_table[256];
static int crc_ready;

uint32_t ck_crc32(const void *data, size_t len) {
  if (!crc_ready) {
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      crc_table[i] = c;
    }
    crc_ready = 1;
  }
  const uint8_t *p = data;
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; i++) c = crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

/* --- little-endian writer/reader ------------------------------------ */
typedef struct { uint8_t *p; size_t n, cap; int err; } wbuf_t;
static void w_u8(wbuf_t *w, uint8_t v) { if (w->n + 1 > w->cap) { w->err = 1; return; } w->p[w->n++] = v; }
static void w_u16(wbuf_t *w, uint16_t v) { w_u8(w, (uint8_t)v); w_u8(w, (uint8_t)(v >> 8)); }
static void w_u32(wbuf_t *w, uint32_t v) { w_u16(w, (uint16_t)v); w_u16(w, (uint16_t)(v >> 16)); }
static void w_u64(wbuf_t *w, uint64_t v) { w_u32(w, (uint32_t)v); w_u32(w, (uint32_t)(v >> 32)); }
static void w_f32(wbuf_t *w, float f) { uint32_t u; memcpy(&u, &f, 4); w_u32(w, u); }
static void w_str(wbuf_t *w, const char *s, size_t fixed) {
  for (size_t i = 0; i < fixed; i++) w_u8(w, (uint8_t)s[i]);
}

typedef struct { const uint8_t *p; size_t n, i; int err; } rbuf_t;
static uint8_t r_u8(rbuf_t *r) { if (r->i + 1 > r->n) { r->err = 1; return 0; } return r->p[r->i++]; }
static uint16_t r_u16(rbuf_t *r) { uint16_t a = r_u8(r); return (uint16_t)(a | (uint16_t)(r_u8(r) << 8)); }
static uint32_t r_u32(rbuf_t *r) { uint32_t a = r_u16(r); return a | ((uint32_t)r_u16(r) << 16); }
static uint64_t r_u64(rbuf_t *r) { uint64_t a = r_u32(r); return a | ((uint64_t)r_u32(r) << 32); }
static float r_f32(rbuf_t *r) { uint32_t u = r_u32(r); float f; memcpy(&f, &u, 4); return f; }
static void r_str(rbuf_t *r, char *s, size_t fixed) {
  for (size_t i = 0; i < fixed; i++) s[i] = (char)r_u8(r);
  s[fixed - 1] = '\0';
}

size_t ck_journal_encode(const ck_jrec_t *rec, uint8_t *buf, size_t cap) {
  uint8_t payload[CK_JREC_MAX_BYTES];
  wbuf_t w = {payload, 0, sizeof payload, 0};
  w_u8(&w, (uint8_t)rec->kind);
  w_u64(&w, rec->t_ns);
  if (rec->kind == JREC_DECISION) {
    const ck_decision_t *d = &rec->d;
    w_u64(&w, d->cmd.id); w_u64(&w, d->cmd.recv_ns);
    w_str(&w, d->cmd.origin, sizeof d->cmd.origin);
    w_str(&w, d->cmd.user, sizeof d->cmd.user);
    w_u8(&w, (uint8_t)d->cmd.type); w_u16(&w, d->cmd.case_id); w_f32(&w, d->cmd.value);
    w_u8(&w, (uint8_t)d->verdict); w_u32(&w, d->rule_id); w_f32(&w, d->applied_value);
    w_str(&w, d->reason, sizeof d->reason);
  } else {
    size_t tl = strnlen(rec->text, CK_JTEXT - 1);
    w_u16(&w, (uint16_t)tl);
    for (size_t i = 0; i < tl; i++) w_u8(&w, (uint8_t)rec->text[i]);
  }
  if (w.err) return 0;

  const size_t total = 4 + 2 + w.n + 4;
  if (total > cap) return 0;
  wbuf_t o = {buf, 0, cap, 0};
  w_u32(&o, JMAGIC);
  w_u16(&o, (uint16_t)w.n);
  memcpy(buf + o.n, payload, w.n); o.n += w.n;
  w_u32(&o, ck_crc32(buf + 4, 2 + w.n));
  return o.err ? 0 : o.n;
}

static int decode_payload(const uint8_t *p, size_t n, ck_jrec_t *rec) {
  rbuf_t r = {p, n, 0, 0};
  memset(rec, 0, sizeof *rec);
  rec->kind = (ck_jrec_kind_t)r_u8(&r);
  rec->t_ns = r_u64(&r);
  if (rec->kind == JREC_DECISION) {
    ck_decision_t *d = &rec->d;
    d->cmd.id = r_u64(&r); d->cmd.recv_ns = r_u64(&r);
    r_str(&r, d->cmd.origin, sizeof d->cmd.origin);
    r_str(&r, d->cmd.user, sizeof d->cmd.user);
    d->cmd.type = (ck_cmd_type_t)r_u8(&r); d->cmd.case_id = r_u16(&r); d->cmd.value = r_f32(&r);
    d->verdict = (ck_verdict_t)r_u8(&r); d->rule_id = r_u32(&r); d->applied_value = r_f32(&r);
    r_str(&r, d->reason, sizeof d->reason);
  } else if (rec->kind == JREC_EVENT || rec->kind == JREC_COMPLIANCE) {
    uint16_t tl = r_u16(&r);
    if (tl >= CK_JTEXT) return -1;
    for (uint16_t i = 0; i < tl; i++) rec->text[i] = (char)r_u8(&r);
    rec->text[tl] = '\0';
  } else {
    return -1;
  }
  return (r.err || r.i != n) ? -1 : 0;
}

size_t ck_journal_scan(const uint8_t *buf, size_t len, ck_jrec_cb cb, void *ctx, size_t *count) {
  size_t off = 0, k = 0;
  while (off + 10 <= len) {
    rbuf_t r = {buf + off, len - off, 0, 0};
    if (r_u32(&r) != JMAGIC) break;
    uint16_t pl = r_u16(&r);
    if (pl > CK_JREC_MAX_BYTES || off + 6 + pl + 4 > len) break;
    r.i += pl;
    uint32_t crc = r_u32(&r);
    if (crc != ck_crc32(buf + off + 4, 2 + (size_t)pl)) break;
    ck_jrec_t rec;
    if (decode_payload(buf + off + 6, pl, &rec) != 0) break;
    if (cb) cb(&rec, ctx);
    k++;
    off += 6 + (size_t)pl + 4;
  }
  if (count) *count = k;
  return off;
}

int ck_journal_open(ck_journal_t *j, const char *path) {
  memset(j, 0, sizeof *j);
  j->fd = -1;
  int fd = open(path, O_RDWR | O_CREAT, 0640);
  if (fd < 0) return -errno;
  struct stat sb;
  if (fstat(fd, &sb) != 0) { int e = errno; close(fd); return -e; }
  size_t sz = (size_t)sb.st_size;
  if (sz > 0) {
    uint8_t *buf = malloc(sz);
    if (!buf) { close(fd); return -ENOMEM; }
    size_t got = 0;
    while (got < sz) {
      ssize_t r = pread(fd, buf + got, sz - got, (off_t)got);
      if (r <= 0) break;
      got += (size_t)r;
    }
    size_t count = 0;
    size_t valid = ck_journal_scan(buf, got, NULL, NULL, &count);
    free(buf);
    if (valid < sz) {
      if (ftruncate(fd, (off_t)valid) != 0) { int e = errno; close(fd); return -e; }
      j->recovered_dropped = sz - valid;
    }
    j->records = count;
    j->bytes = valid;
  }
  if (lseek(fd, 0, SEEK_END) < 0) { int e = errno; close(fd); return -e; }
  j->fd = fd;
  return 0;
}

int ck_journal_append(ck_journal_t *j, const ck_jrec_t *rec, bool sync) {
  uint8_t buf[CK_JREC_MAX_BYTES + 16];
  size_t n = ck_journal_encode(rec, buf, sizeof buf);
  if (n == 0) return -EINVAL;
  size_t done = 0;
  while (done < n) {
    ssize_t w = write(j->fd, buf + done, n - done);
    if (w < 0) { if (errno == EINTR) continue; return -errno; }
    done += (size_t)w;
  }
  if (sync && fsync(j->fd) != 0) return -errno;
  j->records++;
  j->bytes += n;
  return 0;
}

void ck_journal_close(ck_journal_t *j) {
  if (j->fd >= 0) close(j->fd);
  j->fd = -1;
}
