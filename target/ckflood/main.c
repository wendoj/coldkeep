/* SPDX-License-Identifier: Apache-2.0
 *
 * ckflood — A2 load generator: floods mgmtd's documented API with status
 * requests over loopback, from inside the target, so the request rate is not
 * limited by QEMU's user-mode network. Run it unprivileged, e.g.
 *
 *   on -u 1105 ckflood [-p 8080] [-c 4] [-s 15]
 *
 * -c connections (each pipelines up to 256 requests), -s seconds. Prints the
 * number of requests sent and replies received.
 */
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAXC 16
#define WINDOW 256

static double now_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int main(int argc, char **argv) {
  int port = 8080, nconn = 4, opt;
  double seconds = 15;
  while ((opt = getopt(argc, argv, "p:c:s:")) != -1) {
    if (opt == 'p') port = atoi(optarg);
    else if (opt == 'c') nconn = atoi(optarg);
    else if (opt == 's') seconds = atof(optarg);
    else { fprintf(stderr, "usage: ckflood [-p port] [-c conns] [-s seconds]\n"); return 2; }
  }
  if (nconn < 1 || nconn > MAXC) nconn = 4;
  int fd[MAXC];
  long sent[MAXC] = {0}, got[MAXC] = {0};
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof sa);
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)port);
  sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  static const char login[] = "{\"op\":\"login\",\"user\":\"admin\",\"pass\":\"admin\"}\n";
  static const char req[] = "{\"op\":\"status\"}\n";
  char batch[32 * (sizeof req - 1)];
  for (int k = 0; k < 32; k++) memcpy(batch + k * (sizeof req - 1), req, sizeof req - 1);

  for (int i = 0; i < nconn; i++) {
    fd[i] = socket(AF_INET, SOCK_STREAM, 0);
    if (fd[i] < 0 || connect(fd[i], (struct sockaddr *)&sa, sizeof sa) != 0) {
      fprintf(stderr, "ckflood: connect: %s\n", strerror(errno));
      return 1;
    }
    if (write(fd[i], login, sizeof login - 1) < 0) return 1;
    got[i] = -1; /* the login reply is not counted */
  }
  const double t0 = now_s(), stop = t0 + seconds;
  static char buf[65536];
  while (now_s() < stop) {
    struct pollfd pf[MAXC];
    for (int i = 0; i < nconn; i++) {
      pf[i].fd = fd[i];
      pf[i].events = POLLIN | (sent[i] - got[i] < WINDOW ? POLLOUT : 0);
    }
    if (poll(pf, (nfds_t)nconn, 100) <= 0) continue;
    for (int i = 0; i < nconn; i++) {
      if (pf[i].revents & POLLIN) {
        const ssize_t n = read(fd[i], buf, sizeof buf);
        if (n <= 0) { fprintf(stderr, "ckflood: connection %d closed\n", i); return 1; }
        for (ssize_t k = 0; k < n; k++) got[i] += buf[k] == '\n';
      }
      if ((pf[i].revents & POLLOUT) && sent[i] - got[i] < WINDOW) {
        const ssize_t n = write(fd[i], batch, sizeof batch);
        if (n > 0) sent[i] += n / (ssize_t)(sizeof req - 1);
      }
    }
  }
  const double el = now_s() - t0;
  long s = 0, g = 0;
  for (int i = 0; i < nconn; i++) { s += sent[i]; g += got[i] > 0 ? got[i] : 0; close(fd[i]); }
  printf("ckflood: %d connections, %.1f s: %ld requests sent, %ld replies (%.0f replies/s)\n", nconn, el, s, g,
         (double)g / el);
  return 0;
}
