/* SPDX-License-Identifier: Apache-2.0
 *
 * governord — the safety governor (core/governor.c, rules G1–G5).
 *
 * The only process allowed to command the case controllers: it creates the custom
 * ability "coldkeep.plant.write", keeps it when it drops root, and no other process
 * gets it. Commands reach it only from brokerd:
 *
 * /dev/coldkeep/gov     owner = brokerd's uid, mode 0600; CK_MSG_GOV messages
 *                       (a sender that is not brokerd or root gets EACCES)
 * /dev/coldkeep/status  read-only text: envelope, verdict counters, recent decisions
 *
 * Its state (envelope, rate-limit history, last manual defrosts) is saved after
 * every decision and restored on restart, so crashing it cannot reset G2/G3.
 * Heartbeats go to every casectl every 250 ms of wall time (G6 if they stop).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/iofunc.h>
#include <sys/neutrino.h>
#include <sys/procmgr.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "ck_target.h"

#define PROC "governord"
#define STATE_MAGIC 0x434B4753u /* "CKGS" */

static resmgr_connect_funcs_t gov_connect, status_connect;
static resmgr_io_funcs_t gov_io, status_io;
static iofunc_attr_t gov_attr, status_attr;

static ck_gov_t g_gov;
static ck_plant_shm_t *g_ps;
static ck_act_shm_t *g_as;
static uint16_t g_n;
static int g_case_fd[CK_MAX_CASES];
static int g_audit_fd = -1;
static uint64_t g_next_gov_tick_sim;
static uint64_t g_rejected;

typedef struct { uint32_t magic; uint32_t size; uint32_t crc; uint32_t pad; ck_gov_t gov; } state_file_t;

static void save_state(void) {
  static state_file_t sf;
  sf.magic = STATE_MAGIC;
  sf.size = sizeof sf.gov;
  sf.gov = g_gov;
  sf.crc = ck_crc32(&sf.gov, sizeof sf.gov);
  int fd = open(CK_GOV_STATE ".tmp", O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) return;
  const ssize_t w = write(fd, &sf, sizeof sf);
  fsync(fd);
  close(fd);
  if (w == (ssize_t)sizeof sf) rename(CK_GOV_STATE ".tmp", CK_GOV_STATE);
}

static int load_state(void) {
  static state_file_t sf;
  int fd = open(CK_GOV_STATE, O_RDONLY);
  if (fd < 0) return -1;
  const ssize_t r = read(fd, &sf, sizeof sf);
  close(fd);
  if (r != (ssize_t)sizeof sf || sf.magic != STATE_MAGIC || sf.size != sizeof sf.gov ||
      sf.crc != ck_crc32(&sf.gov, sizeof sf.gov) || sf.gov.n != g_n)
    return -1;
  g_gov = sf.gov;
  return 0;
}

static int case_devctl(uint16_t case_id, const ck_case_cmd_t *cmd) {
  const int i = case_id - 1;
  for (int attempt = 0; attempt < 2; attempt++) {
    if (g_case_fd[i] < 0) {
      char path[64];
      snprintf(path, sizeof path, CK_DEV_CASE_FMT, case_id);
      g_case_fd[i] = open(path, O_RDWR);
      if (g_case_fd[i] < 0) return errno;
    }
    ck_case_cmd_t c = *cmd;
    const int rc = devctl(g_case_fd[i], DCMD_CK_CASE, &c, sizeof c, NULL);
    if (rc == EOK || (rc != EBADF && rc != ESRCH && rc != ENOTCONN)) return rc;
    close(g_case_fd[i]); /* controller restarted: reconnect once */
    g_case_fd[i] = -1;
  }
  return ENOTCONN;
}

static void apply_and_audit(ck_decision_t *d, const ck_gov_action_t *act, int has_action) {
  const uint64_t now = ck_sim_now(g_ps);
  if (has_action && act->cmd.type != 0) {
    const int rc = case_devctl(act->case_id, &act->cmd);
    if (rc != EOK) {
      char why[256]; /* truncated to the record's 96 bytes below, on purpose */
      snprintf(why, sizeof why, "controller refused: %s (%s)", strerror(rc), d->reason);
      ck_strlcpy(d->reason, why, sizeof d->reason);
      d->verdict = VERDICT_DENY;
      d->rule_id = CK_RULE_UNAVAILABLE;
    }
  }
  ck_audit_decision(g_audit_fd, d, now);
}

static int on_cmd(message_context_t *ctp, int type, unsigned flags, void *handle) {
  const int uid = ck_sender_uid(ctp->info.scoid);
  if (uid != CK_UID_BROKER && uid != 0) {
    g_rejected++;
    ck_audit_event(g_audit_fd, ck_sim_now(g_ps), "governord: rejected message from uid %d", uid);
    MsgError(ctp->rcvid, EACCES);
    return 0;
  }
  if (ctp->info.msglen < (ssize_t)sizeof(ck_msg_cmd_t)) { MsgError(ctp->rcvid, EINVAL); return 0; }
  ck_msg_cmd_t m;
  memcpy(&m, ctp->msg, sizeof m);
  m.cmd.origin[sizeof m.cmd.origin - 1] = '\0';
  m.cmd.user[sizeof m.cmd.user - 1] = '\0';

  ck_case_state_t view[CK_MAX_CASES];
  ck_case_view(g_ps, g_as, view);
  ck_decision_t d;
  ck_gov_action_t act;
  const bool ok = ck_gov_decide(&g_gov, &m.cmd, view, ck_sim_now(g_ps), &d, &act);
  apply_and_audit(&d, &act, ok);
  save_state();
  MsgReply(ctp->rcvid, EOK, &d, sizeof d);
  return 0;
}

static int on_tick(message_context_t *ctp, int code, unsigned flags, void *handle) {
  const ck_case_cmd_t hb = {.type = CASE_CMD_HEARTBEAT};
  for (uint16_t i = 1; i <= g_n; i++) case_devctl(i, &hb);

  const uint64_t now = ck_sim_now(g_ps);
  if (now < g_next_gov_tick_sim) return 0;
  g_next_gov_tick_sim = now + CK_NS_PER_S;
  ck_case_state_t view[CK_MAX_CASES];
  ck_case_view(g_ps, g_as, view);
  ck_gov_action_t acts[3 * CK_MAX_CASES];
  ck_decision_t decs[3 * CK_MAX_CASES];
  const int k = ck_gov_tick(&g_gov, view, now, acts, decs, 3 * CK_MAX_CASES);
  for (int i = 0; i < k; i++) apply_and_audit(&decs[i], &acts[i], 1);
  if (k > 0) save_state();
  return 0;
}

static int status_read(resmgr_context_t *ctp, io_read_t *msg, RESMGR_OCB_T *ocb) {
  int status = iofunc_read_verify(ctp, msg, ocb, NULL);
  if (status != EOK) return status;
  if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) return ENOSYS;
  static char buf[8192];
  int len = ck_gov_status_text(&g_gov, buf, sizeof buf);
  if (len >= (int)sizeof buf) len = (int)sizeof buf - 1;
  len += snprintf(buf + len, sizeof buf - (size_t)len, "rejected senders on %s: %llu\n", CK_DEV_GOV,
                  (unsigned long long)g_rejected);
  if (len >= (int)sizeof buf) len = (int)sizeof buf - 1;
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
  /* 1. the custom ability that guards the plant; only this process will keep it */
  int able = procmgr_ability_create(CK_ABILITY_PLANT_WRITE, 0);
  if (able < 0) able = procmgr_ability_lookup(CK_ABILITY_PLANT_WRITE);
  if (able < 0) { ck_log(PROC, "custom ability: %s", strerror(errno)); return 1; }

  g_ps = ck_map_plant(0, 30);
  g_as = ck_map_act(0, 30);
  if (!g_ps || !g_as) { ck_log(PROC, "plant shared memory not available"); return 1; }
  g_n = g_ps->ncases;
  for (uint16_t i = 0; i < g_n; i++) {
    char path[64];
    snprintf(path, sizeof path, CK_DEV_CASE_FMT, i + 1);
    g_case_fd[i] = ck_open_retry(path, O_RDWR, 30);
    if (g_case_fd[i] < 0) { ck_log(PROC, "open %s: %s", path, strerror(errno)); return 1; }
  }
  g_audit_fd = ck_open_retry(CK_DEV_AUDIT, O_RDONLY, 30);
  if (g_audit_fd < 0) ck_log(PROC, "warning: auditd not reachable; decisions will not be journalled");

  /* 2. state: restore after a restart, else start from the compiled-in envelope */
  mkdir(CK_DATA_DIR, 0755);
  mkdir(CK_GOV_DIR, 0700);
  if (chown(CK_GOV_DIR, CK_UID_GOV, CK_UID_GOV) != 0) ck_log(PROC, "warning: chown %s: %s", CK_GOV_DIR, strerror(errno));
  ck_case_type_t types[CK_MAX_CASES];
  for (uint16_t i = 0; i < g_n; i++) types[i] = (ck_case_type_t)g_ps->types[i];
  const int restored = load_state() == 0;
  if (!restored) {
    ck_envelope_t env;
    ck_envelope_default(&env);
    ck_gov_init(&g_gov, &env, g_n, types, ck_sim_now(g_ps));
  }

  /* 3. command channel (brokerd only) and read-only status */
  dispatch_t *dpp = dispatch_create();
  iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &gov_connect, _RESMGR_IO_NFUNCS, &gov_io);
  iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &status_connect, _RESMGR_IO_NFUNCS, &status_io);
  status_io.read = status_read;
  iofunc_attr_init(&gov_attr, S_IFNAM | 0600, NULL, NULL);
  gov_attr.uid = CK_UID_BROKER;
  gov_attr.gid = CK_UID_BROKER;
  iofunc_attr_init(&status_attr, S_IFNAM | 0444, NULL, NULL);
  resmgr_attr_t rattr;
  memset(&rattr, 0, sizeof rattr);
  rattr.nparts_max = 1;
  rattr.msg_max_size = 4096;
  if (!dpp ||
      resmgr_attach(dpp, &rattr, CK_DEV_GOV, _FTYPE_ANY, 0, &gov_connect, &gov_io, &gov_attr) == -1 ||
      resmgr_attach(dpp, &rattr, CK_DEV_STATUS, _FTYPE_ANY, 0, &status_connect, &status_io, &status_attr) == -1 ||
      message_attach(dpp, NULL, CK_MSG_GOV, CK_MSG_GOV, on_cmd, NULL) == -1) {
    ck_log(PROC, "attach: %s", strerror(errno));
    return 1;
  }
  const int code = pulse_attach(dpp, MSG_FLAG_ALLOC_PULSE, 0, on_tick, NULL);
  const int coid = message_connect(dpp, MSG_FLAG_SIDE_CHANNEL);
  struct sigevent ev;
  SIGEV_PULSE_INIT(&ev, coid, CK_PRIO_GOV, code, 0);
  timer_t tid;
  struct itimerspec its = {.it_value = {0, 250000000}, .it_interval = {0, 250000000}};
  if (code == -1 || coid == -1 || timer_create(CLOCK_MONOTONIC, &ev, &tid) != 0 ||
      timer_settime(tid, 0, &its, NULL) != 0) {
    ck_log(PROC, "timer: %s", strerror(errno));
    return 1;
  }

  /* 4. drop root, keeping only the plant-write ability */
  if (ck_set_fifo_prio(CK_PRIO_GOV) != EOK) ck_log(PROC, "warning: could not set priority");
  const unsigned keep = (unsigned)able;
  if (ck_drop_to(CK_UID_GOV, &keep, 1, 0) != 0) { ck_log(PROC, "drop privileges: %s", strerror(errno)); return 1; }
  ck_log(PROC, "governing %u cases; ability %s=%d kept; state %s; uid %d", g_n, CK_ABILITY_PLANT_WRITE,
         able, restored ? "restored" : "fresh", (int)getuid());
  ck_audit_event(g_audit_fd, ck_sim_now(g_ps), "governord started (%s state)", restored ? "restored" : "fresh");

  dispatch_context_t *ctp = dispatch_context_alloc(dpp);
  for (;;) {
    if ((ctp = dispatch_block(ctp)) == NULL) { ck_log(PROC, "dispatch_block: %s", strerror(errno)); return 1; }
    dispatch_handler(ctp);
  }
}
