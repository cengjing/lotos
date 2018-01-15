#include "lotos_epoll.h"
#include "misc.h"
#include "server.h"
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static void usage(const char *executable) {
  printf("Usage: %s -r html_root_dir [-p port] "
         "[-t timeout] [-w worker_num] [-d (debug mode)]\n",
         executable);
}

int main(int argc, char *argv[]) {
  if (argc < 2 || config_parse(argc, argv) != OK) {
    usage(argv[0]);
    exit(ERROR);
  }

  if (server_config.debug) {
    goto work;
  }

  int nworker = 0;
  while (TRUE) {
    if (nworker >= server_config.worker) {
      int status;
      waitpid(-1, &status, 0); // wait all children
      if (WIFEXITED(status))
        raise(SIGINT);
      lotos_log(LOG_ERR, "a worker exit, please restart...");
      raise(SIGINT);
    }
    pid_t pid = fork();
    ABORT_ON(pid == -1, "fork");
    if (pid == 0) { // child
      break;        // child ends up in loop and directly goto `work`
    }
    nworker++;
  }

work:;
  int nfds;
  int i;

  server_setup(server_config.port);

  while (TRUE) {
    /**
     * nfds is number of file descriptors ready for the requested I/O or zero
     * if timeout
     */
    nfds = lotos_epoll_wait(epoll_fd, lotos_events, MAX_EVENTS, 40);
    if (nfds == ERROR) {
      // if not caused by signal, cannot recover
      ERR_ON(errno != EINTR, "lotos_epoll_wait");
    }

    for (i = 0; i < nfds; i++) {
      int fd = *((int *)(lotos_events[i].data.ptr));
      if (fd == listen_fd) {
        // accept connection
        server_accept(listen_fd);
      } else {
        // handle connection
        connection_t *c = lotos_events[i].data.ptr;
        int status;
        assert(c != NULL);
        /**TODO:
         * if use slow_client, every time will recv only 1 byte.When to decide
         * the connection has recv enough data?
         */
        if (!connecion_is_expired(c) && CONN_IS_IN(c)) {
          // recv
          status = request_handle(c);
          if (status == ERROR)
            connecion_set_expired(c);
          else
            connecion_set_reactivated(c);
        }
        if (!connecion_is_expired(c) && CONN_IS_OUT(c)) {
          // send
          char response[] =
              "HTTP/1.0 200 OK" CRLF "Connection:close" CRLF CRLF
              "<p>hello, this is lotos web server<p>"
              "<p>far from complete, wish I can manage it carefully</p>" CRLF;

          int len = send(c->fd, response, sizeof(response) - 1, 0);
          printf("send %d bytes\n", len);
          connection_disable_out(epoll_fd, c);
          connecion_set_expired(c);
        }
      } // else
    }   // for loop
    /* prune expired connections */
    connection_prune();
  } // while

  close(epoll_fd);
  server_shutdown();
  return OK;
}
