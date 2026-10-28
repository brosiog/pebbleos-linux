/* SPDX-License-Identifier: Apache-2.0 */

#include <stdbool.h>

#include <sys/socket.h>

static int prv_competing_bind(int fd, const struct sockaddr *address, socklen_t size);

#define bind prv_competing_bind
#include <pbl/drivers/linux_adapter.h>
#undef bind

static int s_competitor = -1;

/* Another process wins the endpoint after the stale-socket check. */
static int prv_competing_bind(int fd, const struct sockaddr *address, socklen_t size) {
  s_competitor = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (s_competitor < 0 || bind(s_competitor, address, size))
    abort();
  return bind(fd, address, size);
}

int main(void) {
  char dir[] = "/tmp/pbl-bind-race.XXXXXX";
  if (!mkdtemp(dir))
    return 1;
  int rc = pbl_adapter_bind(dir, PBL_ADAPTER_OS_SOCK);
  struct sockaddr_un address;
  pbl_adapter_socket_address(&address, dir, PBL_ADAPTER_OS_SOCK);
  int sender = socket(AF_UNIX, SOCK_DGRAM, 0);
  char received = 0;
  bool ok = rc == -EADDRINUSE &&
            sendto(sender, "x", 1, 0, (const struct sockaddr *)&address, sizeof(address)) == 1 &&
            recv(s_competitor, &received, 1, MSG_DONTWAIT) == 1 && received == 'x';
  if (rc >= 0)
    close(rc);
  close(sender);
  close(s_competitor);
  unlink(address.sun_path);
  rmdir(dir);
  printf("%s  failed bind preserves the competing live endpoint\n", ok ? "PASS" : "FAIL");
  return !ok;
}
