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

#endif /* NEOINIT_H */
