#include "Log.h"


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
