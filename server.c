/*
 * Copyright 2022 Jörg Bakker
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is furnished to do
 * so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <u.h>
#include <stdio.h>
#include <time.h>  // posix std headers should be included between u.h and libc.h
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include <9pclient.h>

#define _DEBUG_ 1
#define LOG(...) printloginfo(); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n");


static struct timespec curtime;

void printloginfo(void)
{
    long            ms; // Milliseconds
    time_t          s;  // Seconds
	pid_t tid;
	tid = threadid();
	timespec_get(&curtime, TIME_UTC);
    s  = curtime.tv_sec;
    ms = round(curtime.tv_nsec / 1.0e6); // Convert nanoseconds to milliseconds
    if (ms > 999) {
        s++;
        ms = 0;
    }
	fprintf(stderr, "%"PRIdMAX".%03ld %d│ ", (intmax_t)s, ms, tid);
}


void
srvopen(Req *r)
{
	LOG("server open");
	respond(r, nil);
}


static int hello_read = 0;

void
srvread(Req *r)
{
	LOG("server read");
	if (hello_read == 1) {
		r->ofcall.count = 0;
		r->ofcall.data = "";
		hello_read = 0;
	}
	else {
		r->ofcall.count = 6;
		r->ofcall.data = "hello\n";
		hello_read = 1;
	}
	respond(r, nil);
}


void
srvwrite(Req *r)
{
	LOG("server write");
	r->ofcall.count = r->ifcall.count;
	respond(r, nil);
}


Srv server = {
	.open  = srvopen,
	.read  = srvread,
	.write = srvwrite,
};


int
threadmaybackground(void)
{
	return 1;
}


void
start_server(void *arg)
{
	LOG("starting 9P server ...");
	char *srvname = "ommserver";
	char *mtpt = nil;
	server.tree = alloctree(nil, nil, DMDIR|0777, nil);
	createfile(server.tree->root, "ctl", nil, 0777, arg);
	server.foreground = 1;
	threadpostmountsrv(&server, srvname, mtpt, MREPL|MCREATE);
	LOG("9P server started.");
}


void
threadmain(int argc, char **argv)
{
	if (_DEBUG_) {
		chatty9pclient = 1;
		/* chattyfuse = 1; */
	}
	start_server(nil);
}
