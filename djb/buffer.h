#ifndef BUFFER_H
#define BUFFER_H

typedef struct buffer {
  char *x;
  unsigned int p;
  unsigned int n;
  int fd;
  int (*op)();
} buffer;

#define BUFFER_INIT(op,fd,buf,len) { (buf), 0, (len), (fd), (op) }
#define BUFFER_OUTSIZE 8192

extern int buffer_put(buffer *,const char *,unsigned int);
extern int buffer_putflush(buffer *,const char *,unsigned int);
extern int buffer_puts(buffer *,const char *);
extern int buffer_putsflush(buffer *,const char *);

extern buffer *buffer_1;
extern buffer *buffer_2;

#endif
