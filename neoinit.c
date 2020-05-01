#include <alloca.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/kd.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/reboot.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "djb/fmt.h"
#include "djb/str.h"

#include "neoinit.h"

typedef struct {
  char *name;
  pid_t pid;
  char respawn;
  char circular;
  int state;
  int sid_father;
  int sid_log;
  time_t changed_at;
  int __stdin, __stdout;
} sv_t;

static sv_t *svlist;
static char *confdata;

static int sv_max = -1;
static int sv_alloc;
static int iam_init;
static int infd, outfd;

#define HISTORY 15
static int history[HISTORY];

static void wout(const char *s) {
  unsigned long len = str_len(s);
  if (write(1, s, len) != len) {
    fprintf(stderr, "neoinit: write out failed!\n");
  }
}
static void werr(const char *s) {
  unsigned long len = str_len(s);
  if (write(2, s, len) != len) {
    fprintf(stderr, "neoinit: write err failed!\n");
  }
}
static void write_checked(int fd, const char *s, unsigned long len) {
  if (write(fd, s, len) != len) {
    werr("neoinit: write failed!\n");
  }
}
#ifdef DEBUG
#define dbg(...)                                                                                   \
  printf(__VA_ARGS__);                                                                             \
  fflush(stdout);
#else
#define dbg(...)
#endif

extern char **environ;

extern int openreadclose(char *fn, char **buf, unsigned long *len);
extern char **split(char *buf, int sep, unsigned long *len, int plus, int ofs);

/* return index of service or -1 if not found */
int findservice(char *service) {
  for (int si = 0; si <= sv_max; ++si) {
    if (!strcmp(svlist[si].name, service)) {
      return si;
    }
  }
  return -1;
}

/* lookup service index by PID */
int findbypid(pid_t pid) {
  for (int si = 0; si <= sv_max; ++si) {
    if (svlist[si].pid == pid) {
      return si;
    }
  }
  return -1;
}

/* clear circular dependency detection flags */
void circsweep() {
  for (int si = 0; si <= sv_max; ++si) {
    svlist[si].circular = 0;
  }
}

/* add service data structure, return index or -1 */
int addsv(sv_t *sv) {
  if (sv_max + 1 >= sv_alloc) {
    sv_t *svlist_ext = 0;
    sv_alloc += 8;
    if ((svlist_ext = (sv_t *)realloc(svlist, sv_alloc * sizeof(sv_t))) == 0) {
      return -1;
    }
    svlist = svlist_ext;
  }
  memmove(&svlist[++sv_max], sv, sizeof(sv_t));
  // dbg("[%d:%s] created\n", sv_max, svlist[sv_max].name);
  return sv_max;
}

int loadservice(char *service);

/* create a service defined in subfolder */
int loadsubservice(sv_t *sv, char *subservice) {
  char *service = alloca(str_len(sv->name) + str_len(subservice) + 2);
  strcpy(service, sv->name);
  strcat(service, "/");
  strcat(service, subservice);
  return loadservice(service);
}

/* load service, return index or -1 if failed */
int loadservice(char *service) {
  sv_t sv;
  if (*service == 0 || str_len(service) > PATH_MAX) {
    return -1;
  }
  int sid = findservice(service);
  if (sid >= 0) {
    return sid;
  }
  if (chdir(NEOROOT) || chdir(service)) {
    return -1;
  }
  if (!(sv.name = strdup(service))) {
    return -1;
  }
  sv.pid = 0;
  sv.circular = 0;
  sv.changed_at = 0;
  sv.state = SID_INIT;
  int fd = open("respawn", O_RDONLY | O_CLOEXEC);
  if (fd >= 0) {
    close(fd);
    sv.respawn = 1;
  } else {
    sv.respawn = 0;
  }
  sv.__stdin = 0;
  sv.__stdout = 1;

  sv.sid_log = loadsubservice(&sv, "log");
  if (sv.sid_log >= 0) {
    int pipefd[2];
    if (pipe(pipefd) ||
        fcntl(pipefd[0], F_SETFD, FD_CLOEXEC) ||
        fcntl(pipefd[1], F_SETFD, FD_CLOEXEC)) {
      free(sv.name);
      return -1;
    }
    svlist[sv.sid_log].__stdin = pipefd[0];
    sv.__stdout = pipefd[1];
  }
  sid = addsv(&sv);
  if (sid < 0) {
    free(sv.name);
  }
  return sid;
}

/* usage: isup(findservice("sshd")), returns nonzero if process was already started */
int isup(int sid) {
  if (sid < 0) {
    return 0;
  }
  return (svlist[sid].state != SID_INIT);
}

/* usage: isrunning(findservice("sshd")), returns nonzero if process is running */
int isrunning(int sid) {
  if (sid < 0) {
    return 0;
  }
  return (svlist[sid].pid > 1);
}

int startservice(int sid, int pause, int sid_father);
int startnodep(int sid, int pause, int setup);

void handlekilled(pid_t killed, int status) {
  if (!killed) {
    return;
  }
  int sid = findbypid(killed);
  dbg("[neoinit] pid %d exited: sid %d %s\n", killed, sid, sid >= 0 ? svlist[sid].name : "");
  if (sid < 0) {
    return;
  }
  if (svlist[sid].state != SID_STOPPED) { // has been stopped
    if (svlist[sid].state == SID_SETUP) { // was setup
      if (WIFEXITED(status) && WEXITSTATUS(status)) {
        dbg("[%d:%s] CANCELED %d\n", sid, svlist[sid].name, WEXITSTATUS(status));
        svlist[sid].state = SID_CANCELED;
      } else {
        dbg("[%d:%s] INIT\n", sid, svlist[sid].name);
        svlist[sid].state = SID_INIT;
      }
    } else { // was active
      if (WIFEXITED(status) && WEXITSTATUS(status)) {
        dbg("[%d:%s] FAILED %d\n", sid, svlist[sid].name, WEXITSTATUS(status));
        svlist[sid].state = SID_FAILED;
      } else {
        dbg("[%d:%s] FINISHED\n", sid, svlist[sid].name);
        svlist[sid].state = SID_FINISHED;
      }
    }
  }
  if (svlist[sid].state == SID_FINISHED && !chdir(NEOROOT) && !chdir(svlist[sid].name)) {
    unsigned long len = 0;
    char *pidfile = 0;
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
          unsigned char c = 0;
          pid_t pid = 0;
          while ((c = *x++ - '0') < 10) {
            pid = pid * 10 + c;
          }
          if (pid > 0 && !kill(pid, 0)) {
            dbg("[%d:%s] pidfile %d\n", sid, svlist[sid].name, pid);
            svlist[sid].pid = pid;
            return;
          }
        }
      }
    }
  }
  time_t sid_started_at = svlist[sid].changed_at;
  svlist[sid].changed_at = time(0); /* set stop time */
  dbg("[%d:%s] pid down\n", sid, svlist[sid].name);
  svlist[sid].pid = PID_DOWN;

  if (svlist[sid].state == SID_INIT) {
    startnodep(sid, 0, 0);
  } else if (svlist[sid].state != SID_STOPPED && svlist[sid].state != SID_CANCELED &&
             svlist[sid].respawn) {
    dbg("[%d:%s] respawn\n", sid, svlist[sid].name);
    dbg("[%d:%s] INIT\n", sid, svlist[sid].name);
    svlist[sid].state = SID_INIT;
    circsweep();
    startservice(sid, time(0) - sid_started_at < 1, svlist[sid].sid_father);
  }
}

/* called from inside the service directory, return the PID or 0 on error */
pid_t forkandexec(int sid, int pause, int setup) {
  int count = 0;
  pid_t pid = 0;
  int sync = 0;
  unsigned long len = 0;
  char *argdata = 0;
  char **argv = 0;
  char *argv0 = 0;
  char *envdata = 0;
  char **env = 0;
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
    if (!setup && !openreadclose("params", &argdata, &len)) {
      len = 0;
      argv = split(argdata, '\n', &len, 2, 1);
      if (argv && len > 0) {
        if (!*argv[len - 1]) {
          argv[len - 1] = 0;
        } else {
          argv[len] = 0;
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
    char *cmd = setup ? "setup" : "run";
    if (readlink(cmd, argv0, PATH_MAX) < 0) {
      if (errno == ENOENT) {
        _exit(0);
      }
      if (errno != EINVAL) {
        _exit(227);
      }
      strcpy(argv0, cmd);
    }
    argv[0] = strrchr(argv0, '/');
    if (argv[0]) {
      argv[0]++;
    } else {
      argv[0] = argv0;
    }
    if (!setup && !openreadclose("environ", &envdata, &len)) {
      len = 0;
      env = split(envdata, '\n', &len, 0, 0);
      if (env) {
        for (int i = 0; i < len; ++i) {
          if (*env[i]) {
            putenv(env[i]);
          }
        }
        free(env);
      }
    }
    char *env_service = (char *)alloca(str_len(svlist[sid].name) + 13);
    if (env_service) {
      strcpy(env_service, "NEO_SERVICE=");
      strcat(env_service, svlist[sid].name);
      putenv(env_service);
    }
    if (svlist[sid].__stdin != 0) {
      if (dup2(svlist[sid].__stdin, 0)) {
        _exit(225);
      }
      if (fcntl(0, F_SETFD, 0)) {
        _exit(225);
      }
    }
    if (svlist[sid].__stdout != 1) {
      if (dup2(svlist[sid].__stdout, 1) || dup2(svlist[sid].__stdout, 2)) {
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
    dbg("[%d:%s] pid %d\n", sid, svlist[sid].name, pid);
    svlist[sid].pid = pid;
    // sync on service 'boot' and depends
    if ((sid == 0 || svlist[sid].sid_father == 0) && !strcmp(svlist[0].name, "boot")) {
      sync = 1;
    } else {
      int fd = open("sync", O_RDONLY | O_CLOEXEC);
      if (fd >= 0) {
        close(fd);
        sync = 1;
      }
    }
    if (sync) {
      int status = 0;
      waitpid(pid, &status, 0);
      svlist[sid].respawn = 0;
      handlekilled(pid, status);
    }
    return 0;
  }
}

/* start a service, return nonzero on error */
int startnodep(int sid, int pause, int setup) {
  if (isup(sid)) {
    return 0;
  }
  if (chdir(NEOROOT) || chdir(svlist[sid].name)) {
    return -1;
  }

  memmove(history + 1, history, sizeof(int) * ((HISTORY)-1));
  history[0] = sid;

  if (setup) {
    dbg("[%d:%s] SETUP\n", sid, svlist[sid].name);
    svlist[sid].state = SID_SETUP;
  } else {
    dbg("[%d:%s] ACTIVE\n", sid, svlist[sid].name);
    svlist[sid].state = SID_ACTIVE;
  }
  svlist[sid].changed_at = time(0); /* set start time */
  return forkandexec(sid, pause, setup);
}

int startservice(int sid, int pause, int sid_father) {
  int dir = -1;
  if (sid < 0) {
    return 0;
  }
  if (svlist[sid].circular) {
    return 0;
  }
  svlist[sid].circular = 1;
  svlist[sid].sid_father = sid_father;
  dbg("[%d:%s] starting\n", sid, svlist[sid].name);
  // dbg("[%d:%s] parent %d %s\n", sid, svlist[sid].name, sid_father,
  //     sid_father >= 0 ? svlist[sid_father].name : "neoinit");
  if (svlist[sid].sid_log >= 0) {
    startservice(svlist[sid].sid_log, pause, sid);
  }
  if (chdir(NEOROOT) || chdir(svlist[sid].name)) {
    return -1;
  }
  if ((dir = open(".", O_RDONLY | O_CLOEXEC)) >= 0) {
    unsigned long len = 0;
    char *depdata = 0;
    if (!openreadclose("depends", &depdata, &len)) {
      len = 0;
      char **depv = split(depdata, '\n', &len, 0, 0);
      if (depv) {
        for (int i = 0; i < len; ++i) {
          if (depv[i][0] == 0 || depv[i][0] == '#') {
            continue;
          }
          dbg("[%d:%s] depends: %s\n", sid, svlist[sid].name, depv[i]);
          int sid_dep = loadservice(depv[i]);
          if (sid_dep >= 0 && !isup(sid_dep)) {
            startservice(sid_dep, 0, sid);
          }
        }
        free(depv);
      }
      free(depdata);
      if (fchdir(dir)) {
        close(dir);
        return -1;
      }
    }
    close(dir);
    int setup = 0;
    int fd = open("setup", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
      close(fd);
      setup = 1;
    }
    return startnodep(sid, pause, setup);
  }
  return -1;
}

void childhandler() {
  pid_t killed = 0;
  int status = 0;
  do {
    killed = waitpid(-1, &status, WNOHANG);
    if (killed != -1) {
      handlekilled(killed, status);
    }
    // TODO check errno
  } while (killed && killed != -1);

  for (int sid = 0; sid <= sv_max; ++sid) {
    if (isrunning(sid)) {
      if (kill(svlist[sid].pid, 0)) {
        handlekilled(svlist[sid].pid, 0);
      } else {
        killed = 0;
      }
    }
  }
  if (killed == -1) {
    if (iam_init) {
      wout("neoinit: all services exited\n");
    }
    if (confdata) {
      free(confdata);
    }
    for (int sid = 0; sid <= sv_max; ++sid) {
      free(svlist[sid].name);
    }
    free(svlist);
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

  infd = open(NEOROOT "/in", O_RDWR | O_CLOEXEC);
  outfd = open(NEOROOT "/out", O_RDWR | O_NONBLOCK | O_CLOEXEC);

  if (infd < 0 || outfd < 0) {
    werr("neoinit: could not open " NEOROOT "/{in,out}\n");
    nfds = 0;
  } else {
    pfd.fd = infd;
  }
  pfd.events = POLLIN;

  if (fcntl(infd, F_SETFD, FD_CLOEXEC) || fcntl(outfd, F_SETFD, FD_CLOEXEC)) {
    werr("neoinit: could not set up " NEOROOT "/{in,out}\n");
    nfds = 0;
  }

  unsigned long len = 0;
  char **conf = 0;
  if (!openreadclose(NEOROOT "/neo.conf", &confdata, &len)) {
    len = 0;
    conf = split(confdata, '\n', &len, 0, 0);
    if (conf) {
      for (int i = 0; i < len; ++i) {
        if (*conf[i]) {
          putenv(conf[i]);
        }
      }
      free(conf);
    }
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
    char buf[BUFSIZE + 1];
    time_t now = 0;
    childhandler();
    now = time(0);
    if (now < last || now - last > 30) {
      /* the system clock was reset, compensate */
      long diff = last - now;
      for (int j = 0; j <= sv_max; ++j) {
        svlist[j].changed_at -= diff;
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
          write_checked(outfd, "0", 1);
        } else {
          switch (buf[0]) {
          case 'p': // get service pid and state
            len = fmt_long(buf, svlist[sid].pid);
            buf[len++] = '@';
            len += fmt_ulong(buf + len, svlist[sid].state);
            buf[len++] = 0;
            write_checked(outfd, buf, len);
            break;
          case 'r': // unset service respawn
            svlist[sid].respawn = 0;
            goto ok;
          case 'R': // set service respawn
            svlist[sid].respawn = 1;
            goto ok;
          case 'c': // cancel service (prepare to stop)
            if (!isrunning(sid)) {
              goto error;
            }
            dbg("[%d:%s] STOPPED\n", sid, svlist[sid].name);
            svlist[sid].state = SID_STOPPED;
            goto ok;
          case 'C': // clear service (reset state)
            if (svlist[sid].pid != PID_DOWN) {
              goto error;
            }
            dbg("[%d:%s] INIT\n", sid, svlist[sid].name);
            svlist[sid].state = SID_INIT;
            svlist[sid].changed_at = time(0);
            goto ok;
          case 'P': { // set service pid
            char *x = buf + str_len(buf) + 1;
            unsigned char c = 0;
            pid_t pid = 0;
            while ((c = *x++ - '0') < 10) {
              pid = pid * 10 + c;
            }
            if (pid > 0) {
              if (kill(pid, 0)) {
                goto error;
              }
            }
            dbg("[%d:%s] set PID\n", sid, svlist[sid].name);
            dbg("[%d:%s] pid %d\n", sid, svlist[sid].name, pid);
            if (svlist[sid].state != SID_ACTIVE) {
              dbg("[%d:%s] ACTIVE\n", sid, svlist[sid].name);
              svlist[sid].state = SID_ACTIVE;
            }
            svlist[sid].changed_at = time(0);
            svlist[sid].pid = pid;
            goto ok;
          }
          case 's': // start service
            sid = loadservice(buf + 1);
            if (sid < 0) {
              goto error;
            }
            if (!isrunning(sid)) {
              dbg("[%d:%s] INIT\n", sid, svlist[sid].name);
              svlist[sid].state = SID_INIT;
              svlist[sid].changed_at = time(0);
              circsweep();
              if (startservice(sid, 0, -1)) {
                goto error;
              }
            }
          ok:
            write_checked(outfd, "1", 1);
            break;
          case 'u': // get service uptime
            write_checked(outfd, buf, fmt_ulong(buf, time(0) - svlist[sid].changed_at));
            break;
          case 'd': // get service dependencies
            len = 0;
            write_checked(outfd, "1:", 2);
            dbg("[neoinit] looking for father = sid %d\n", sid);
            for (int si = 0; si <= sv_max; ++si) {
              if (svlist[si].sid_father == sid) {
                write_checked(outfd, svlist[si].name, str_len(svlist[si].name) + 1);
                len = 1;
              }
            }
            if (!len) {
              write_checked(outfd, "\0\0", 2);
            } else {
              write_checked(outfd, "\0", 1);
            }
            break;
          }
        }
      } else {
        if (buf[0] == 'h') { // get service history
          write_checked(outfd, "1:", 2);
          for (int i = 0; i < HISTORY; ++i) {
            if (history[i] != -1) {
              write_checked(outfd, svlist[history[i]].name, str_len(svlist[history[i]].name) + 1);
            }
          }
          write_checked(outfd, "\0", 1);
        } else if (buf[0] == 'l' || buf[0] == 'L') { // get service list
          write_checked(outfd, "1:", 2);
          for (int si = 0; si <= sv_max; ++si) {
            write_checked(outfd, svlist[si].name, str_len(svlist[si].name));
            if (buf[0] == 'l') {
              write_checked(outfd, "\0", 1);
              continue;
            }
            write_checked(outfd, " ", 1);
            write_checked(outfd, buf, fmt_state(buf, svlist[si].state));
            write_checked(outfd, " ", 1);
            write_checked(outfd, buf, fmt_ulong(buf, time(0) - svlist[si].changed_at));
            write_checked(outfd, "s\0", 2);
          }
          write_checked(outfd, "\0", 1);
        }
      }
      break;
    default:
      break;
    }
  }
}
