/* valved: a resource manager for /dev/valve that only takes writes from
 * processes holding the custom ability "demo.valve.write".
 * Run as root:  ./valved &      (-o: check the ability at open only) */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/iofunc.h>
#include <sys/procmgr.h>
#include <sys/stat.h>

#define ABILITY "demo.valve.write"

static int valve_id;
static int open_only;

/* Ask the kernel who sent this message and whether it holds our ability. */
static int holds_ability(resmgr_context_t *ctp, uid_t *uid) {
  struct _client_info *info = NULL;
  struct _client_able able = { .ability = (unsigned)valve_id };
  *uid = (uid_t)-1;
  if (iofunc_client_info_able(ctp, 0, &info, 0, &able, 1) != EOK) return 0;
  *uid = info->cred.euid;
  iofunc_client_info_ext_free(&info);
  return able.flags != 0;
}

static int valve_open(resmgr_context_t *ctp, io_open_t *msg,
                      RESMGR_HANDLE_T *handle, void *extra) {
  uid_t uid;
  if (open_only && !holds_ability(ctp, &uid)) {
    printf("valved: refused open from uid %d\n", (int)uid);
    return EPERM;
  }
  return iofunc_open_default(ctp, msg, handle, extra);
}

static int valve_write(resmgr_context_t *ctp, io_write_t *msg, RESMGR_OCB_T *ocb) {
  int status = iofunc_write_verify(ctp, msg, ocb, NULL);
  if (status != EOK) return status;
  if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) return ENOSYS;

  char cmd[64] = "";
  size_t n = msg->i.nbytes < sizeof cmd - 1 ? msg->i.nbytes : sizeof cmd - 1;
  resmgr_msgread(ctp, cmd, n, sizeof msg->i);
  cmd[strcspn(cmd, "\n")] = '\0';

  uid_t uid;
  int ok = holds_ability(ctp, &uid);
  if (!ok && !open_only) {
    printf("valved: refused '%s' from uid %d\n", cmd, (int)uid);
    return EPERM;
  }
  printf("valved: accepted '%s' from uid %d\n", cmd, (int)uid);
  _IO_SET_WRITE_NBYTES(ctp, msg->i.nbytes);
  return _RESMGR_NPARTS(0);
}

int main(int argc, char **argv) {
  static resmgr_connect_funcs_t connect_funcs;
  static resmgr_io_funcs_t io_funcs;
  static iofunc_attr_t attr;

  open_only = argc > 1 && strcmp(argv[1], "-o") == 0;
  setvbuf(stdout, NULL, _IOLBF, 0);

  valve_id = procmgr_ability_create(ABILITY, 0);
  if (valve_id < 0) valve_id = procmgr_ability_lookup(ABILITY); /* made by an earlier run */
  if (valve_id < 0) { printf("valved: no ability id (%d)\n", valve_id); return 1; }
  printf("valved: %s is ability %d\n", ABILITY, valve_id);

  dispatch_t *dpp = dispatch_create();
  iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &connect_funcs, _RESMGR_IO_NFUNCS, &io_funcs);
  connect_funcs.open = valve_open;
  io_funcs.write = valve_write;
  iofunc_attr_init(&attr, S_IFNAM | 0666, NULL, NULL);
  if (resmgr_attach(dpp, NULL, "/dev/valve", _FTYPE_ANY, 0,
                    &connect_funcs, &io_funcs, &attr) == -1) {
    perror("valved: resmgr_attach");
    return 1;
  }
  printf("valved: serving /dev/valve%s\n", open_only ? " (checking at open only)" : "");

  dispatch_context_t *ctp = dispatch_context_alloc(dpp);
  for (;;) {
    if ((ctp = dispatch_block(ctp)) == NULL) return 1;
    dispatch_handler(ctp);
  }
}
