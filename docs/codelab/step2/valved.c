/* valved, step 2 of the codelab: a resource manager for /dev/valve that accepts every write. */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/iofunc.h>
#include <sys/stat.h>

static int valve_write(resmgr_context_t *ctp, io_write_t *msg, RESMGR_OCB_T *ocb) {
  int status = iofunc_write_verify(ctp, msg, ocb, NULL);
  if (status != EOK) return status;
  if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) return ENOSYS;

  char cmd[64] = "";
  size_t n = msg->i.nbytes < sizeof cmd - 1 ? msg->i.nbytes : sizeof cmd - 1;
  resmgr_msgread(ctp, cmd, n, sizeof msg->i);
  cmd[strcspn(cmd, "\n")] = '\0';

  printf("valved: accepted '%s'\n", cmd);
  _IO_SET_WRITE_NBYTES(ctp, msg->i.nbytes);
  return _RESMGR_NPARTS(0);
}

int main(void) {
  static resmgr_connect_funcs_t connect_funcs;
  static resmgr_io_funcs_t io_funcs;
  static iofunc_attr_t attr;
  setvbuf(stdout, NULL, _IOLBF, 0);

  dispatch_t *dpp = dispatch_create();
  iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &connect_funcs, _RESMGR_IO_NFUNCS, &io_funcs);
  io_funcs.write = valve_write;
  iofunc_attr_init(&attr, S_IFNAM | 0666, NULL, NULL);
  if (resmgr_attach(dpp, NULL, "/dev/valve", _FTYPE_ANY, 0,
                    &connect_funcs, &io_funcs, &attr) == -1) {
    perror("valved: resmgr_attach");
    return 1;
  }
  printf("valved: serving /dev/valve\n");

  dispatch_context_t *ctp = dispatch_context_alloc(dpp);
  for (;;) {
    if ((ctp = dispatch_block(ctp)) == NULL) return 1;
    dispatch_handler(ctp);
  }
}
