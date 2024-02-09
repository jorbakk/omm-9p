#ifndef _LOG_INCLUDED_
#define _LOG_INCLUDED_

#include <u.h>
#include <time.h>  // posix std headers should be included between u.h and libc.h
#include <stdio.h>
#include <libc.h>
#include <thread.h>

#define LOG(...) {printloginfo(); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n");};
static struct timespec curtime;

void printloginfo(void)
{
	long ms;  // Milliseconds
	time_t s; // Seconds
	pid_t tid;
	tid = threadid();
	clock_gettime(CLOCK_REALTIME, &curtime);
	s  = curtime.tv_sec;
	ms = round(curtime.tv_nsec / 1.0e6); // Convert nanoseconds to milliseconds
	if (ms > 999) {
	    s++;
	    ms = 0;
	}
	fprintf(stderr, "%"PRIdMAX".%03ld %d│ ", (intmax_t)s, ms, tid);
}

#endif
