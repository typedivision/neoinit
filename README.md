![neoinit](neoinit.png)

[![Build Status](https://travis-ci.org/typedivision/neoinit.svg?branch=master)](https://travis-ci.org/typedivision/neoinit)

__neoinit__ is a service start and supervising daemon based on the
[minit](http://www.fefe.de/minit/) system init program.

It works well with the glibc or musl libc but has also sources included from libdjb/libowfat to
compile into that fast and tiny executable.
For detailed documentation on how to use it, see its [man page](man/neoinit.txt).

To interact with the daemon, there is __neorc__ - the neoinit run control, coming with a variety of
command line [options](man/neorc.txt).
