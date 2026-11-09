/* SPDX-License-Identifier: Apache-2.0
 *
 * auditd — append-only audit journal (core/journal.c) and compliance log.
 *
 * /dev/coldkeep/audit
 *   read            the last records as text (anyone)
 *   CK_MSG_AUDIT    append a record; accepted only from trusted uids
 *                   (governord, brokerd, casectl, plantsim, root). Anyone else,
 *                   including mgmtd, gets EACCES.
 *
 * The journal file (/data/coldkeep/audit.bin) is owned by auditd's uid, mode 0600.
 * Every 15 sim-minutes auditd also records each case's air and product reading.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/iofunc.h>
#include <sys/neutrino.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "ck_target.h"

#define PROC "auditd"
#define RING 64

static resmgr_connect_funcs_t connect_funcs;
static resmgr_io_funcs_t io_funcs;
static iofunc_attr_t attr;
static ck_journal_t g_j;
static ck_plant_shm_t *g_ps;
static uint64_t g_next_compliance;
static char g_ring[RING][200];
static unsigned g_ring_head, g_ring_n;
static uint64_t g_rejected;

static int trusted(int uid) {
  return uid == 0 || uid == CK_UID_GOV || uid == CK_UID_BROKER || uid == CK_UID_CASE || uid == CK_UID_PLANT;
}

static void remember(const ck_jrec_t *r) {
  char *line = g_ring[g_ring_head];
  const double t = (double)r->t_ns / 1e9;
  if (r->kind == JREC_DECISION) {
    const ck_decision_t *d = &r->d;
    snprintf(line, sizeof g_ring[0], "t=%.0f %s/%s %s case=%u value=%.1f -> %s %s %s", t,
             d->cmd.origin, d->cmd.user, ck_cmd_type_name(d->cmd.type), d->cmd.case_id,
             (double)d->cmd.value, ck_verdict_name(d->verdict), ck_rule_name(d->rule_id), d->reason);
  } else {
    snprintf(line, sizeof g_ring[0], "t=%.0f %s %s", t,
             r->kind == JREC_COMPLIANCE ? "compliance" : "event", r->text);
  }
  g_ring_head = (g_ring_head + 1) % RING;
  if (g_ring_n < RING) g_ring_n++;
}

static int append(const ck_jrec_t *r) {
  const bool sync = r->kind == JREC_DECISION &&
                    (r->d.verdict == VERDICT_DENY || r->d.verdict == VERDICT_CLAMP ||
                     r->d.verdict == VERDICT_NEEDS_KEY);
  const int rc = ck_journal_append(&g_j, r, sync);
  if (rc == 0) remember(r);
  return rc;
}

static int on_audit(message_context_t *ctp, int type, unsigned flags, void *handle) {
  const int uid = ck_sender_uid(ctp->info.scoid);
  if (!trusted(uid)) {
    g_rejected++;
    MsgError(ctp->rcvid, EACCES);
    return 0;
  }
  if (ctp->info.msglen < (ssize_t)sizeof(ck_msg_audit_t)) { MsgError(ctp->rcvid, EINVAL); return 0; }
  ck_msg_audit_t m;
  memcpy(&m, ctp->msg, sizeof m);
  m.rec.text[sizeof m.rec.text - 1] = '\0';
  m.rec.d.reason[sizeof m.rec.d.reason - 1] = '\0';
  const int rc = append(&m.rec);
  if (rc != 0) MsgError(ctp->rcvid, -rc);
  else MsgReply(ctp->rcvid, EOK, NULL, 0);
  return 0;
}

static int on_tick(message_context_t *ctp, int code, unsigned flags, void *handle) {
  ck_plant_shm_t snap;
  ck_plant_read(g_ps, &snap);
  if (snap.sim_ns < g_next_compliance) return 0;
  g_next_compliance = snap.sim_ns + CK_S(15 * 60);
  ck_jrec_t r;
  memset(&r, 0, sizeof r);
  r.kind = JREC_COMPLIANCE;
  r.t_ns = snap.sim_ns;
  size_t o = 0;
  for (uint16_t i = 0; i < snap.ncases && o < sizeof r.text - 24; i++) {
    int w = snprintf(r.text + o, sizeof r.text - o, "%s%u:%.1f/%.1f", i ? " " : "", i + 1,
                     (double)snap.c[i].air_c, (double)snap.c[i].product_c);
    if (w > 0) o += (size_t)w;
  }
  append(&r);
  return 0;
}

static int io_read(resmgr_context_t *ctp, io_read_t *msg, RESMGR_OCB_T *ocb) {
  int status = iofunc_read_verify(ctp, msg, ocb, NULL);
  if (status != EOK) return status;
  if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) return ENOSYS;
  static char buf[RING * 202 + 128];
  int len = snprintf(buf, sizeof buf, "# journal %s: %zu records (%zu bytes); %llu rejected senders\n",
                     CK_JOURNAL, g_j.records, g_j.bytes, (unsigned long long)g_rejected);
  for (unsigned k = 0; k < g_ring_n; k++) {
    unsigned idx = (g_ring_head + RING - g_ring_n + k) % RING;
    len += snprintf(buf + len, sizeof buf - (size_t)len, "%s\n", g_ring[idx]);
  }
  iofunc_ocb_t *o = (iofunc_ocb_t *)ocb;
  size_t n = 0;
  if (o->offset < len) {
    n = (size_t)(len - o->offset);
    if (n > msg->i.nbytes) n = msg->i.nbytes;
  }
  MsgReply(ctp->rcvid, (long)n, buf + (n ? o->offset : 0), n);
  o->offset += (off_t)n;
  return _RESMGR_NOREPLY;
}

int main(void) {
  mkdir(CK_DATA_DIR, 0755);
  int rc = ck_journal_open(&g_j, CK_JOURNAL);
  if (rc != 0) { ck_log(PROC, "open %s: %s", CK_JOURNAL, strerror(-rc)); return 1; }
  if (fchown(g_j.fd, CK_UID_AUDIT, CK_UID_AUDIT) != 0 || fchmod(g_j.fd, 0600) != 0) {
    ck_log(PROC, "journal ownership: %s", strerror(errno));
    return 1;
  }
  if (g_j.recovered_dropped)
    ck_log(PROC, "recovered journal: dropped %zu torn bytes", g_j.recovered_dropped);
  g_ps = ck_map_plant(0, 30);
  if (!g_ps) { ck_log(PROC, "plant shared memory not available"); return 1; }

  dispatch_t *dpp = dispatch_create();
  iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &connect_funcs, _RESMGR_IO_NFUNCS, &io_funcs);
  io_funcs.read = io_read;
  iofunc_attr_init(&attr, S_IFNAM | 0444, NULL, NULL);
  resmgr_attr_t rattr;
  memset(&rattr, 0, sizeof rattr);
  rattr.nparts_max = 1;
  rattr.msg_max_size = 4096;
  if (!dpp || resmgr_attach(dpp, &rattr, CK_DEV_AUDIT, _FTYPE_ANY, 0, &connect_funcs, &io_funcs, &attr) == -1 ||
      message_attach(dpp, NULL, CK_MSG_AUDIT, CK_MSG_AUDIT, on_audit, NULL) == -1) {
    ck_log(PROC, "attach: %s", strerror(errno));
    return 1;
  }
  const int code = pulse_attach(dpp, MSG_FLAG_ALLOC_PULSE, 0, on_tick, NULL);
  const int coid = message_connect(dpp, MSG_FLAG_SIDE_CHANNEL);
  struct sigevent ev;
  SIGEV_PULSE_INIT(&ev, coid, CK_PRIO_AUDIT, code, 0);
  timer_t tid;
  struct itimerspec its = {.it_value = {1, 0}, .it_interval = {1, 0}};
  if (timer_create(CLOCK_MONOTONIC, &ev, &tid) != 0 || timer_settime(tid, 0, &its, NULL) != 0) {
    ck_log(PROC, "timer: %s", strerror(errno));
    return 1;
  }
  if (ck_set_fifo_prio(CK_PRIO_AUDIT) != EOK) ck_log(PROC, "warning: could not set priority");
  if (ck_drop_to(CK_UID_AUDIT, NULL, 0, 0) != 0) { ck_log(PROC, "drop privileges: %s", strerror(errno)); return 1; }
  ck_log(PROC, "journal %s (%zu records), uid %d", CK_JOURNAL, g_j.records, (int)getuid());

  dispatch_context_t *ctp = dispatch_context_alloc(dpp);
  for (;;) {
    if ((ctp = dispatch_block(ctp)) == NULL) { ck_log(PROC, "dispatch_block: %s", strerror(errno)); return 1; }
    dispatch_handler(ctp);
  }
}
