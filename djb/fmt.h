/* this header file comes from libowfat, http://www.fefe.de/libowfat/ */
#ifndef FMT_H
#define FMT_H

/* for size_t: */
#include <stddef.h>
/* for uint32_t */
#include <stdint.h>

#ifndef __pure__
#define __pure__
#endif

#define FMT_ULONG 40 /* enough space to hold 2^128 - 1 in decimal, plus \0 */

/* convert unsigned src integer 23 to ASCII '2','3', return number of
 * bytes of value in output format (2 in this example).
 * If dest is not NULL, write result to dest */
size_t fmt_ulong(char *dest,unsigned long src) __pure__;

/* copy str to dest until \0 byte, return number of copied bytes. */
size_t fmt_str(char *dest,const char *src) __pure__;

#endif
