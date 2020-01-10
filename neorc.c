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

void write_checked(int fd, const char *s, unsigned long len) {
  if (write(fd, s, len) != len) {
    carp("write failed!");
  }
}

int addservice(char *service) {
  if (str_start(service, NIROOT "/")) {
    service += sizeof(NIROOT "/") - 1;
  }
  char *x = service + str_len(service) - 1;
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
  write_checked(infd, buf, buf_len);
  return read(outfd, buf, BUFSIZE);
}

/* return PID, 0 if error */
pid_t __readpid(char *service, int *state) {
  buf[0] = 'p';
  int len = addreadwrite(service);
  if (len < 1) {
    return 0;
  }
  buf[len] = 0;
  char *s = strchr(buf, '@');
  if (!s) {
    return 0;
  }
  s[0] = 0;
  if (!str_len(buf)) {
    return 0;
  }
  if (state) {
    *state = atoi(s + 1);
  }
  return atoi(buf);
}

/* return nonzero if error */
int respawn(char *service, int yesno) {
  buf[0] = yesno ? 'R' : 'r';
  int len = addreadwrite(service);
  return (len != 1 || buf[0] == '0');
}

/* return nonzero if error */
int cancel(char *service) {
  buf[0] = 'c';
  int len = addreadwrite(service);
  return (len != 1 || buf[0] == '0');
}

/* return nonzero if error */
int setpid(char *service, pid_t pid) {
  buf[0] = 'P';
  int buf_len = addservice(service);
  if (buf_len + 10 > BUFSIZE) {
    return 0;
  }
  char *tmp = buf + buf_len + 1;
  tmp[fmt_ulong(tmp, pid)] = 0;
  write_checked(infd, buf, buf_len + str_len(tmp) + 2);
  int len = read(outfd, buf, BUFSIZE);
  return (len != 1 || buf[0] == '0');
}

/* return nonzero if error */
int clear(char *service) {
  buf[0] = 'C';
  int len = addreadwrite(service);
  return (len != 1 || buf[0] == '0');
}

/* return nonzero if error */
int startservice(char *service) {
  buf[0] = 's';
  int len = addreadwrite(service);
  return (len != 1 || buf[0] == '0');
}

/* return uptime, 0 if error */
unsigned long uptime(char *service) {
  buf[0] = 'u';
  int len = addreadwrite(service);
  if (len < 0) {
    return 0;
  }
  buf[len] = 0;
  return atoi(buf);
}

void dumpservices(char dump_cmd) {
  char tmp[16384];
  int i = 0;
  int j = 0;
  int done = 0;
  char first = 1;
  char last = 'x';
  write_checked(infd, &dump_cmd, 1);
  for (;;) {
    j = read(outfd, tmp, sizeof(tmp));
    if (j < 1) {
      break;
    }
    done = i = 0;
    if (first) {
      if (dump_cmd == 'h' && tmp[0] == '0') {
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
      write_checked(1, tmp + 2, j - 2);
    } else {
      write_checked(1, tmp, j);
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
  int i = 0;
  int j = 0;
  int done = 0;
  char first = 1;
  char last = 'x';
  buf[0] = 'd';
  int buf_len = addservice(service);
  write_checked(infd, buf, buf_len);
  for (;;) {
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
      write_checked(1, tmp + 2, j - 2);
    } else {
      write_checked(1, tmp, j);
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
        " -s\tprint the current state of the service\n"
        " -g\tget pid. print just the service PID\n"
        " -C\tclear. reset a finished service\n"
        " -P pid\tset PID of service\n"
        " -D\tprint services started as dependency\n"
        " -H\thistory. print last started services\n"
        " -L\tlist. print all services and its states");
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
    if (argc == 2 && argv[1][1] != 'H' && argv[1][1] != 'L') {
      int state = 0;
      pid_t pid = __readpid(argv[1], &state);
      if (buf[0] != '0') {
        if (pid > 0) {
          unsigned long ut = uptime(argv[1]);
          char which[FMT_STATE];
          char since[FMT_ULONG];
          which[fmt_state(which, state)] = 0;
          since[fmt_ulong(since, ut)] = 0;
          msg(argv[1], ": ", which, " ", since, "s");
          return 0;
        }
        carp(argv[1], ": get pid failed");
        return 1;
      }
      carp(argv[1], ": no such service");
      return 1;
    }
    int ret = 0;
    int sig = 0;
    pid_t pid = 0;
    if (argv[1][0] == '-') {
      switch (argv[1][1]) {
      case 'g':
        for (int i = 2; i < argc; ++i) {
          pid = __readpid(argv[i], NULL);
          if (pid < 2) {
            carp(argv[i], pid == 0 ? ": no such service" : ": service not running");
            ret = 1;
          } else {
            char tmp[FMT_ULONG];
            int len = 0;
            tmp[len = fmt_ulong(tmp, pid)] = '\n';
            write_checked(1, tmp, len + 1);
          }
        }
        break;
      case 's':
        for (int i = 2; i < argc; ++i) {
          int state;
          pid = __readpid(argv[i], &state);
          if (pid == 0) {
            carp(argv[i], ": no such service");
            ret = 1;
          } else {
            char tmp[FMT_STATE];
            int len = 0;
            tmp[len = fmt_state(tmp, state)] = '\n';
            write_checked(1, tmp, len + 1);
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
        for (int i = 2; i < argc; ++i) {
          if (startservice(argv[i]) || respawn(argv[i], 0)) {
            carp("could not start ", argv[i]);
            ret = 1;
          }
        }
        break;
      case 'd':
        for (int i = 2; i < argc; ++i) {
          pid = __readpid(argv[i], NULL);
          if (pid == 0) {
            carp(argv[i], ": no such service");
            ret = 1;
          } else if (pid < 2) {
            continue;
          } else {
            if (!cancel(argv[i])) {
              if (!kill(pid, SIGTERM)) {
                kill(pid, SIGCONT);
              }
            }
          }
        }
        break;
      case 'R':
        for (int i = 2; i < argc; ++i) {
          if (respawn(argv[i], 1)) {
            carp("could not set respawn for ", argv[i]);
            ret = 1;
          }
        }
        break;
      case 'r':
        for (int i = 2; i < argc; ++i) {
          if (respawn(argv[i], 0)) {
            carp("could not unset respawn for ", argv[i]);
            ret = 1;
          }
        }
        break;
      case 'u':
        for (int i = 2; i < argc; ++i) {
          if (startservice(argv[i]) || respawn(argv[i], 1)) {
            carp("could not start ", argv[i]);
            ret = 1;
          }
        }
        break;
      case 'C':
        for (int i = 2; i < argc; ++i) {
          if (clear(argv[i])) {
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
        dumpservices('h');
        break;
      case 'L':
        dumpservices('l');
        break;
      case 'D':
        dumpdependencies(argv[2]);
        break;
      }
    }
    return ret;
  dokill:
    for (int i = 2; i < argc; ++i) {
      pid = __readpid(argv[i], NULL);
      if (pid < 2) {
        carp(argv[i], pid == 0 ? ": no such service" : ": service not running");
        ret = 1;
      } else if (kill(pid, sig)) {
        char sigstr[FMT_ULONG];
        char pidstr[FMT_ULONG];
        char *err = 0;
        switch (errno) {
        case EINVAL:
          err = "invalid signal";
          break;
        case EPERM:
          err = "permission denied";
          break;
        case ESRCH:
          err = "no such pid";
          break;
        default:
          err = "unknown error";
        }
        sigstr[fmt_ulong(sigstr, sig)] = 0;
        pidstr[fmt_ulong(pidstr, pid)] = 0;
        carp(argv[i], ": could not send signal ", sigstr, " to PID ", pidstr, ": ", err);
        ret = 1;
      }
    }
    return ret;
  }
  carp("neoinit: could not open " NIROOT "/in or " NIROOT "/out");
  return 1;
}
