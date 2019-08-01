#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "djb/errmsg.h"
#include "djb/fmt.h"
#include "djb/str.h"

#include "neoinit.h"

static int infd, outfd;

static char buf[BUFSIZE + 1];

int addservice(char *service) {
  char *x;
  if (str_start(service, NIROOT "/")) {
    service += sizeof(NIROOT "/") - 1;
  }
  x = service + str_len(service) - 1;
  while (x > service && *x == '/') {
    *x = 0;
    --x;
  }
  strncpy(buf + 1, service, BUFSIZE - 1);
  buf[BUFSIZE] = 0;
  return str_len(buf);
}

int addreadwrite(char *service) {
  int buf_len = addservice(service);
  write(infd, buf, buf_len);
  return read(outfd, buf, BUFSIZE);
}

/* return PID, 0 if error */
pid_t __readpid(char *service) {
  int len;
  buf[0] = 'p';
  len = addreadwrite(service);
  if (len < 0) {
    return 0;
  }
  buf[len] = 0;
  return atoi(buf);
}

/* return nonzero if error */
int respawn(char *service, int yesno) {
  int len;
  buf[0] = yesno ? 'R' : 'r';
  len = addreadwrite(service);
  return (len != 1 || buf[0] == '0');
}

/* return nonzero if error */
int setpid(char *service, pid_t pid) {
  char *tmp;
  int len;
  buf[0] = 'P';
  int buf_len = addservice(service);
  if (buf_len + 10 > BUFSIZE) {
    return 0;
  }
  tmp = buf + buf_len + 1;
  tmp[fmt_ulong(tmp, pid)] = 0;
  write(infd, buf, buf_len + str_len(tmp) + 2);
  len = read(outfd, buf, BUFSIZE);
  return (len != 1 || buf[0] == '0');
}

/* return nonzero if error */
int check_clear(char *service) {
  int len;
  buf[0] = 'C';
  len = addreadwrite(service);
  return (len != 1 || buf[0] == '0');
}

/* return nonzero if error */
int startservice(char *service) {
  int len;
  buf[0] = 's';
  len = addreadwrite(service);
  return (len != 1 || buf[0] == '0');
}

/* return uptime, 0 if error */
unsigned long uptime(char *service) {
  int len;
  buf[0] = 'u';
  len = addreadwrite(service);
  if (len < 0) {
    return 0;
  }
  buf[len] = 0;
  return atoi(buf);
}

void dumphistory() {
  char tmp[16384];
  int i, j;
  char first, last;
  first = 1;
  last = 'x';
  write(infd, "h", 1);
  for (;;) {
    int done;
    j = read(outfd, tmp, sizeof(tmp));
    if (j < 1) {
      break;
    }
    done = i = 0;
    if (first) {
      if (tmp[0] == '0') {
        carp("neoinit compiled without history support");
        return;
      }
      i += 2;
    } else {
      if (!tmp[0] && last == '\n') {
        break;
      }
    }
    for (; i < j; ++i) {
      if (!tmp[i]) {
        tmp[i] = done ? 0 : '\n';
        if (i + 1 < j && !tmp[i + 1]) {
          done = 1;
          j = i + 1;
        }
      }
    }
    if (first) {
      write(1, tmp + 2, j - 2);
    } else {
      write(1, tmp, j);
    }
    if (done) {
      break;
    }
    last = tmp[j - 1];
    first = 0;
  }
}

void dumpdependencies(char *service) {
  char tmp[16384];
  int i, j;
  char first, last;
  buf[0] = 'd';
  int buf_len = addservice(service);
  write(infd, buf, buf_len);
  first = 1;
  last = 'x';
  for (;;) {
    int done;
    j = read(outfd, tmp, sizeof(tmp));
    if (j < 1) {
      break;
    }
    done = i = 0;
    if (first) {
      if (tmp[0] == '0') {
        carp(service, ": no such service");
        return;
      }
      i += 2;
    } else {
      if (!tmp[0] && last == '\n') {
        break;
      }
    }
    for (; i < j; ++i) {
      if (!tmp[i]) {
        tmp[i] = done ? 0 : '\n';
        if (i + 1 < j && !tmp[i + 1]) {
          done = 1;
          j = i + 1;
        }
      }
    }
    if (first) {
      write(1, tmp + 2, j - 2);
    } else {
      write(1, tmp, j);
    }
    if (done) {
      break;
    }
    last = tmp[j - 1];
    first = 0;
  }
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    msg("usage:\tneorc [OPTIONS] SERVICE...\n"
        "options:\n"
        " -o\tonce. start service without respawn\n"
        " -u\tup. start service with respawn\n"
        " -d\tdown. signal service to stop, no respawn\n"
        " -R\tenable respawn\n"
        " -r\tdisable respawn\n"
        " -t\tsend service SIGTERM\n"
        " -k\tsend service SIGKILL\n"
        " -g\tget pid. print just the service PID\n"
        " -C\tclear. reset a finished service\n"
        " -P pid\tset PID of service\n"
        " -D\tprint services started as dependency\n"
        " -H\thistory. print last started services");
    return 0;
  }
  // errmsg_iam("neorc");
  infd = open(NIROOT "/in", O_WRONLY | O_CLOEXEC);
  outfd = open(NIROOT "/out", O_RDONLY | O_CLOEXEC);
  if (infd >= 0) {
    while (lockf(infd, F_LOCK, 1)) {
      carp("could not acquire lock");
      sleep(1);
    }
    if (argc == 2 && argv[1][1] != 'H') {
      pid_t pid = __readpid(argv[1]);
      if (buf[0] != '0') {
        unsigned long ut = uptime(argv[1]);
        char *what;
        char since[FMT_ULONG];

        if (pid > 1) {
          what = "UP ";
        } else if (pid == PID_FINISHED) {
          what = "FINISHED ";
        } else if (pid == PID_DOWN) {
          what = "DOWN ";
        } else if (pid == PID_SETUP) {
          what = "SETUP ";
        } else if (pid == PID_FAILED) {
          what = "FAILED ";
        } else if (pid == PID_SETUP_FAILED) {
          what = "SETUP_FAILED ";
        } else {
          what = "UNKNOWN ";
        }
        since[fmt_ulong(since, ut)] = 0;
        msg(argv[1], " ", what, since, "s");
        return 0;
      }
      carp(argv[1], ": no such service");
      return 1;
    }
    int i;
    int ret = 0;
    int sig = 0;
    pid_t pid;
    if (argv[1][0] == '-') {
      switch (argv[1][1]) {
      case 'g':
        for (i = 2; i < argc; ++i) {
          pid = __readpid(argv[i]);
          if (pid < 2) {
            carp(argv[i], pid == 0 ? ": no such service" : ": service not running");
            ret = 1;
          } else {
            char tmp[FMT_ULONG];
            int i;
            tmp[i = fmt_ulong(tmp, pid)] = '\n';
            write(1, tmp, i + 1);
          }
        }
        break;
      case 't':
        sig = SIGTERM;
        goto dokill;
        break;
      case 'k':
        sig = SIGKILL;
        goto dokill;
        break;
      case 'o':
        for (i = 2; i < argc; ++i) {
          if (startservice(argv[i]) || respawn(argv[i], 0)) {
            carp("could not start ", argv[i]);
            ret = 1;
          }
        }
        break;
      case 'd':
        for (i = 2; i < argc; ++i) {
          pid = __readpid(argv[i]);
          if (pid == 0) {
            carp(argv[i], ": no such service");
            ret = 1;
          } else if (pid < 2) {
            continue;
          } else {
            if (!respawn(argv[i], 0)) {
              if (!kill(pid, SIGTERM)) {
                kill(pid, SIGCONT);
              }
            }
          }
        }
        break;
      case 'R':
        for (i = 2; i < argc; ++i) {
          if (respawn(argv[i], 1)) {
            carp("could not set respawn for ", argv[i]);
            ret = 1;
          }
        }
        break;
      case 'r':
        for (i = 2; i < argc; ++i) {
          if (respawn(argv[i], 0)) {
            carp("could not unset respawn for ", argv[i]);
            ret = 1;
          }
        }
        break;
      case 'u':
        for (i = 2; i < argc; ++i) {
          if (startservice(argv[i]) || respawn(argv[i], 1)) {
            carp("could not start ", argv[i]);
            ret = 1;
          }
        }
        break;
      case 'C':
        for (i = 2; i < argc; ++i) {
          if (check_clear(argv[i])) {
            carp(argv[i], " could not be cleared");
            ret = 1;
          }
        }
        break;
      case 'P':
        pid = atoi(argv[1] + 2);
        if (pid > 1) {
          if (setpid(argv[2], pid)) {
            carp("could not set PID of service ", argv[2]);
            ret = 1;
          }
        }
        break;
      case 'H':
        dumphistory();
        break;
      case 'D':
        dumpdependencies(argv[2]);
        break;
      }
    }
    return ret;
  dokill:
    for (i = 2; i < argc; i++) {
      pid = __readpid(argv[i]);
      if (pid < 2) {
        carp(argv[i], pid == 0 ? ": no such service" : ": service not running");
        ret = 1;
      } else if (kill(pid, sig)) {
        char tmp[FMT_ULONG];
        char tmp2[FMT_ULONG];
        char *s;
        switch (errno) {
        case EINVAL:
          s = "invalid signal";
          break;
        case EPERM:
          s = "permission denied";
          break;
        case ESRCH:
          s = "no such pid";
          break;
        default:
          s = "unknown error";
        }
        tmp[fmt_ulong(tmp, sig)] = 0;
        tmp2[fmt_ulong(tmp2, pid)] = 0;
        carp(argv[i], ": could not send signal ", tmp, " to PID ", pid, ": ", s);
        ret = 1;
      }
    }
    return ret;
  }
  carp("neoinit: could not open " NIROOT "/in or " NIROOT "/out");
  return 1;
}
