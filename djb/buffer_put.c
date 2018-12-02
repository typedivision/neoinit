#include <errno.h>

#include "buffer.h"
#include "str.h"
#include "byte.h"

static int allwrite(int (*op)(),int fd,const char *buf,unsigned int len)
{
  int w;

  while (len) {
    w = op(fd,buf,len);
    if (w == -1) {
      if (errno == EINTR) continue;
      return -1; /* note that some data may have been written */
    }
    if (w == 0) ; /* luser's fault */
    buf += w;
    len -= w;
  }
  return 0;
}

int buffer_flush(buffer *s)
{
  int p;
 
  p = s->p;
  if (!p) return 0;
  s->p = 0;
  return allwrite(s->op,s->fd,s->x,p);
}

int buffer_put(buffer *s,const char *buf,unsigned int len)
{
  unsigned int n;
 
  n = s->n;
  if (len > n - s->p) {
    if (buffer_flush(s) == -1) return -1;
    /* now s->p == 0 */
    if (n < BUFFER_OUTSIZE) n = BUFFER_OUTSIZE;
    while (len > s->n) {
      if (n > len) n = len;
      if (allwrite(s->op,s->fd,buf,n) == -1) return -1;
      buf += n;
      len -= n;
    }
  }
  /* now len <= s->n - s->p */
  byte_copy(s->x + s->p,len,buf);
  s->p += len;
  return 0;
}

int buffer_putflush(buffer *s,const char *buf,unsigned int len)
{
  if (buffer_flush(s) == -1) return -1;
  return allwrite(s->op,s->fd,buf,len);
}

int buffer_puts(buffer *s,const char *buf)
{
  return buffer_put(s,buf,str_len(buf));
}

int buffer_putsflush(buffer *s,const char *buf)
{
  return buffer_putflush(s,buf,str_len(buf));
}
