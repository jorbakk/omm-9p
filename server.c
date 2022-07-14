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

// TODO
// 1. Database backend


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
#define QOBJID(p) (((p) >> 4) & 0xFFFFFFFF)
/* #define QOBJID(p) (((p) >> 8) & 0xFFFFFFFF) */


static char *srvname    = "ommserver";
static char *uname      = "omm";
static char *gname      = "omm";
static char *datafname  = "data";
static char *metafname  = "meta";
static char *queryfname = "query";
static char *queryres   = "query result";

static int nrootdir = 4;
static int nobjdir = 2;

enum
{
	Qroot = 0,
	Qobj,
	Qdata,
	Qmeta,
	Qquery,
};


static vlong
qpath(int type, int obj)
{
	return type | (obj << 4);
	/* return type | (obj << 8); */
	/* return type | ((vlong)obj << 4); */
}


static struct timespec curtime;

static void
printloginfo(void)
{
    long ms; // Milliseconds
    time_t s;  // Seconds
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
logpath(char *logstr, vlong path)
{
	LOG("%s path: 0%08llo, type: %lld, objid: %lld", logstr, path, QTYPE(path), QOBJID(path));
}


static void
dostat(vlong path, Qid *qid, Dir *dir)
{
	logpath("stat", path);
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
	case Qobj:
		q.type = QTDIR;
		char namestr[128];
		snprint(namestr, 5 ,"obj%d", QOBJID(path));
		name = namestr;
		break;
	case Qdata:
		q.type = QTFILE;
		name = datafname;
		break;
	case Qmeta:
		q.type = QTFILE;
		name = metafname;
		break;
	case Qquery:
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
		dostat(qpath(Qquery, 0), nil, d);
	}
	else {
		dostat(qpath(Qobj, i), nil, d);
	}
	return 0;
}


static int
objgen(int i, Dir *d, void *v)
{
	if(i >= nobjdir)
		// End of directory entries
		return -1;
	if (i == 0) {
		dostat(qpath(Qdata, i), nil, d);
	}
	else {
		dostat(qpath(Qmeta, i), nil, d);
	}
	return 0;
}


static void
srvattach(Req *r)
{
	/* dostat(0, &r->ofcall.qid, nil); */
	// Maybe more explicitly writing the path of the root dir ...
	/* dostat(QTDIR | Qroot, &r->ofcall.qid, nil); */
	dostat(Qroot, &r->ofcall.qid, nil);
	r->fid->qid = r->ofcall.qid;
	respond(r, nil);
}


static char*
srvwalk1(Fid *fid, char *name, Qid *qid)
{
	int i, dotdot;
	vlong path;
	path = fid->qid.path;
	logpath("walk1 obj", path);
	LOG("walk1 name: %s", name);
	dotdot = strcmp(name, "..") == 0;
	switch(QTYPE(path)) {
	default:
	NotFound:
		return "obj not found";
	case Qroot:
		if(dotdot)
			break;
		for(i=0; i<nrootdir; i++) {
			if(strcmp(queryfname, name) == 0) {
				path = qpath(Qquery, 0);
				goto Found;
			}
			char namestr[128];
			snprint(namestr, 5 ,"obj%d", i);
			if(strncmp(namestr, name, 5) == 0) {
				LOG("FOUND obj");
				path = qpath(Qobj, i);
				goto Found;
			}
		}
		goto NotFound;
	case Qobj:
		if(dotdot) {
			path = Qroot;
			break;
		}
		if(strcmp(datafname, name) == 0) {
			path = qpath(Qdata, QOBJID(path));
			LOG("data file");
			goto Found;
		}
		if(strcmp(metafname, name) == 0) {
			path = qpath(Qmeta, QOBJID(path));
			LOG("meta file");
			goto Found;
		}
		goto NotFound;
		break;
	}

Found:
	logpath("new qid", path);
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
	LOG("server open on qid path: 0%08llo, vers: %ld, type: %d",
		r->fid->qid.path, r->fid->qid.vers, r->fid->qid.type);
	r->ofcall.qid = r->fid->qid;
	respond(r, nil);
}


static void
srvread(Req *r)
{
	LOG("server read on qid path: 0%08llo, vers: %ld, type: %d",
		r->fid->qid.path, r->fid->qid.vers, r->fid->qid.type);
	vlong path;
	path = r->fid->qid.path;
	vlong objid = QOBJID(path);
	char objstr[128];
	sprint(objstr, "%lld", objid);
	switch(QTYPE(path)) {
	case Qroot:
		dirread9p(r, rootgen, nil);
		break;
	case Qobj:
		dirread9p(r, objgen, nil);
		break;
	case Qdata:
		readstr(r, objstr);
		break;
	case Qmeta:
		readstr(r, objstr);
		break;
	case Qquery:
		readstr(r, queryres);
		break;
	}
	respond(r, nil);
}


static void
srvwrite(Req *r)
{
	LOG("server write on qid path: 0%08llo, vers: %ld, type: %d",
		r->fid->qid.path, r->fid->qid.vers, r->fid->qid.type);
	/* vlong offset; */
	vlong path;
	long count;
	char querystr[128];

	path = r->fid->qid.path;
	/* offset = r->ifcall.offset; */
	count = r->ifcall.count;
	switch(QTYPE(path)) {
	case Qquery:
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
