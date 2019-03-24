#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

/* open fd and read file into allocated buffer */
int openreadclose(char *fn, char **buf, unsigned long *len) {
  int fd = open(fn, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return -1;
  }
  if (!*buf) {
    *len = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    *buf = (char *)malloc(*len + 1);
    if (!*buf) {
      close(fd);
      return -1;
    }
  }
  *len = read(fd, *buf, *len);
  if (*len != (unsigned long)-1) {
    (*buf)[*len] = 0;
  }
  return close(fd);
}
