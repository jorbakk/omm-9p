#include <u.h>
#include <libc.h>
#include <thread.h>

#define LOG(...) printloginfo(); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n");

const int stacksize = 8192;

void
thread1(void *arg)
{
}


void
threadmain(int argc, char **argv)
{
	LOG("dialing address: %s ...", addr);
	if ((fd = dial(addr, nil, nil, nil)) < 0)
		sysfatal("dial: %r");
	LOG("mounting address ...");
	if ((fs = fsmnt(fd, aname)) == nil)
		sysfatal("mount: %r");
	fs = xparse(name, &name);
	LOG("opening: %s", name);
	fid = fsopen(fs, name, mode);
	if (fid == nil)
		sysfatal("fsopen %s: %r", name);
	char *arg = "thread1";
	int thread1id = threadcreate(thread1, arg, stacksize);
	sleep(10000);
}
