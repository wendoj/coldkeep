/* SPDX-License-Identifier: Apache-2.0
 *
 * QNX feature check 1: custom ability + client ability check in a resource manager.
 *
 * Creates the custom ability "coldkeep.check.write", attaches /dev/ckcheck and
 * accepts a write only if the writing client holds that ability. Every write
 * prints what the server saw, so the output answers:
 *   - does procmgr_ability_create() work, and what does it return?
 *   - does iofunc_client_info_able() compile with this signature, and how does
 *     it report whether the client holds the ability?
 *
 * Run as root:  ./able_server &
 *
 * If this file does not compile, the compiler errors are the answer we need:
 * paste them back. Signatures marked VERIFY are from older QNX docs.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/dispatch.h>
#include <sys/iofunc.h>
#include <sys/procmgr.h>
#include <sys/stat.h>

#define ABILITY_NAME "coldkeep.check.write"
#define PATH "/dev/ckcheck"

static resmgr_connect_funcs_t connect_funcs;
static resmgr_io_funcs_t io_funcs;
static iofunc_attr_t attr;
static int able_id = -1;

/* Returns 1 if the client holds the ability, 0 if not, -errno on failure. */
static int client_has_ability(resmgr_context_t *ctp) {
  struct _client_info *info = NULL;
  struct _client_able able;
  memset(&able, 0, sizeof able);
  able.ability = (unsigned)able_id;
  /* VERIFY: iofunc_client_info_able(ctp, ioflag, &info, flags, abilities[], nable) */
  int rc = iofunc_client_info_able(ctp, 0, &info, 0, &able, 1);
  if (rc != EOK) {
    printf("[server] iofunc_client_info_able failed: rc=%d (%s)\n", rc, strerror(rc));
    return -rc;
  }
  /* VERIFY: we assume able.flags is non-zero when the client holds the ability */
  printf("[server] write from pid=%d euid=%d ruid=%d -> able.flags=0x%x\n",
         (int)ctp->info.pid, (int)info->cred.euid, (int)info->cred.ruid, (unsigned)able.flags);
  int has = able.flags != 0;
  iofunc_client_info_ext_free(&info);
  return has;
}

static int io_write(resmgr_context_t *ctp, io_write_t *msg, RESMGR_OCB_T *ocb) {
  int status = iofunc_write_verify(ctp, msg, ocb, NULL);
  if (status != EOK) return status;
  if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) return ENOSYS;
  int has = client_has_ability(ctp);
  if (has < 0) return -has;
  if (has == 0) {
    printf("[server] -> refusing with EPERM\n");
    fflush(stdout);
    return EPERM;
  }
  printf("[server] -> accepted\n");
  fflush(stdout);
  _IO_SET_WRITE_NBYTES(ctp, msg->i.nbytes);
  return _RESMGR_NPARTS(0);
}

int main(void) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  printf("[server] pid=%d uid=%d\n", (int)getpid(), (int)getuid());

  /* VERIFY: procmgr_ability_create(name, flags); flag values for the default domain */
  errno = 0;
  able_id = procmgr_ability_create(ABILITY_NAME, 0);
  printf("[server] procmgr_ability_create(\"%s\", 0) = %d (errno=%d %s)\n",
         ABILITY_NAME, able_id, errno, strerror(errno));
  if (able_id < 0) {
    errno = 0;
    able_id = procmgr_ability_lookup(ABILITY_NAME);
    printf("[server] procmgr_ability_lookup(\"%s\") = %d (errno=%d %s)\n",
           ABILITY_NAME, able_id, errno, strerror(errno));
    if (able_id < 0) return 1;
  }

  dispatch_t *dpp = dispatch_create();
  if (!dpp) { perror("[server] dispatch_create"); return 1; }
  iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &connect_funcs, _RESMGR_IO_NFUNCS, &io_funcs);
  io_funcs.write = io_write;
  iofunc_attr_init(&attr, S_IFNAM | 0666, NULL, NULL);

  resmgr_attr_t rattr;
  memset(&rattr, 0, sizeof rattr);
  rattr.nparts_max = 1;
  rattr.msg_max_size = 2048;
  if (resmgr_attach(dpp, &rattr, PATH, _FTYPE_ANY, 0, &connect_funcs, &io_funcs, &attr) == -1) {
    perror("[server] resmgr_attach");
    return 1;
  }
  printf("[server] attached %s; waiting for writes\n", PATH);

  dispatch_context_t *ctp = dispatch_context_alloc(dpp);
  for (;;) {
    ctp = dispatch_block(ctp);
    if (!ctp) { perror("[server] dispatch_block"); return 1; }
    dispatch_handler(ctp);
  }
}
