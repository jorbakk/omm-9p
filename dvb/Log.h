#ifndef __LOG_INCLUDED__
#define __LOG_INCLUDED__

#include <math.h>
#include <time.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>

/* #define LOG(module, level, ...) printloginfo(); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); */
#define LOG(module, level, ...) printloginfo(); fprintf(stderr, (std::string(__VA_ARGS__)).c_str()); fprintf(stderr, "\n");
/* #define LOG(module, level, cxxstr) printloginfo(); fprintf(stderr, cxxstr.c_str()); fprintf(stderr, "\n"); */

void printloginfo(void);

#endif
