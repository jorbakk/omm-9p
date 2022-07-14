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

#define QTYPE(p) ((p) & 0xF)
#define QOBJ(p) (((p) >> 4) & 0xFFFFFFFF)


static char *srvname = "ommserver";
static char *uname = "omm";
static char *gname = "omm";
static char *queryfname = "query";
static char *queryres = "Hello from 9P!\n";

static int nrootdir = 4;

enum
{
	Qroot = 0,
	Qmediaobj,
	Qqueryfile,
};


static vlong
qpath(int type, int obj)
{
	return type | (obj << 4);
}


static struct timespec curtime;

static void
printloginfo(void)
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


static void
dostat(vlong path, Qid *qid, Dir *dir)
{
	char *name;
	Qid q;
	ulong mode;
	vlong length;

	q.type = 0;
	q.vers = 0;
	q.path = path;
	mode = 0444;
	length = 0;
	name = "???";

	switch(QTYPE(path)) {
	default:
		sysfatal("dostat %#llux", path);

	case Qroot:
		q.type = QTDIR;
		name = "/";
		break;

	case Qmediaobj:
		q.type = QTDIR;
		char namestr[128];
		snprint(namestr, 5 ,"obj%d", QOBJ(path));
		name = namestr;
		break;

	case Qqueryfile:
		q.type = QTFILE;
		name = queryfname;
		mode = 0666;
		length = strlen(queryres);
		break;
	}

	if(qid)
		*qid = q;
	if(dir) {
		memset(dir, 0, sizeof *dir);
		dir->name = estrdup9p(name);
		dir->muid = estrdup9p("");
		dir->uid  = estrdup9p(uname);
		dir->gid  = estrdup9p(gname);
		dir->qid  = q;
		if(q.type == QTDIR)
			mode |= DMDIR | 0111;
		dir->mode = mode;
		dir->length = length;
	}
}


static int
rootgen(int i, Dir *d, void *v)
{
	if(i >= nrootdir)
		// End of directory
		return -1;
	if (i == 0) {
		dostat(qpath(Qqueryfile, i), nil, d);
	}
	else {
		dostat(qpath(Qmediaobj, i), nil, d);
	}
	return 0;
}


static void
srvattach(Req *r)
{
	/* dostat(0, &r->ofcall.qid, nil); */
	// Maybe more explicitly writing the path of the root dir ...
	dostat(QTDIR | Qroot, &r->ofcall.qid, nil);
	r->fid->qid = r->ofcall.qid;
	respond(r, nil);
}


static char*
srvwalk1(Fid *fid, char *name, Qid *qid)
{
	int i, dotdot;
	vlong path;
	path = fid->qid.path;
	dotdot = strcmp(name, "..") == 0;
	switch(QTYPE(path)) {
	default:
	NotFound:
		return "file not found";
	case Qroot:
		if(dotdot)
			break;
		for(i=0; i<nrootdir; i++) {
			if(strcmp(queryfname, name) == 0) {
				path = QTFILE | Qqueryfile;
				goto Found;
			}
			char namestr[128];
			snprint(namestr, 5 ,"obj%d", QOBJ(path));
			if(strcmp(namestr, name) == 0) {
				path = qpath(Qmediaobj, i);
				goto Found;
			}
		}
		goto NotFound;
	}

Found:
	dostat(path, qid, nil);
	fid->qid = *qid;
	return nil;
}


void
srvstat(Req *r)
{
	dostat(r->fid->qid.path, nil, &r->d);
	respond(r, nil);
}


static void
srvopen(Req *r)
{
	LOG("server open on qid path: %lld, vers: %ld, type: %c", r->fid->qid.path, r->fid->qid.vers, r->fid->qid.type);
	r->ofcall.qid = r->fid->qid;
	respond(r, nil);
}


static void
srvread(Req *r)
{
	LOG("server read on qid path: %lld, vers: %ld, type: %c", r->fid->qid.path, r->fid->qid.vers, r->fid->qid.type);
	vlong path;

	path = r->fid->qid.path;
	switch(QTYPE(path)) {
	case Qroot:
		dirread9p(r, rootgen, nil);
		break;
	case Qqueryfile:
		readstr(r, queryres);
		break;
	}
	respond(r, nil);
}


static void
srvwrite(Req *r)
{
	LOG("server write on qid path: %lld, vers: %ld, type: %c", r->fid->qid.path, r->fid->qid.vers, r->fid->qid.type);
	/* vlong offset; */
	vlong path;
	long count;
	char querystr[128];

	path = r->fid->qid.path;
	/* offset = r->ifcall.offset; */
	count = r->ifcall.count;
	switch(QTYPE(path)) {
	case Qqueryfile:
		snprint(querystr, count, "%s", r->ifcall.data);
		LOG("query: %s", querystr);
		break;
	}
	r->ofcall.count = count;
	respond(r, nil);
}


Srv server = {
	.attach = srvattach,
	.walk1  = srvwalk1,
	.stat   = srvstat,
	.open   = srvopen,
	.read   = srvread,
	.write  = srvwrite,
};


int
threadmaybackground(void)
{
	return 1;
}


static void
start_server(void *arg)
{
	LOG("starting 9P server ...");
	char *mtpt = nil;
	server.foreground = 1;
	threadpostmountsrv(&server, srvname, mtpt, MREPL | MCREATE);
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
