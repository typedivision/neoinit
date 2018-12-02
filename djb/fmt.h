#ifndef FMT_H
#define FMT_H

#define FMT_ULONG 40 // enough space to hold 2^128 - 1 in decimal, plus \0

extern unsigned int fmt_ulong(char *,unsigned long);
extern unsigned int fmt_str(char *,const char *);

#endif
