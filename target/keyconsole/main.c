/* SPDX-License-Identifier: Apache-2.0
 *
 * keyconsole — the store's local "key switch". Dangerous commands (alarm
 * changes, envelope changes, defrost schedule) are never applied from the
 * network: brokerd queues them here and only someone at the store can approve.
 *
 * /dev/coldkeep/key   owner root, mode 0600: the local operator's console
 *                     read  -> the request list
 *                     write "approve <id>" or "reject <id>" (root senders only)
 * /dev/coldkeep/keyq  owner brokerd's uid, mode 0600: CK_MSG_KEYREQ from brokerd
 *
 * keyconsole creates the custom ability "coldkeep.envelope.write" and is the only
 * process that keeps it. governord applies an approved command only if the
 * sender is keyconsole's uid AND holds that ability, so a compromised network
 * process cannot approve its own request, whatever path it finds to governord.
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
#include <unistd.h>

#include "ck_target.h"

#define PROC "keyconsole"
#define MAXREQ 64

typedef enum { REQ_PENDING, REQ_APPROVED, REQ_REJECTED, REQ_FAILED } req_state_t;
static const char *const k_state[] = {"pending", "approved", "rejected", "failed"};

typedef struct {
  uint64_t id;
  ck_cmd_t cmd;
  uint64_t t_ns;
  req_state_t state;
  char result[96];
} keyreq_t;

static keyreq_t g_req[MAXREQ];
static unsigned g_n;
static uint64_t g_next_id = 1;
static ck_plant_shm_t *g_ps;
static int g_audit_fd = -1, g_gov_fd = -1;

static resmgr_connect_funcs_t key_connect, q_connect;
static resmgr_io_funcs_t key_io, q_io;
static iofunc_attr_t key_attr, q_attr;

static keyreq_t *find(uint64_t id) {
  for (unsigned i = 0; i < g_n; i++)
    if (g_req[i].id == id) return &g_req[i];
  return NULL;
}

/* brokerd -> keyconsole: queue a dangerous command, reply with its request id */
static int on_keyreq(message_context_t *ctp, int type, unsigned flags, void *handle) {
  const int uid = ck_sender_uid(ctp->info.scoid);
  if (uid != CK_UID_BROKER && uid != 0) { MsgError(ctp->rcvid, EACCES); return 0; }
  if (ctp->info.msglen < (ssize_t)sizeof(ck_msg_cmd_t)) { MsgError(ctp->rcvid, EINVAL); return 0; }
  ck_msg_cmd_t m;
  memcpy(&m, ctp->msg, sizeof m);
  m.cmd.origin[sizeof m.cmd.origin - 1] = '\0';
  m.cmd.user[sizeof m.cmd.user - 1] = '\0';
  if (g_n == MAXREQ) { /* drop the oldest finished request, else refuse */
    unsigned k = 0;
    while (k < g_n && g_req[k].state == REQ_PENDING) k++;
    if (k == g_n) { MsgError(ctp->rcvid, ENOSPC); return 0; }
    memmove(&g_req[k], &g_req[k + 1], (g_n - k - 1) * sizeof g_req[0]);
    g_n--;
  }
  keyreq_t *r = &g_req[g_n++];
  memset(r, 0, sizeof *r);
  r->id = g_next_id++;
  r->cmd = m.cmd;
  r->t_ns = ck_sim_now(g_ps);
  r->state = REQ_PENDING;
  ck_audit_event(g_audit_fd, r->t_ns, "key request #%llu queued: %s case=%u from %s/%s; approve at the local console",
                 (unsigned long long)r->id, ck_cmd_type_name(m.cmd.type), m.cmd.case_id, m.cmd.origin, m.cmd.user);
  MsgReply(ctp->rcvid, EOK, &r->id, sizeof r->id);
  return 0;
}

static int send_approval(keyreq_t *r) {
  ck_msg_approved_t m;
  memset(&m, 0, sizeof m);
  m.hdr.type = CK_MSG_APPROVED;
  m.approval_id = r->id;
  m.cmd = r->cmd;
  ck_strlcpy(m.cmd.origin, "keyconsole", sizeof m.cmd.origin);
  ck_decision_t d;
  for (int attempt = 0; attempt < 2; attempt++) {
    if (g_gov_fd < 0) g_gov_fd = open(CK_DEV_GOVKEY, O_RDWR);
    if (g_gov_fd >= 0 && MsgSend(g_gov_fd, &m, sizeof m, &d, sizeof d) != -1) {
      snprintf(r->result, sizeof r->result, "%.12s %.12s %.60s", ck_verdict_name(d.verdict), ck_rule_name(d.rule_id), d.reason);
      return d.verdict == VERDICT_ALLOW ? 0 : -1;
    }
    if (g_gov_fd >= 0) close(g_gov_fd); /* governord restarted: reconnect once */
    g_gov_fd = -1;
  }
  snprintf(r->result, sizeof r->result, "governor unavailable: %s", strerror(errno));
  return -1;
}

static int sender_is_root(resmgr_context_t *ctp) {
  struct _client_info *info = NULL;
  if (iofunc_client_info_ext(ctp, 0, &info, 0) != EOK || !info) return 0;
  const int root = info->cred.euid == 0;
  iofunc_client_info_ext_free(&info);
  return root;
}

static int key_write(resmgr_context_t *ctp, io_write_t *msg, RESMGR_OCB_T *ocb) {
  int status = iofunc_write_verify(ctp, msg, ocb, NULL);
  if (status != EOK) return status;
  if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) return ENOSYS;
  if (!sender_is_root(ctp)) return EPERM; /* the file is 0600 root already; check again */
  char text[64];
  const size_t n = msg->i.nbytes < sizeof text - 1 ? msg->i.nbytes : sizeof text - 1;
  if (resmgr_msgread(ctp, text, n, sizeof msg->i) == -1) return errno;
  text[n] = '\0';
  unsigned long long id = 0;
  int approve;
  if (sscanf(text, "approve %llu", &id) == 1) approve = 1;
  else if (sscanf(text, "reject %llu", &id) == 1) approve = 0;
  else return EINVAL;
  keyreq_t *r = find(id);
  if (!r) return ENOENT;
  if (r->state != REQ_PENDING) return EALREADY;
  const uint64_t now = ck_sim_now(g_ps);
  if (approve) {
    r->state = send_approval(r) == 0 ? REQ_APPROVED : REQ_FAILED;
    ck_audit_event(g_audit_fd, now, "key request #%llu %s at the local console: %s", id,
                   r->state == REQ_APPROVED ? "approved" : "approval failed", r->result);
  } else {
    r->state = REQ_REJECTED;
    ck_strlcpy(r->result, "rejected at the local console", sizeof r->result);
    ck_audit_event(g_audit_fd, now, "key request #%llu rejected at the local console", id);
  }
  _IO_SET_WRITE_NBYTES(ctp, msg->i.nbytes);
  return _RESMGR_NPARTS(0);
}

static int key_read(resmgr_context_t *ctp, io_read_t *msg, RESMGR_OCB_T *ocb) {
  int status = iofunc_read_verify(ctp, msg, ocb, NULL);
  if (status != EOK) return status;
  if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) return ENOSYS;
  static char buf[MAXREQ * 220 + 160];
  int len = snprintf(buf, sizeof buf, "# key requests (write \"approve <id>\" or \"reject <id>\" here)\n");
  for (unsigned i = 0; i < g_n && len < (int)sizeof buf - 220; i++) {
    const keyreq_t *r = &g_req[i];
    len += snprintf(buf + len, sizeof buf - (size_t)len, "#%llu t=%.0f %-8s %s case=%u value=%.1f from %s/%s%s%s\n",
                    (unsigned long long)r->id, (double)r->t_ns / 1e9, k_state[r->state],
                    ck_cmd_type_name(r->cmd.type), r->cmd.case_id, (double)r->cmd.value, r->cmd.origin, r->cmd.user,
                    r->result[0] ? " -> " : "", r->result);
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
  /* the ability governord requires for approvals; only this process keeps it */
  int able = procmgr_ability_create(CK_ABILITY_ENVELOPE_WRITE, 0);
  if (able < 0) able = procmgr_ability_lookup(CK_ABILITY_ENVELOPE_WRITE);
  if (able < 0) { ck_log(PROC, "custom ability: %s", strerror(errno)); return 1; }
  g_ps = ck_map_plant(0, 30);
  if (!g_ps) { ck_log(PROC, "plant shared memory not available"); return 1; }
  g_audit_fd = ck_open_retry(CK_DEV_AUDIT, O_RDONLY, 30);

  dispatch_t *dpp = dispatch_create();
  iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &key_connect, _RESMGR_IO_NFUNCS, &key_io);
  iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &q_connect, _RESMGR_IO_NFUNCS, &q_io);
  key_io.read = key_read;
  key_io.write = key_write;
  iofunc_attr_init(&key_attr, S_IFNAM | 0600, NULL, NULL); /* owner root */
  key_attr.uid = 0;
  key_attr.gid = 0;
  iofunc_attr_init(&q_attr, S_IFNAM | 0600, NULL, NULL);
  q_attr.uid = CK_UID_BROKER;
  q_attr.gid = CK_UID_BROKER;
  resmgr_attr_t rattr;
  memset(&rattr, 0, sizeof rattr);
  rattr.nparts_max = 1;
  rattr.msg_max_size = 4096;
  if (!dpp ||
      resmgr_attach(dpp, &rattr, CK_DEV_KEY, _FTYPE_ANY, 0, &key_connect, &key_io, &key_attr) == -1 ||
      resmgr_attach(dpp, &rattr, CK_DEV_KEYQ, _FTYPE_ANY, 0, &q_connect, &q_io, &q_attr) == -1 ||
      message_attach(dpp, NULL, CK_MSG_KEYREQ, CK_MSG_KEYREQ, on_keyreq, NULL) == -1) {
    ck_log(PROC, "attach: %s", strerror(errno));
    return 1;
  }
  if (ck_set_fifo_prio(CK_PRIO_KEY) != EOK) ck_log(PROC, "warning: could not set priority");
  const unsigned keep = (unsigned)able;
  if (ck_drop_to(CK_UID_KEY, &keep, 1, 0) != 0) { ck_log(PROC, "drop privileges: %s", strerror(errno)); return 1; }
  ck_log(PROC, "store key console on %s; ability %s=%d kept; uid %d", CK_DEV_KEY, CK_ABILITY_ENVELOPE_WRITE, able,
         (int)getuid());

  dispatch_context_t *ctp = dispatch_context_alloc(dpp);
  for (;;) {
    if ((ctp = dispatch_block(ctp)) == NULL) { ck_log(PROC, "dispatch_block: %s", strerror(errno)); return 1; }
    dispatch_handler(ctp);
  }
}
