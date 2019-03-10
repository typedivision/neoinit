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

#include "djb/fmt.h"
#include "djb/str.h"

#include "minit.h"

typedef struct {
  char *name;
  pid_t pid;
  char respawn;
  char circular;
  time_t startedat;
  int father;
  int __stdin, __stdout;
  int logservice;
} process_t;

static process_t *root;

static int infd, outfd;
static int maxprocess = -1;
static int processalloc;
static int i_am_init;

extern int openreadclose(char *fn, char **buf, unsigned long *len);
extern char **split(char *buf, int c, int *len, int plus, int ofs);

extern char **environ;

#define HISTORY 10
int history[HISTORY];

char **Argv;

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
int addprocess(process_t *p) {
  if (maxprocess + 1 >= processalloc) {
    process_t *fump;
    processalloc += 8;
    if ((fump = (process_t *)realloc(root, processalloc * sizeof(process_t))) == 0) {
      return -1;
    }
    root = fump;
  }
  memmove(&root[++maxprocess], p, sizeof(process_t));
  return maxprocess;
}

/* load a service into the process data structure and return index or -1 if failed */
int loadservice(char *service) {
  process_t tmp;
  int fd;
  if (*service == 0) {
    return -1;
  }
  fd = findservice(service);
  if (fd >= 0) {
    return fd;
  }
  if (chdir(MINITROOT) || chdir(service)) {
    return -1;
  }
  if (!(tmp.name = strdup(service))) {
    return -1;
  }
  tmp.pid = 0;
  fd = open("respawn", O_RDONLY | O_CLOEXEC);
  if (fd >= 0) {
    tmp.respawn = 1;
    close(fd);
  } else {
    tmp.respawn = 0;
  }
  tmp.startedat = 0;
  tmp.circular = 0;
  tmp.__stdin = 0;
  tmp.__stdout = 1;

  char *logservice = alloca(str_len(service) + 5);
  strcpy(logservice, service);
  strcat(logservice, "/log");
  tmp.logservice = loadservice(logservice);
  if (tmp.logservice >= 0) {
    int pipefd[2];
    if (pipe(pipefd)) {
      free(tmp.name);
      return -1;
    }
    fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
    fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);
    root[tmp.logservice].__stdin = pipefd[0];
    tmp.__stdout = pipefd[1];
  }
  int new_service = addprocess(&tmp);
  if (new_service < 0) {
    free(tmp.name);
  }
  return new_service;
}

/* usage: isup(findservice("sshd")), returns nonzero if process is up */
int isup(int service) {
  if (service < 0) {
    return 0;
  }
  return (root[service].pid != 0);
}

int startservice(int service, int pause, int father);

void handlekilled(pid_t killed) {
  int i;
  if (killed == (pid_t)-1) {
    static int saidso;
    if (!saidso) {
      write(2, "all services exited\n", 21);
      saidso = 1;
    }
    if (i_am_init) {
      exit(0);
    }
  }
  if (killed == 0) {
    return;
  }
  i = findbypid(killed);
  // printf("pid %u exited, idx %d (%s)\n", killed, i, i >= 0 ? root[i].name : "[unknown]");
  if (i < 0) {
    return;
  }
  char *pidfile = 0;
  unsigned long len;
  if (!chdir(MINITROOT) && !chdir(root[i].name)) {
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
            // printf("replace idx %d (%s) with pid %u\n", i, root[i].name, pid);
            root[i].pid = pid;
            return;
          }
        }
      }
    }
  }
  root[i].pid = 0;
  if (root[i].respawn) {
    // printf("restarting %s\n", root[i].name);
    circsweep();
    startservice(i, time(0) - root[i].startedat < 1, root[i].father);
  } else {
    root[i].startedat = time(0);
    root[i].pid = 1;
  }
}

/* called from inside the service directory, return the PID or 0 on error */
pid_t forkandexec(int pause, int service) {
  char **argv = 0;
  int count = 0;
  pid_t p;
  int fd;
  unsigned long len;
  char *s = 0;
  int argc;
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
    if (i_am_init) {
      ioctl(0, TIOCNOTTY, 0);
      setsid();
      tcsetpgrp(0, getpgrp());
    }
    if (pause) {
      struct timespec req;
      req.tv_sec = 0;
      req.tv_nsec = 500000000;
      nanosleep(&req, 0);
    }
    if (!openreadclose("params", &s, &len)) {
      argv = split(s, '\n', &argc, 2, 1);
      if (argv[argc - 1]) {
        argv[argc - 1] = 0;
      } else {
        argv[argc] = 0;
      }
    } else {
      argv = (char **)alloca(2 * sizeof(char *));
      argv[1] = 0;
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
    argv[0] = strrchr(argv0, '/');
    if (argv[0]) {
      argv[0]++;
    } else {
      argv[0] = argv0;
    }
    if (root[service].__stdin != 0) {
      dup2(root[service].__stdin, 0);
      fcntl(0, F_SETFD, 0);
    }
    if (root[service].__stdout != 1) {
      dup2(root[service].__stdout, 1);
      dup2(root[service].__stdout, 2);
      fcntl(1, F_SETFD, 0);
      fcntl(2, F_SETFD, 0);
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
      root[service].pid = p;
      handlekilled(p);
      return root[service].pid;
    }
    return p;
  }
}

/* start a service, return nonzero on error */
int startnodep(int service, int pause) {
  /* step 1: see if the process is already up */
  if (isup(service)) {
    return 0;
  }
  /* step 2: fork and exec service, put PID in data structure */
  if (chdir(MINITROOT) || chdir(root[service].name)) {
    return -1;
  }
  root[service].startedat = time(0);
  root[service].pid = forkandexec(pause, service);
  return root[service].pid;
}

int startservice(int service, int pause, int father) {
  int dir;
  unsigned long len;
  char *s = 0;
  pid_t pid = 0;
  if (service < 0) {
    return 0;
  }
  if (root[service].circular) {
    return 0;
  }
  root[service].circular = 1;
  // printf("setting father of idx %d (%s) to idx %d (%s)\n", service, root[service].name, father,
  //        father >= 0 ? root[father].name : "minit");
  root[service].father = father;

  memmove(history + 1, history, sizeof(int) * ((HISTORY)-1));
  history[0] = service;

  if (root[service].logservice >= 0) {
    startservice(root[service].logservice, pause, service);
  }
  if (chdir(MINITROOT) || chdir(root[service].name)) {
    return -1;
  }
  if ((dir = open(".", O_RDONLY | O_CLOEXEC)) >= 0) {
    if (!openreadclose("depends", &s, &len)) {
      char **deps;
      int depc, i;
      deps = split(s, '\n', &depc, 0, 0);
      for (i = 0; i < depc; i++) {
        int Service, blacklisted, j;
        if (deps[i][0] == '#') {
          continue;
        }
        Service = loadservice(deps[i]);
        for (j = blacklisted = 0; Argv[j]; ++j) {
          if (Argv[j][0] == '-' && !strcmp(Argv[j] + 1, deps[i])) {
            blacklisted = 1;
            ++Argv[j];
            break;
          }
        }
        if (Service >= 0 && root[Service].pid != 1 && !blacklisted) {
          startservice(Service, 0, service);
        }
      }
      fchdir(dir);
    }
    pid = startnodep(service, pause);
    // printf("started service %s with pid %u\n", root[service].name, pid);
    close(dir);
  }
  chdir(MINITROOT);
  return pid;
}

void sulogin() { /* exiting on an initialization failure is not a good idea for init */
  char *argv[] = {"sulogin", 0};
  execve("/sbin/sulogin", argv, environ);
  _exit(1);
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

  Argv = argv;

  infd = open(MINITROOT "/in", O_RDWR | O_CLOEXEC);
  outfd = open(MINITROOT "/out", O_RDWR | O_NONBLOCK | O_CLOEXEC);

  if (getpid() == 1) {
    i_am_init = 1;
    reboot(0);
  }

  if (infd < 0 || outfd < 0) {
    _puts("minit: could not open " MINITROOT "/in or " MINITROOT "/out\n");
    sulogin();
    nfds = 0;
  } else {
    pfd.fd = infd;
  }
  pfd.events = POLLIN;

  fcntl(infd, F_SETFD, FD_CLOEXEC);
  fcntl(outfd, F_SETFD, FD_CLOEXEC);

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
    int i;
    char buf[1501];
    time_t now;
    childhandler();
    now = time(0);
    if (now < last || now - last > 30) {
      /* the system clock was reset, compensate */
      long diff = last - now;
      for (int j = 0; j <= maxprocess; ++j) {
        root[j].startedat -= diff;
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
      i = read(infd, buf, 1500);
      if (i > 1) {
        int idx = -1, tmp;
        buf[i] = 0;
        if (buf[0] != 's' && ((idx = findservice(buf + 1)) < 0) && strcmp(buf, "d-") != 0) {
        error:
          write(outfd, "0", 1);
        } else {
          switch (buf[0]) {
          case 'p':
            write(outfd, buf, fmt_ulong(buf, root[idx].pid));
            break;
          case 'r':
            root[idx].respawn = 0;
            goto ok;
          case 'R':
            root[idx].respawn = 1;
            goto ok;
          case 'C':
            if (kill(root[idx].pid, 0)) {  /* check if still active */
              handlekilled(root[idx].pid); /* no!?! remove form active list */
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
          }
            if (tmp > 0) {
              if (kill(tmp, 0)) {
                goto error;
              }
            }
            root[idx].pid = tmp;
            goto ok;
          case 's':
            idx = loadservice(buf + 1);
            if (idx < 0) {
              goto error;
            }
            if (root[idx].pid < 2) {
              root[idx].pid = 0;
              circsweep();
              idx = startservice(idx, 0, -1);
              if (idx == 0) {
                write(outfd, "0", 1);
                break;
              }
            }
          ok:
            write(outfd, "1", 1);
            break;
          case 'u':
            write(outfd, buf, fmt_ulong(buf, time(0) - root[idx].startedat));
            break;
          case 'd':
            write(outfd, "1:", 2);
            // printf("looking for father == %d\n", idx);
            for (int i = 0; i <= maxprocess; ++i) {
              // printf("pid of idx %d (%s) is %u, father is idx %d\n", i, root[i].name,
              //        root[i].pid, root[i].father);
              if (root[i].father == idx) {
                write(outfd, root[i].name, str_len(root[i].name) + 1);
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
