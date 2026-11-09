/* SPDX-License-Identifier: Apache-2.0
 *
 * casectl — one case controller (core/control.c) as a resource manager.
 *
 *   casectl -n <case id> [-L]
 *
 * /dev/coldkeep/case/N
 *   read    current state as text (anyone)
 *   devctl  DCMD_CK_CASE with a ck_case_cmd_t
 *   write   text commands (legacy mode only): setpoint <c> | defrost <s> | stop | raw <0|1>
 *
 * Protected mode: every devctl and write must come from a client holding the
 * custom ability "coldkeep.plant.write" (only governord has it), else EPERM.
 * The controller also expects governor heartbeats and holds a safe configuration
 * (G6) if they stop. -L (legacy) disables both: anyone who can open the file
 * controls the case, which models a flat architecture.
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

#define PROC "casectl"

static resmgr_connect_funcs_t connect_funcs;
static resmgr_io_funcs_t io_funcs;
static iofunc_attr_t attr;

static uint16_t g_id;
static int g_legacy;
static int g_able_id = -1;
static ck_ctl_t g_ctl;
static ck_case_state_t g_out;
static ck_plant_shm_t *g_ps;
static ck_act_shm_t *g_as;
static uint64_t g_period_ns, g_last_tick_wall, g_jit_window_start;
static uint64_t g_jit_sum, g_jit_n, g_jit_max;

static void publish_slot(void) {
  ck_act_slot_t *a = &g_as->a[g_id - 1];
  atomic_fetch_add_explicit(&a->seq, 1, memory_order_acq_rel);
  a->compressor_on = g_out.compressor_on;
  a->heater_on = g_out.heater_on;
  a->fans_on = g_out.fans_on;
  a->defrost_on = ck_ctl_in_defrost(&g_ctl) ? 1 : 0;
  a->hold = g_ctl.hold ? 1 : 0;
  a->setpoint_c = g_ctl.setpoint_c;
  a->alive = 1;
  a->ticks++;
  atomic_fetch_add_explicit(&a->seq, 1, memory_order_acq_rel);
}

static int client_has_plant_write(resmgr_context_t *ctp) {
  if (g_able_id < 0) g_able_id = procmgr_ability_lookup(CK_ABILITY_PLANT_WRITE);
  if (g_able_id < 0) return 0; /* governor has not created it: fail closed */
  struct _client_info *info = NULL;
  struct _client_able able;
  memset(&able, 0, sizeof able);
  able.ability = (unsigned)g_able_id;
  if (iofunc_client_info_able(ctp, 0, &info, 0, &able, 1) != EOK) return 0;
  const int has = able.flags != 0; /* verified: 0x1 when held, 0x0 otherwise */
  iofunc_client_info_ext_free(&info);
  return has;
}

static int apply(const ck_case_cmd_t *cmd) {
  const int rc = ck_ctl_command(&g_ctl, cmd, ck_sim_now(g_ps));
  publish_slot(); /* the governor sees a started defrost before its reply arrives */
  return rc;
}

static int io_devctl(resmgr_context_t *ctp, io_devctl_t *msg, RESMGR_OCB_T *ocb) {
  int status = iofunc_devctl_default(ctp, msg, ocb);
  if (status != _RESMGR_DEFAULT) return status;
  if (msg->i.dcmd != DCMD_CK_CASE) return ENOSYS;
  if (!g_legacy && !client_has_plant_write(ctp)) return EPERM;
  if (msg->i.nbytes < sizeof(ck_case_cmd_t)) return EINVAL;
  ck_case_cmd_t cmd;
  memcpy(&cmd, _DEVCTL_DATA(msg->i), sizeof cmd);
  const int rc = apply(&cmd);
  if (rc != 0) return rc;
  memset(&msg->o, 0, sizeof msg->o);
  return _RESMGR_PTR(ctp, &msg->o, sizeof msg->o);
}

static int render(char *buf, size_t cap) {
  const ck_shm_case_t *c = &g_ps->c[g_id - 1];
  return snprintf(buf, cap,
                  "case %u %s mode=%s air=%.2f product=%.2f evap=%.2f setpoint=%.1f "
                  "compressor=%u heater=%u fans=%u defrost=%u hold=%u\n",
                  g_id, ck_case_type_name(g_ctl.type), g_legacy ? "legacy" : "protected",
                  (double)c->air_c, (double)c->product_c, (double)c->evap_c, (double)g_ctl.setpoint_c,
                  g_out.compressor_on, g_out.heater_on, g_out.fans_on,
                  ck_ctl_in_defrost(&g_ctl) ? 1u : 0u, g_ctl.hold ? 1u : 0u);
}

static int io_read(resmgr_context_t *ctp, io_read_t *msg, RESMGR_OCB_T *ocb) {
  int status = iofunc_read_verify(ctp, msg, ocb, NULL);
  if (status != EOK) return status;
  if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) return ENOSYS;
  char buf[512];
  const int len = render(buf, sizeof buf);
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

static int io_write(resmgr_context_t *ctp, io_write_t *msg, RESMGR_OCB_T *ocb) {
  int status = iofunc_write_verify(ctp, msg, ocb, NULL);
  if (status != EOK) return status;
  if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) return ENOSYS;
  if (!g_legacy && !client_has_plant_write(ctp)) return EPERM;
  if (!g_legacy) return EINVAL; /* the governor uses devctl, never text */
  char text[128];
  size_t n = msg->i.nbytes < sizeof text - 1 ? msg->i.nbytes : sizeof text - 1;
  if (resmgr_msgread(ctp, text, n, sizeof msg->i) == -1) return errno;
  text[n] = '\0';
  ck_case_cmd_t cmd;
  memset(&cmd, 0, sizeof cmd);
  float v = 0;
  if (sscanf(text, "setpoint %f", &v) == 1) { cmd.type = CASE_CMD_SET_SETPOINT; cmd.setpoint_c = v; }
  else if (sscanf(text, "defrost %f", &v) == 1) { cmd.type = CASE_CMD_START_DEFROST; cmd.duration_s = (uint32_t)v; cmd.term_c = CK_TERM_NONE; }
  else if (!strncmp(text, "stop", 4)) cmd.type = CASE_CMD_STOP_DEFROST;
  else if (sscanf(text, "raw %f", &v) == 1) { cmd.type = CASE_CMD_RAW_ACTUATORS; cmd.raw_override = v != 0; cmd.raw_heater = v != 0; }
  else return EINVAL;
  const int rc = apply(&cmd);
  if (rc != 0) return rc;
  _IO_SET_WRITE_NBYTES(ctp, msg->i.nbytes);
  return _RESMGR_NPARTS(0);
}

static int on_tick(message_context_t *ctp, int code, unsigned flags, void *handle) {
  const uint64_t wall = ck_wall_ns();
  if (g_last_tick_wall) {
    const uint64_t gap = wall - g_last_tick_wall;
    const uint64_t jit = gap > g_period_ns ? gap - g_period_ns : g_period_ns - gap;
    g_jit_sum += jit; g_jit_n++;
    if (jit > g_jit_max) g_jit_max = jit;
  }
  g_last_tick_wall = wall;
  if (wall - g_jit_window_start >= CK_NS_PER_S) {
    ck_act_slot_t *a = &g_as->a[g_id - 1];
    a->jitter_us_max = (uint32_t)(g_jit_max / 1000);
    a->jitter_us_avg = g_jit_n ? (uint32_t)(g_jit_sum / g_jit_n / 1000) : 0;
    g_jit_sum = g_jit_n = g_jit_max = 0;
    g_jit_window_start = wall;
  }
  ck_plant_shm_t snap;
  ck_plant_read(g_ps, &snap);
  const ck_probe_t probe = {snap.c[g_id - 1].air_c, snap.c[g_id - 1].product_c, snap.c[g_id - 1].evap_c};
  ck_ctl_tick(&g_ctl, &probe, snap.sim_ns, &g_out);
  publish_slot();
  return 0;
}

int main(int argc, char **argv) {
  int opt;
  while ((opt = getopt(argc, argv, "n:L")) != -1) {
    if (opt == 'n') g_id = (uint16_t)atoi(optarg);
    else if (opt == 'L') g_legacy = 1;
    else { fprintf(stderr, "usage: casectl -n <case> [-L]\n"); return 2; }
  }
  g_ps = ck_map_plant(0, 30);
  g_as = ck_map_act(1, 30);
  if (!g_ps || !g_as) { ck_log(PROC, "plant shared memory not available: %s", strerror(errno)); return 1; }
  if (g_id < 1 || g_id > g_ps->ncases) { ck_log(PROC, "bad case id %u", g_id); return 2; }

  const ck_case_type_t type = (ck_case_type_t)g_ps->types[g_id - 1];
  ck_ctl_init(&g_ctl, g_id, type, !g_legacy, ck_sim_now(g_ps));
  /* G6 timeout: 2 s of wall time, whatever the sim speed */
  g_ctl.hb_timeout_ns = (uint64_t)(2.0 * (double)g_ps->speed * (double)CK_NS_PER_S);
  g_out.fans_on = 1;

  dispatch_t *dpp = dispatch_create();
  iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &connect_funcs, _RESMGR_IO_NFUNCS, &io_funcs);
  io_funcs.devctl = io_devctl;
  io_funcs.read = io_read;
  io_funcs.write = io_write;
  iofunc_attr_init(&attr, S_IFNAM | 0666, NULL, NULL);
  resmgr_attr_t rattr;
  memset(&rattr, 0, sizeof rattr);
  rattr.nparts_max = 1;
  rattr.msg_max_size = 2048;
  char path[64];
  snprintf(path, sizeof path, CK_DEV_CASE_FMT, g_id);
  if (!dpp || resmgr_attach(dpp, &rattr, path, _FTYPE_ANY, 0, &connect_funcs, &io_funcs, &attr) == -1) {
    ck_log(PROC, "resmgr_attach %s: %s", path, strerror(errno));
    return 1;
  }

  /* control loop: 100 ms of sim time per tick */
  const int code = pulse_attach(dpp, MSG_FLAG_ALLOC_PULSE, 0, on_tick, NULL);
  const int coid = message_connect(dpp, MSG_FLAG_SIDE_CHANNEL);
  struct sigevent ev;
  SIGEV_PULSE_INIT(&ev, coid, CK_PRIO_CASE, code, 0);
  timer_t tid;
  g_period_ns = (uint64_t)((double)CK_TICK_NS / (double)g_ps->speed);
  if (g_period_ns < 1000000) g_period_ns = 1000000;
  struct itimerspec its = {
    .it_value = {0, (long)g_period_ns}, .it_interval = {0, (long)g_period_ns}};
  if (code == -1 || coid == -1 || timer_create(CLOCK_MONOTONIC, &ev, &tid) != 0 ||
      timer_settime(tid, 0, &its, NULL) != 0) {
    ck_log(PROC, "timer: %s", strerror(errno));
    return 1;
  }

  if (ck_set_fifo_prio(CK_PRIO_CASE) != EOK) ck_log(PROC, "warning: could not set priority");
  if (ck_drop_to(CK_UID_CASE, NULL, 0, 0) != 0) { ck_log(PROC, "drop privileges: %s", strerror(errno)); return 1; }
  ck_log(PROC, "case %u (%s) %s at %s, period %.2f ms, uid %d", g_id, ck_case_type_name(type),
         g_legacy ? "LEGACY (no checks)" : "protected", path, (double)g_period_ns / 1e6, (int)getuid());

  dispatch_context_t *ctp = dispatch_context_alloc(dpp);
  for (;;) {
    if ((ctp = dispatch_block(ctp)) == NULL) { ck_log(PROC, "dispatch_block: %s", strerror(errno)); return 1; }
    dispatch_handler(ctp);
  }
}
