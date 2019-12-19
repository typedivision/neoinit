#include <alloca.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/kd.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/reboot.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef DEBUG
#include <stdio.h>
#endif

#include "djb/fmt.h"
#include "djb/str.h"

#include "neoinit.h"

typedef struct {
  char *name;
  pid_t pid;
  char respawn;
  char circular;
  int sid_father;
  int sid_setup;
  int sid_log;
  time_t changed_at;
  int __stdin, __stdout;
} svc_t;

static svc_t *slist;

static int svc_max = -1;
static int svc_alloc;
static int iam_init;
static int infd, outfd;

#define HISTORY 15
static int history[HISTORY];

static void wout(const char *s) { write(1, s, str_len(s)); }
static void werr(const char *s) { write(2, s, str_len(s)); }
#ifdef DEBUG
#define dbg(...)                                                                                   \
  printf(__VA_ARGS__);                                                                             \
  fflush(stdout);
#else
#define dbg(...)
#endif

extern char **environ;

extern int openreadclose(char *fn, char **buf, unsigned long *len);
extern char **split(char *buf, int sep, int *len, int plus, int ofs);

/* return index of service or -1 if not found */
int findservice(char *service) {
  for (int i = 0; i <= svc_max; ++i) {
    if (!strcmp(slist[i].name, service)) {
      return i;
    }
  }
  return -1;
}

/* lookup service index by PID */
int findbypid(pid_t pid) {
  for (int i = 0; i <= svc_max; ++i) {
    if (slist[i].pid == pid) {
      return i;
    }
  }
  return -1;
}

/* clear circular dependency detection flags */
void circsweep() {
  for (int i = 0; i <= svc_max; ++i) {
    slist[i].circular = 0;
  }
}

/* add service data structure, return index or -1 */
int addsvc(svc_t *svc) {
  if (svc_max + 1 >= svc_alloc) {
    svc_t *slist_ext;
    svc_alloc += 8;
    if ((slist_ext = (svc_t *)realloc(slist, svc_alloc * sizeof(svc_t))) == 0) {
      return -1;
    }
    slist = slist_ext;
  }
  memmove(&slist[++svc_max], svc, sizeof(svc_t));
  // dbg("[%d:%s] created\n", svc_max, slist[svc_max].name);
  return svc_max;
}

int loadservice(char *service);

/* create a service defined in subfolder */
int loadsubservice(svc_t *svc, char *subservice) {
  char *service = alloca(str_len(svc->name) + str_len(subservice) + 2);
  strcpy(service, svc->name);
  strcat(service, "/");
  strcat(service, subservice);
  return loadservice(service);
}

/* load service, return index or -1 if failed */
int loadservice(char *service) {
  svc_t svc;
  if (*service == 0 || str_len(service) > PATH_MAX) {
    return -1;
  }
  int sid = findservice(service);
  if (sid >= 0) {
    return sid;
  }
  if (chdir(NIROOT) || chdir(service)) {
    return -1;
  }
  if (!(svc.name = strdup(service))) {
    return -1;
  }
  svc.pid = 0;
  svc.circular = 0;
  svc.changed_at = 0;
  int fd = open("respawn", O_RDONLY | O_CLOEXEC);
  if (fd >= 0) {
    close(fd);
    svc.respawn = 1;
  } else {
    svc.respawn = 0;
  }
  svc.__stdin = 0;
  svc.__stdout = 1;

  svc.sid_log = loadsubservice(&svc, "log");
  svc.sid_setup = loadsubservice(&svc, "setup");

  if (svc.sid_log >= 0) {
    int pipefd[2];
    if (pipe(pipefd) ||
        fcntl(pipefd[0], F_SETFD, FD_CLOEXEC) ||
        fcntl(pipefd[1], F_SETFD, FD_CLOEXEC)) {
      free(svc.name);
      return -1;
    }
    slist[svc.sid_log].__stdin = pipefd[0];
    svc.__stdout = pipefd[1];
  }
  sid = addsvc(&svc);
  if (sid < 0) {
    free(svc.name);
  }
  return sid;
}

/* usage: isup(findservice("sshd")), returns nonzero if process is running, finished or failed */
int isup(int sid) {
  if (sid < 0) {
    return 0;
  }
  return (slist[sid].pid && slist[sid].pid != PID_DOWN && slist[sid].pid != PID_SETUP);
}

/* usage: isrunning(findservice("sshd")), returns nonzero if process is running */
int isrunning(int sid) {
  if (sid < 0) {
    return 0;
  }
  return (slist[sid].pid > 1);
}

int startservice(int sid, int pause, int sid_father);

void handlekilled(pid_t killed, const int *status) {
  if (!killed) {
    return;
  }
  int sid = findbypid(killed);
  dbg("[neoinit] pid %d exited: sid %d %s\n", killed, sid, sid >= 0 ? slist[sid].name : "");
  if (sid < 0) {
    return;
  }
  unsigned long len = 0;
  char *pidfile = 0;
  if (!chdir(NIROOT) && !chdir(slist[sid].name)) {
    if (!openreadclose("pidfile", &pidfile, &len)) {
      for (char *s = pidfile; *s; s++) {
        if (*s == '\n') {
          *s = 0;
          break;
        }
      }
      int fd = open(pidfile, O_RDONLY | O_CLOEXEC);
      free(pidfile);
      if (fd >= 0) {
        char pidbuf[8];
        len = read(fd, pidbuf, sizeof(pidbuf) - 1);
        close(fd);
        if (len > 0) {
          pidbuf[len] = 0;
          char *x = pidbuf;
          unsigned char c;
          pid_t pid = 0;
          while ((c = *x++ - '0') < 10) {
            pid = pid * 10 + c;
          }
          if (pid > 0 && !kill(pid, 0)) {
            dbg("[%d:%s] pidfile %d\n", sid, slist[sid].name, pid);
            slist[sid].pid = pid;
            return;
          }
        }
      }
    }
  }
  if (status && WIFEXITED(*status) && WEXITSTATUS(*status)) {
    dbg("[%d:%s] FAILED %d\n", sid, slist[sid].name, WEXITSTATUS(*status));
    slist[sid].pid = PID_FAILED;
  } else {
    dbg("[%d:%s] FINISHED\n", sid, slist[sid].name);
    slist[sid].pid = PID_FINISHED;
  }
  time_t sid_started_at = slist[sid].changed_at;
  slist[sid].changed_at = time(0); /* set stop time */

  int sid_father = slist[sid].sid_father;
  if (sid_father >= 0 && slist[sid_father].sid_setup == sid) {
    if (slist[sid].pid == PID_FAILED) {
      /* dont start service if setup failed */
      dbg("[%d:%s] SETUP_FAILED\n", sid_father, slist[sid_father].name);
      slist[sid_father].pid = PID_SETUP_FAILED;
      slist[sid_father].changed_at = slist[sid].changed_at;
    } else {
      circsweep();
      startservice(sid_father, 0, slist[sid_father].sid_father);
    }
  } else {
    if (slist[sid].respawn) {
      dbg("[%d:%s] restarting\n", sid, slist[sid].name);
      dbg("[%d:%s] DOWN\n", sid, slist[sid].name);
      slist[sid].pid = PID_DOWN;
      circsweep();
      startservice(sid, time(0) - sid_started_at < 1, slist[sid].sid_father);
    }
  }
}

/* called from inside the service directory, return the PID or 0 on error */
pid_t forkandexec(int sid, int pause) {
  int count = 0;
  pid_t pid;
  int fd;
  unsigned long len = 0;
  char *params = 0;
  char **argv = 0;
  int argc = 0;
  char *argv0 = 0;
again:
  switch (pid = fork()) {
  case -1:
    if (count > 3) {
      return -1;
    }
    sleep(++count * 2);
    goto again;
  case 0:
    /* child */
    if (iam_init) {
      ioctl(0, TIOCNOTTY, 0);
      setsid();
      pid_t pgrp = getpgrp();
      if (pgrp > 0) {
        tcsetpgrp(0, pgrp);
      }
    }
    if (pause) {
      struct timespec req;
      req.tv_sec = 0;
      req.tv_nsec = 500000000;
      nanosleep(&req, 0);
    }
    if (!openreadclose("params", &params, &len)) {
      argv = split(params, '\n', &argc, 2, 1);
      if (argv) {
        if (argv[argc - 1]) {
          argv[argc - 1] = 0;
        } else {
          argv[argc] = 0;
        }
      }
    }
    if (!argv) {
      argv = (char **)alloca(2 * sizeof(char *));
      if (argv) {
        argv[1] = 0;
      }
    }
    argv0 = (char *)alloca(PATH_MAX + 1);
    if (!argv || !argv0) {
      _exit(225);
    }
    memset(argv0, 0, PATH_MAX + 1);
    if (readlink("run", argv0, PATH_MAX) < 0) {
      if (errno == ENOENT) {
        _exit(0);
      }
      if (errno != EINVAL) {
        _exit(227);
      }
      argv0 = strdup("./run");
    }
    argv[0] = strrchr(argv0, '/');
    if (argv[0]) {
      argv[0]++;
    } else {
      argv[0] = argv0;
    }
    if (slist[sid].__stdin != 0) {
      if (dup2(slist[sid].__stdin, 0)) {
        _exit(225);
      }
      if (fcntl(0, F_SETFD, 0)) {
        _exit(225);
      }
    }
    if (slist[sid].__stdout != 1) {
      if (dup2(slist[sid].__stdout, 1) || dup2(slist[sid].__stdout, 2)) {
        _exit(225);
      }
      if (fcntl(1, F_SETFD, 0) || fcntl(2, F_SETFD, 0)) {
        _exit(225);
      }
    }
    for (int i = 3; i < 1024; ++i) {
      close(i);
    }
    execve(argv0, argv, environ);
    _exit(226);
  default:
    dbg("[%d:%s] pid %d\n", sid, slist[sid].name, pid);
    slist[sid].pid = pid;
    fd = open("sync", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
      close(fd);
      int status = 0;
      waitpid(pid, &status, 0);
      slist[sid].respawn = 0;
      handlekilled(pid, &status);
    }
    return 0;
  }
}

/* start a service, return nonzero on error */
int startnodep(int sid, int pause) {
  if (isup(sid)) {
    return 0;
  }
  if (chdir(NIROOT) || chdir(slist[sid].name)) {
    return -1;
  }

  memmove(history + 1, history, sizeof(int) * ((HISTORY)-1));
  history[0] = sid;

  dbg("[%d:%s] RUNNING\n", sid, slist[sid].name);
  slist[sid].changed_at = time(0); /* set start time */
  return forkandexec(sid, pause);
}

int startservice(int sid, int pause, int sid_father) {
  int dir = -1;
  if (sid < 0) {
    return 0;
  }
  if (slist[sid].circular) {
    return 0;
  }
  slist[sid].circular = 1;
  slist[sid].sid_father = sid_father;
  dbg("[%d:%s] starting\n", sid, slist[sid].name);
  // dbg("[%d:%s] parent %d %s\n", sid, slist[sid].name, sid_father,
  //     sid_father >= 0 ? slist[sid_father].name : "neoinit");

  if (slist[sid].sid_log >= 0) {
    startservice(slist[sid].sid_log, pause, sid);
  }
  if (chdir(NIROOT) || chdir(slist[sid].name)) {
    return -1;
  }
  if ((dir = open(".", O_RDONLY | O_CLOEXEC)) >= 0) {
    unsigned long len = 0;
    char *depends = 0;
    if (!openreadclose("depends", &depends, &len)) {
      int depc = 0;
      char **depv = split(depends, '\n', &depc, 0, 0);
      if (depv) {
        for (int di = 0; di < depc; di++) {
          if (depv[di][0] == 0 || depv[di][0] == '#') {
            continue;
          }
          dbg("[%d:%s] depends: %s\n", sid, slist[sid].name, depv[di]);
          int sid_dep = loadservice(depv[di]);
          if (sid_dep >= 0 && !isup(sid_dep)) {
            startservice(sid_dep, 0, sid);
          }
        }
        free(depv);
      }
      free(depends);
      fchdir(dir);
    }
    int started = -1;
    if (slist[sid].sid_setup >= 0 && !isup(slist[sid].sid_setup)) {
      dbg("[%d:%s] SETUP\n", sid, slist[sid].name);
      slist[sid].pid = PID_SETUP;
      slist[sid].changed_at = time(0);
      started = startservice(slist[sid].sid_setup, pause, sid);
    } else {
      started = startnodep(sid, pause);
    }
    close(dir);
    return started;
  }
  return -1;
}

void childhandler() {
  pid_t killed;
  int status = 0;
  do {
    killed = waitpid(-1, &status, WNOHANG);
    if (killed != -1) {
      handlekilled(killed, &status);
    }
    // TODO check errno
  } while (killed && killed != -1);

  status = 0;
  for (int sid = 0; sid <= svc_max; ++sid) {
    if (isrunning(sid)) {
      if (kill(slist[sid].pid, 0)) {
        handlekilled(slist[sid].pid, &status);
      } else {
        killed = 0;
      }
    }
  }
  if (killed == -1) {
    if (iam_init) {
      wout("neoinit: all services exited\n");
    }
    for (int sid = 0; sid <= svc_max; ++sid) {
      free(slist[sid].name);
    }
    free(slist);
    exit(0);
  }
}

int main(int argc, char *argv[]) {
  struct pollfd pfd;
  time_t last = time(0);
  int nfds = 1;

  for (int i = 0; i < HISTORY; ++i) {
    history[i] = -1;
  }

  if (getpid() == 1) {
    iam_init = 1;
    reboot(0);
  }

  circsweep();
  startservice(loadservice("boot"), 0, -1);

  infd = open(NIROOT "/in", O_RDWR | O_CLOEXEC);
  outfd = open(NIROOT "/out", O_RDWR | O_NONBLOCK | O_CLOEXEC);

  if (infd < 0 || outfd < 0) {
    werr("neoinit: could not open " NIROOT "/{in,out}\n");
    nfds = 0;
  } else {
    pfd.fd = infd;
  }
  pfd.events = POLLIN;

  if (fcntl(infd, F_SETFD, FD_CLOEXEC) || fcntl(outfd, F_SETFD, FD_CLOEXEC)) {
    werr("neoinit: could not set up " NIROOT "/{in,out}\n");
    nfds = 0;
  }

  int count = 0;
  for (int i = 1; i < argc; i++) {
    circsweep();
    if (startservice(loadservice(argv[i]), 0, -1)) {
      count++;
    }
  }
  circsweep();
  if (!count) {
    startservice(loadservice("default"), 0, -1);
  }

  for (;;) {
    int len;
    char buf[BUFSIZE + 1];
    time_t now;
    childhandler();
    now = time(0);
    if (now < last || now - last > 30) {
      /* the system clock was reset, compensate */
      long diff = last - now;
      for (int j = 0; j <= svc_max; ++j) {
        slist[j].changed_at -= diff;
      }
    }
    last = now;
    switch (poll(&pfd, nfds, 5000)) {
    case -1:
      if (errno == EINTR) {
        childhandler();
        break;
      }
      werr("neoinit: poll failed!\n");
      break;
    case 1:
      len = read(infd, buf, BUFSIZE);
      if (len > 1) {
        int sid = -1;
        buf[len] = 0;
        if (buf[0] != 's' && ((sid = findservice(buf + 1)) < 0) && strcmp(buf, "d-") != 0) {
        error:
          write(outfd, "0", 1);
        } else {
          switch (buf[0]) {
          case 'p':
            write(outfd, buf, fmt_long(buf, slist[sid].pid));
            break;
          case 'r':
            slist[sid].respawn = 0;
            goto ok;
          case 'R':
            slist[sid].respawn = 1;
            goto ok;
          case 'C':
            if (isrunning(sid) || slist[sid].pid == PID_SETUP) {
              goto error;
            }
            dbg("[%d:%s] DOWN\n", sid, slist[sid].name);
            slist[sid].pid = PID_DOWN;
            slist[sid].changed_at = time(0);
            int sid_setup = slist[sid].sid_setup;
            if (sid_setup >= 0) {
              dbg("[%d:%s] DOWN\n", sid_setup, slist[sid_setup].name);
              slist[sid_setup].pid = PID_DOWN;
              slist[sid_setup].changed_at = slist[sid].changed_at;
            }
            goto ok;
          case 'P': {
            char *x = buf + str_len(buf) + 1;
            unsigned char c;
            pid_t pid = 0;
            while ((c = *x++ - '0') < 10) {
              pid = pid * 10 + c;
            }
            if (pid > 0) {
              if (kill(pid, 0)) {
                goto error;
              }
            }
            slist[sid].pid = pid;
            goto ok;
          }
          case 's':
            sid = loadservice(buf + 1);
            if (sid < 0) {
              goto error;
            }
            if (!isrunning(sid) && !isrunning(slist[sid].sid_setup)) {
              /* start service including setup */
              dbg("[%d:%s] DOWN\n", sid, slist[sid].name);
              slist[sid].pid = PID_DOWN;
              slist[sid].changed_at = time(0);
              int sid_setup = slist[sid].sid_setup;
              if (sid_setup >= 0) {
                dbg("[%d:%s] DOWN\n", sid_setup, slist[sid_setup].name);
                slist[sid_setup].pid = PID_DOWN;
                slist[sid_setup].changed_at = slist[sid].changed_at;
              }
              circsweep();
              if (startservice(sid, 0, -1)) {
                goto error;
              }
            }
          ok:
            write(outfd, "1", 1);
            break;
          case 'u':
            write(outfd, buf, fmt_ulong(buf, time(0) - slist[sid].changed_at));
            break;
          case 'd':
            len = 0;
            write(outfd, "1:", 2);
            dbg("[neoinit] looking for father = sid %d\n", sid);
            for (int si = 0; si <= svc_max; ++si) {
              if (slist[si].sid_father == sid) {
                write(outfd, slist[si].name, str_len(slist[si].name) + 1);
                len = 1;
              }
            }
            if (!len) {
              write(outfd, "\0\0", 2);
            } else {
              write(outfd, "\0", 1);
            }
            break;
          }
        }
      } else {
        if (buf[0] == 'h') {
          write(outfd, "1:", 2);
          for (int i = 0; i < HISTORY; ++i) {
            if (history[i] != -1) {
              write(outfd, slist[history[i]].name, str_len(slist[history[i]].name) + 1);
            }
          }
          write(outfd, "\0", 1);
        }
      }
      break;
    default:
      break;
    }
  }
}
