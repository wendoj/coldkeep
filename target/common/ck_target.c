/* SPDX-License-Identifier: Apache-2.0 — helpers shared by the COLDKEEP QNX processes */
#include "ck_target.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/neutrino.h>
#include <sys/procmgr.h>
#include <time.h>
#include <unistd.h>

void ck_log(const char *proc, const char *fmt, ...) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  fprintf(stderr, "[%5lld.%03ld] %s: ", (long long)ts.tv_sec, ts.tv_nsec / 1000000, proc);
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
}

int ck_set_fifo_prio(int prio) {
  struct sched_param sp = {.sched_priority = prio};
  return pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
}

int ck_drop_to(uid_t uid, const unsigned *keep, int nkeep, int deny_rest) {
  /* Same call shapes as verified on QNX 8.0 (docs/QNX_FEATURES.md, questions 1-2):
   * keep + deny-rest go in ONE call, so the deny of "everything else" does not
   * touch the ability that was just allowed and locked. */
  const unsigned allow = PROCMGR_ADN_NONROOT | PROCMGR_AOP_ALLOW | PROCMGR_AOP_LOCK;
  const unsigned deny_eol = PROCMGR_ADN_NONROOT | PROCMGR_AOP_DENY | PROCMGR_AOP_LOCK | PROCMGR_AID_EOL;
  int rc = EOK;
  if (nkeep > 1) return -1; /* COLDKEEP needs at most one kept ability */
  if (nkeep == 1)
    rc = deny_rest ? procmgr_ability(0, allow | keep[0], deny_eol)
                   : procmgr_ability(0, allow | keep[0], PROCMGR_AID_EOL);
  else if (deny_rest)
    rc = procmgr_ability(0, deny_eol);
  if (rc != EOK) return -1;
  if (setgroups(0, NULL) != 0 || setgid((gid_t)uid) != 0 || setuid(uid) != 0) return -1;
  if (deny_rest &&
      procmgr_ability(0, PROCMGR_ADN_ROOT | PROCMGR_AOP_DENY | PROCMGR_AOP_LOCK | PROCMGR_AID_EOL) != EOK)
    return -1;
  return 0;
}

uint64_t ck_wall_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * CK_NS_PER_S + (uint64_t)ts.tv_nsec;
}

static void *map_shm(const char *name, size_t size, int writable, int wait_s) {
  for (int i = 0; i <= wait_s * 10; i++) {
    int fd = shm_open(name, writable ? O_RDWR : O_RDONLY, 0);
    if (fd >= 0) {
      void *p = mmap(NULL, size, writable ? PROT_READ | PROT_WRITE : PROT_READ, MAP_SHARED, fd, 0);
      close(fd);
      if (p != MAP_FAILED) return p;
      return NULL;
    }
    if (wait_s == 0) break;
    usleep(100000);
  }
  return NULL;
}

ck_plant_shm_t *ck_map_plant(int writable, int wait_s) {
  ck_plant_shm_t *p = map_shm(CK_SHM_PLANT, sizeof *p, writable, wait_s);
  for (int i = 0; p && p->magic != CK_SHM_MAGIC && i < wait_s * 10; i++) usleep(100000);
  return (p && p->magic == CK_SHM_MAGIC) ? p : NULL;
}

ck_act_shm_t *ck_map_act(int writable, int wait_s) {
  ck_act_shm_t *p = map_shm(CK_SHM_ACT, sizeof *p, writable, wait_s);
  for (int i = 0; p && p->magic != CK_SHM_MAGIC && i < wait_s * 10; i++) usleep(100000);
  return (p && p->magic == CK_SHM_MAGIC) ? p : NULL;
}

void ck_plant_read(const ck_plant_shm_t *s, ck_plant_shm_t *out) {
  for (;;) {
    uint32_t a = atomic_load_explicit(&s->seq, memory_order_acquire);
    if (a & 1u) continue;
    memcpy((void *)out, (const void *)s, sizeof *out);
    atomic_thread_fence(memory_order_acquire);
    if (atomic_load_explicit(&s->seq, memory_order_relaxed) == a) return;
  }
}

uint64_t ck_sim_now(const ck_plant_shm_t *s) {
  ck_plant_shm_t snap;
  ck_plant_read(s, &snap);
  return snap.sim_ns;
}

void ck_case_view(const ck_plant_shm_t *ps, const ck_act_shm_t *as, ck_case_state_t *v) {
  ck_plant_shm_t snap;
  ck_plant_read(ps, &snap);
  for (uint16_t i = 0; i < snap.ncases; i++) {
    memset(&v[i], 0, sizeof v[i]);
    v[i].id = (uint16_t)(i + 1);
    v[i].type = (ck_case_type_t)snap.types[i];
    v[i].air_c = snap.c[i].air_c;
    v[i].product_c = snap.c[i].product_c;
    v[i].evap_c = snap.c[i].evap_c;
    v[i].door_open = snap.c[i].door_open;
    const ck_act_slot_t *a = &as->a[i];
    v[i].compressor_on = a->compressor_on;
    v[i].heater_on = a->heater_on;
    v[i].fans_on = a->fans_on;
    v[i].defrost_on = a->defrost_on;
    v[i].setpoint_c = a->setpoint_c;
  }
}

int ck_open_retry(const char *path, int flags, int wait_s) {
  for (int i = 0; i <= wait_s * 10; i++) {
    int fd = open(path, flags);
    if (fd >= 0) return fd;
    if (errno != ENOENT) return -1;
    usleep(100000);
  }
  return -1;
}

int ck_audit_send(int fd, const ck_jrec_t *rec) {
  if (fd < 0) return -1;
  ck_msg_audit_t m;
  memset(&m, 0, sizeof m);
  m.hdr.type = CK_MSG_AUDIT;
  m.rec = *rec;
  return MsgSend(fd, &m, sizeof m, NULL, 0) == -1 ? -1 : 0;
}

int ck_audit_decision(int fd, const ck_decision_t *d, uint64_t sim_ns) {
  ck_jrec_t r;
  memset(&r, 0, sizeof r);
  r.kind = JREC_DECISION;
  r.t_ns = sim_ns;
  r.d = *d;
  return ck_audit_send(fd, &r);
}

int ck_audit_event(int fd, uint64_t sim_ns, const char *fmt, ...) {
  ck_jrec_t r;
  memset(&r, 0, sizeof r);
  r.kind = JREC_EVENT;
  r.t_ns = sim_ns;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(r.text, sizeof r.text, fmt, ap);
  va_end(ap);
  return ck_audit_send(fd, &r);
}

int ck_sender_uid(int scoid) {
  struct _client_info info;
  if (ConnectClientInfo(scoid, &info, 0) == -1) return -1;
  return (int)info.cred.euid;
}
