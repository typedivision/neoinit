#include <stdlib.h>

/* split buf into n strings that are separated by sep, return n as len
 * allocate plus more slots and leave the first ofs of them alone */
char **split(char *buf, int sep, int *len, int plus, int ofs) {
  int n = 1;
  char **v = 0;
  char **w;
  /* step 1: count tokens */
  char *s;
  for (s = buf; *s; s++) {
    if (*s == sep) {
      n++;
    }
  }
  /* step 2: allocate space for pointers */
  v = (char **)malloc((n + plus) * sizeof(char *));
  if (!v) {
    return 0;
  }
  w = v + ofs;
  *w++ = buf;
  for (s = buf;; s++) {
    while (*s && *s != sep) {
      s++;
    }
    if (*s == 0) {
      break;
    }
    if (*s == sep) {
      *s = 0;
      *w++ = s + 1;
    }
  }
  *len = w - v;
  return v;
}
