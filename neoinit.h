#ifndef NEOINIT_H
#define NEOINIT_H

#ifndef NIROOT
#define NIROOT "/etc/neoinit"
#endif

#define BUFSIZE 1500

#define PID_DOWN         1
#define SID_INIT         0
#define SID_ACTIVE       1
#define SID_FINISHED     2
#define SID_STOPPED      3
#define SID_FAILED       4
#define SID_SETUP        5
#define SID_SETUP_FAILED 6

#define FMT_STATE 11 // str_len("setupfailed")

size_t fmt_state(char *buf, int state) {
  switch (state) {
  case SID_INIT:
    strcpy(buf, "init");
    break;
  case SID_ACTIVE:
    strcpy(buf, "active");
    break;
  case SID_FINISHED:
    strcpy(buf, "finished");
    break;
  case SID_STOPPED:
    strcpy(buf, "stopped");
    break;
  case SID_FAILED:
    strcpy(buf, "failed");
    break;
  case SID_SETUP:
    strcpy(buf, "setup");
    break;
  case SID_SETUP_FAILED:
    strcpy(buf, "setupfailed");
    break;
  default:
    strcpy(buf, "invalid");
    buf = "invalid";
  }
  return str_len(buf);
}

#endif /* NEOINIT_H */
