/* this header file comes from libowfat, http://www.fefe.de/libowfat/ */
#ifndef BUFFER_H
#define BUFFER_H

/* for size_t: */
//#include <stddef.h>
/* for ssize_t: */
#include <sys/types.h>
/* for strlen */
//#include <string.h>

typedef struct buffer {
  char *x;		/* actual buffer space */
  size_t p;		/* current position */
  size_t n;		/* current size of string in buffer */
  size_t a;		/* allocated buffer size */
  ssize_t (*op)();	/* use read(2) or write(2) */
  void* cookie;			/* used internally by the to-stralloc buffers, and for buffer chaining */
  void (*deinit)(void*);	/* called to munmap/free cleanup, with a pointer to the buffer as argument */
  int fd;		/* passed as first argument to op */
} buffer;

#define BUFFER_INIT(op,fd,buf,len) { (buf), 0, 0, (len), (op), NULL, NULL, (fd) }
#define BUFFER_INSIZE 8192
#define BUFFER_OUTSIZE 8192

int buffer_flush(buffer* b);
int buffer_put(buffer* b,const char* x,size_t len);
int buffer_putflush(buffer* b,const char* x,size_t len);
int buffer_puts(buffer* b,const char* x);
int buffer_putsflush(buffer* b,const char* x);

#if defined(__GNUC__) && !defined(__LIBOWFAT_INTERNAL)
/* as a little gcc-specific hack, if somebody calls buffer_puts with a
 * constant string, where we know its length at compile-time, call
 * buffer_put with the known length instead */
#define buffer_puts(b,s) (__builtin_constant_p(s) ? buffer_put(b,s,strlen(s)) : buffer_puts(b,s))
#define buffer_putsflush(b,s) (__builtin_constant_p(s) ? buffer_putflush(b,s,strlen(s)) : buffer_putsflush(b,s))
#endif

extern buffer *buffer_1;
extern buffer *buffer_2;

#endif
