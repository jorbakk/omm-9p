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

static struct timespec curtime;

void printloginfo(void)
{
	long ms; // Milliseconds
	time_t s;  // Seconds
	pid_t tid;
	tid = syscall(SYS_gettid);
	/* tid = threadid(); */
	timespec_get(&curtime, TIME_UTC);
	/* clock_gettime(CLOCK_REALTIME, &curtime); */
	s  = curtime.tv_sec;
	ms = round(curtime.tv_nsec / 1.0e6); // Convert nanoseconds to milliseconds
	if (ms > 999) {
	    s++;
	    ms = 0;
	}
	/* fprintf(stderr, "%"PRIdMAX".%03ld %d│ ", (intmax_t)s, ms, tid); */
	fprintf(stderr, "%ld.%03ld %d│ ", s, ms, tid);
}

#endif
