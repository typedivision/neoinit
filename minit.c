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
//#include <stdio.h>
//#include <mcheck.h>

#include "djb/fmt.h"
#include "djb/str.h"

#include "minit.h"

typedef struct {
  char *name;
  pid_t pid;
  char respawn;
  char circular;
  time_t started_at;
  int sid_father;
  int __stdin, __stdout;
  int sid_logservice;
} process_t;

static process_t *root;

static int infd, outfd;
static int maxprocess = -1;
static int processalloc;
static int iam_init;

extern int openreadclose(char *fn, char **buf, unsigned long *len);
extern char **split(char *buf, int sep, int *len, int plus, int ofs);

extern char **environ;

#define HISTORY 10
int history[HISTORY];

/* open rescue shell */
void sulogin() {
  char *argv[] = {"sulogin", 0};
  execve("/sbin/sulogin", argv, environ);
  _exit(1);
}

/* return index of service in process data structure or -1 if not found */
int findservice(char *service) {
  for (int i = 0; i <= maxprocess; ++i) {
    if (!strcmp(root[i].name, service)) {
      return i;
    }
  }
  return -1;
}

/* look up process index in data structure by PID */
int findbypid(pid_t pid) {
  for (int i = 0; i <= maxprocess; ++i) {
    if (root[i].pid == pid) {
      return i;
    }
  }
  return -1;
}

/* clear circular dependency detection flags */
void circsweep() {
  for (int i = 0; i <= maxprocess; ++i) {
    root[i].circular = 0;
  }
}

/* add process to data structure, return index or -1 */
int addprocess(process_t *proc) {
  if (maxprocess + 1 >= processalloc) {
    process_t *rootext;
    processalloc += 8;
    if ((rootext = (process_t *)realloc(root, processalloc * sizeof(process_t))) == 0) {
      return -1;
    }
    root = rootext;
  }
  memmove(&root[++maxprocess], proc, sizeof(process_t));
  return maxprocess;
}

/* check for valid service name */
/* coverity[ +tainted_string_sanitize_content : arg-0 ] */
int check_service(char *service) {
  if (str_len(service) > PATH_MAX) {
    return -1;
  }
  return 0;
}

/* load a service into the process data structure and return index or -1 if failed */
int loadservice(char *service) {
  process_t proc;
  if (*service == 0 || check_service(service)) {
    return -1;
  }
  int sid = findservice(service);
  if (sid >= 0) {
    return sid;
  }
  if (chdir(MINITROOT) || chdir(service)) {
    return -1;
  }
  if (!(proc.name = strdup(service))) {
    return -1;
  }
  proc.pid = 0;
  int fd = open("respawn", O_RDONLY | O_CLOEXEC);
  if (fd >= 0) {
    proc.respawn = 1;
    close(fd);
  } else {
    proc.respawn = 0;
  }
  proc.started_at = 0;
  proc.circular = 0;
  proc.__stdin = 0;
  proc.__stdout = 1;

  char *logservice = alloca(str_len(proc.name) + 5);
  strcpy(logservice, proc.name);
  strcat(logservice, "/log");
  proc.sid_logservice = loadservice(logservice);
  if (proc.sid_logservice >= 0) {
    int pipefd[2];
    if (pipe(pipefd) ||
        fcntl(pipefd[0], F_SETFD, FD_CLOEXEC) ||
        fcntl(pipefd[1], F_SETFD, FD_CLOEXEC)) {
      free(proc.name);
      return -1;
    }
    root[proc.sid_logservice].__stdin = pipefd[0];
    proc.__stdout = pipefd[1];
  }
  sid = addprocess(&proc);
  if (sid < 0) {
    free(proc.name);
  }
  return sid;
}

/* usage: isup(findservice("sshd")), returns nonzero if process is up */
int isup(int sid) {
  if (sid < 0) {
    return 0;
  }
  return (root[sid].pid != 0);
}

int startservice(int sid, int pause, int sid_father);

void handlekilled(pid_t killed) {
  int sid;
  if (killed == (pid_t)-1) {
    static int saidso;
    if (!saidso) {
      write(2, "all services exited\n", 21);
      saidso = 1;
    }
    if (iam_init) {
      for (int si = 0; si <= maxprocess; ++si) {
        free(root[si].name);
      }
      free(root);
      // muntrace();
      // sulogin();
      exit(0);
    }
  }
  if (killed == 0) {
    return;
  }
  sid = findbypid(killed);
  // printf("pid %u exited, sid %d (%s)\n", killed, sid, sid >= 0 ? root[sid].name : "[unknown]");
  if (sid < 0) {
    return;
  }
  unsigned long len = 0;
  char *pidfile = 0;
  if (!chdir(MINITROOT) && !chdir(root[sid].name)) {
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
          int pid = 0;
          while ((c = *x++ - '0') < 10) {
            pid = pid * 10 + c;
          }
          if (pid > 0 && !kill(pid, 0)) {
            // printf("replace sid %d (%s) with pid %u\n", sid, root[sid].name, pid);
            root[sid].pid = pid;
            return;
          }
        }
      }
    }
  }
  root[sid].pid = 0;
  if (root[sid].respawn) {
    // printf("restarting %s\n", root[sid].name);
    circsweep();
    startservice(sid, time(0) - root[sid].started_at < 1, root[sid].sid_father);
  } else {
    root[sid].started_at = time(0);
    root[sid].pid = 1;
  }
}

/* called from inside the service directory, return the PID or 0 on error */
pid_t forkandexec(int sid, int pause) {
  int count = 0;
  pid_t p;
  int fd;
  unsigned long len = 0;
  char *params = 0;
  char **argv = 0;
  int argc = 0;
  char *argv0 = 0;
again:
  switch (p = fork()) {
  case (pid_t)-1:
    if (count > 3) {
      return 0;
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
      _exit(1);
    }
    if (readlink("run", argv0, PATH_MAX) < 0) {
      if (errno != EINVAL) {
        _exit(1); /* not a symbolic link */
      }
      argv0 = strdup("./run");
    }
    argv0[PATH_MAX] = 0;
    argv[0] = strrchr(argv0, '/');
    if (argv[0]) {
      argv[0]++;
    } else {
      argv[0] = argv0;
    }
    if (root[sid].__stdin != 0) {
      if (dup2(root[sid].__stdin, 0)) {
        _exit(1);
      }
      if (fcntl(0, F_SETFD, 0)) {
        _exit(1);
      }
    }
    if (root[sid].__stdout != 1) {
      if (dup2(root[sid].__stdout, 1) || dup2(root[sid].__stdout, 2)) {
        _exit(1);
      }
      if (fcntl(1, F_SETFD, 0) || fcntl(2, F_SETFD, 0)) {
        _exit(1);
      }
    }
    for (int i = 3; i < 1024; ++i) {
      close(i);
    }
    execve(argv0, argv, environ);
    _exit(1);
  default:
    fd = open("sync", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
      close(fd);
      waitpid(p, 0, 0);
      root[sid].pid = p;
      handlekilled(p);
      return root[sid].pid;
    }
    return p;
  }
}

/* start a service, return nonzero on error */
int startnodep(int sid, int pause) {
  /* step 1: see if the process is already up */
  if (isup(sid)) {
    return 0;
  }
  /* step 2: fork and exec service, put PID in data structure */
  if (chdir(MINITROOT) || chdir(root[sid].name)) {
    return -1;
  }
  root[sid].started_at = time(0);
  root[sid].pid = forkandexec(sid, pause);
  return root[sid].pid;
}

int startservice(int sid, int pause, int sid_father) {
  int dir;
  pid_t pid = 0;
  if (sid < 0) {
    return 0;
  }
  if (root[sid].circular) {
    return 0;
  }
  root[sid].circular = 1;
  // printf("setting father of sid %d (%s) to sid %d (%s)\n", sid, root[sid].name, sid_father,
  //        sid_father >= 0 ? root[sid_father].name : "minit");
  root[sid].sid_father = sid_father;

  memmove(history + 1, history, sizeof(int) * ((HISTORY)-1));
  history[0] = sid;

  if (root[sid].sid_logservice >= 0) {
    startservice(root[sid].sid_logservice, pause, sid);
  }
  if (chdir(MINITROOT) || chdir(root[sid].name)) {
    return -1;
  }
  if ((dir = open(".", O_RDONLY | O_CLOEXEC)) >= 0) {
    unsigned long len = 0;
    char *depends = 0;
    char **depv = 0;
    int depc = 0;
    if (!openreadclose("depends", &depends, &len)) {
      depv = split(depends, '\n', &depc, 0, 0);
      if (depv) {
        for (int i = 0; i < depc; i++) {
          if (depv[i][0] == '#') {
            continue;
          }
          int sid_dep = loadservice(depv[i]);
          if (sid_dep >= 0 && root[sid_dep].pid != 1) {
            startservice(sid_dep, 0, sid);
          }
        }
        free(depv);
      }
      free(depends);
      fchdir(dir);
    }
    pid = startnodep(sid, pause);
    // printf("started service %s with pid %u\n", root[sid].name, pid);
    close(dir);
  }
  return pid;
}

static void _puts(const char *s) { write(1, s, str_len(s)); }

void childhandler() {
  pid_t killed;
  do {
    killed = waitpid(-1, 0, WNOHANG);
    handlekilled(killed);
  } while (killed && killed != (pid_t)-1);
}

int main(int argc, char *argv[]) {
  int count = 0;
  struct pollfd pfd;
  time_t last = time(0);
  int nfds = 1;

  for (int i = 0; i < HISTORY; ++i) {
    history[i] = -1;
  }

  infd = open(MINITROOT "/in", O_RDWR | O_CLOEXEC);
  outfd = open(MINITROOT "/out", O_RDWR | O_NONBLOCK | O_CLOEXEC);

  if (getpid() == 1) {
    iam_init = 1;
    reboot(0);
  }

  if (infd < 0 || outfd < 0) {
    _puts("minit: could not open " MINITROOT "/in,out}\n");
    sulogin();
    nfds = 0;
  } else {
    pfd.fd = infd;
  }
  pfd.events = POLLIN;

  if (fcntl(infd, F_SETFD, FD_CLOEXEC) || fcntl(outfd, F_SETFD, FD_CLOEXEC)) {
    _puts("minit: could not set up " MINITROOT "/{in,out}\n");
    sulogin();
    nfds = 0;
  }

  // mtrace();

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
    char buf[1501];
    time_t now;
    childhandler();
    now = time(0);
    if (now < last || now - last > 30) {
      /* the system clock was reset, compensate */
      long diff = last - now;
      for (int j = 0; j <= maxprocess; ++j) {
        root[j].started_at -= diff;
      }
    }
    last = now;
    switch (poll(&pfd, nfds, 5000)) {
    case -1:
      if (errno == EINTR) {
        childhandler();
        break;
      }
      _puts("poll failed!\n");
      sulogin();
      /* what should we do if poll fails?! */
      break;
    case 1:
      len = read(infd, buf, 1500);
      if (len > 1) {
        int sid = -1, tmp;
        buf[len] = 0;
        if (buf[0] != 's' && ((sid = findservice(buf + 1)) < 0) && strcmp(buf, "d-") != 0) {
        error:
          write(outfd, "0", 1);
        } else {
          switch (buf[0]) {
          case 'p':
            write(outfd, buf, fmt_ulong(buf, root[sid].pid));
            break;
          case 'r':
            root[sid].respawn = 0;
            goto ok;
          case 'R':
            root[sid].respawn = 1;
            goto ok;
          case 'C':
            if (kill(root[sid].pid, 0)) {  /* check if still active */
              handlekilled(root[sid].pid); /* no!?! remove form active list */
              goto error;
            }
            goto ok;
          case 'P': {
            char *x = buf + str_len(buf) + 1;
            unsigned char c;
            tmp = 0;
            while ((c = *x++ - '0') < 10) {
              tmp = tmp * 10 + c;
            }
            if (tmp > 0) {
              if (kill(tmp, 0)) {
                goto error;
              }
            }
            root[sid].pid = tmp;
            goto ok;
          }
          case 's':
            sid = loadservice(buf + 1);
            if (sid < 0) {
              goto error;
            }
            if (root[sid].pid < 2) {
              root[sid].pid = 0;
              circsweep();
              sid = startservice(sid, 0, -1);
              if (sid == 0) {
                write(outfd, "0", 1);
                break;
              }
            }
          ok:
            write(outfd, "1", 1);
            break;
          case 'u':
            write(outfd, buf, fmt_ulong(buf, time(0) - root[sid].started_at));
            break;
          case 'd':
            write(outfd, "1:", 2);
            // printf("looking for sid father == %d\n", sid);
            for (int si = 0; si <= maxprocess; ++si) {
              // printf("pid of sid %d (%s) is %d, father sid %d\n", si, root[si].name,
              //        root[si].pid, root[si].sid_father);
              if (root[si].sid_father == sid) {
                write(outfd, root[si].name, str_len(root[si].name) + 1);
              }
            }
            write(outfd, "\0", 2);
            break;
          }
        }
      } else {
        if (buf[0] == 'h') {
          write(outfd, "1:", 2);
          for (int i = 0; i < HISTORY; ++i) {
            if (history[i] != -1) {
              write(outfd, root[history[i]].name, str_len(root[history[i]].name) + 1);
            }
          }
          write(outfd, "\0", 2);
        }
      }
      break;
    default:
      break;
    }
  }
}
