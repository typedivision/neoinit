#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

/* open fd and read file into allocated buffer */
int openreadclose(char *fn, char **buf, unsigned long *len) {
  long rlen = *len;
  int fd = open(fn, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return -1;
  }
  if (!*buf) {
    rlen = lseek(fd, 0, SEEK_END);
    if (rlen <= 0) {
      close(fd);
      return -1;
    }
    lseek(fd, 0, SEEK_SET);
    *buf = (char *)malloc(rlen + 1);
    if (!*buf) {
      close(fd);
      return -1;
    }
  }
  rlen = read(fd, *buf, rlen);
  if (rlen >= 0) {
    (*buf)[rlen] = 0;
    *len = rlen;
  }
  close(fd);
  return 0;
}
