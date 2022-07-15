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
// 1. Serve file data in data file
// 2. Test server on a network (replace u9fs)
// 3. Test multiple clients

#include <u.h>
#include <stdio.h>
#include <time.h>  // posix std headers should be included between u.h and libc.h
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include <sqlite3.h>

#define _DEBUG_ 1
#define LOG(...) printloginfo(); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n");

#define QTYPE(p) ((p) & 0xF)
#define QOBJID(p) (((p) >> 4) & 0xFFFFFFFF)
#define IDSTR_MAXLEN 10

static char *srvname           = "ommserver";
static char *uname             = "omm";
static char *gname             = "omm";
static char *datafname         = "data";
static char *metafname         = "meta";
static char *queryfname        = "query";
static char *queryres          = "query result";
static sqlite3 *db             = NULL;
static sqlite3_stmt *idstmt    = NULL;
static sqlite3_stmt *countstmt = NULL;
static sqlite3_stmt *metastmt  = NULL;
static const char *idqry       = "SELECT id FROM obj LIMIT 1 OFFSET ?";
static const char *countqry    = "SELECT COUNT(id) FROM obj LIMIT 1";
static const char *metaqry     = "SELECT title, path FROM obj WHERE id = ? LIMIT 1";

/* static int nrootdir = 4; */
static int objcount = 0;
static int nobjdir = 2;

enum
{
	Qroot = 0,
	Qobj,
	Qdata,
	Qmeta,
	Qquery,
};

static void closedb(void);

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
		char idstr[IDSTR_MAXLEN + 1];
		snprint(idstr, IDSTR_MAXLEN ,"%d", QOBJID(path));
		name = idstr;
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
	if(i >= objcount + 1)
		// End of root directory with objcount obj dirs and one query file
		return -1;
	if (i == 0) {
		LOG("rootgen: query file");
		dostat(qpath(Qquery, 0), nil, d);
	}
	else {
		// SELECT id FROM obj LIMIT 1 OFFSET i
		sqlite3_bind_int(idstmt, 1, i - 1);
		int ret = sqlite3_step(idstmt);
		if (ret == SQLITE_ROW) {
			int id = sqlite3_column_int(idstmt, 0);
			LOG("rootgen: select row %d returned objid: %d", i, id);
			dostat(qpath(Qobj, id), nil, d);
		}
		sqlite3_reset(idstmt);
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
	int dotdot;
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
		if(strcmp(queryfname, name) == 0) {
			path = qpath(Qquery, 0);
			goto Found;
		}
		char *endnum;
		vlong objid = strtoull(name, &endnum, 10);
		if (objid == 0 || endnum == name) {
			LOG("failed to convert obj file name to objid");
			/* werrstr("failed to convert obj file name to objid"); */
			goto NotFound;
        }
		LOG("FOUND obj");
		path = qpath(Qobj, objid);
		goto Found;
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
	LOG("server read on qid path: 0%08llo, objid: %lld, vers: %ld, type: %d",
		r->fid->qid.path, QOBJID(r->fid->qid.path), r->fid->qid.vers, r->fid->qid.type);
	vlong path;
	path = r->fid->qid.path;
	vlong objid = QOBJID(path);
	char objstr[128];
	sprint(objstr, "%lld", objid);
	const unsigned char *title, *objpath;
	int sqlret;
	switch(QTYPE(path)) {
	case Qroot:
		dirread9p(r, rootgen, nil);
		break;
	case Qobj:
		dirread9p(r, objgen, nil);
		break;
	case Qdata:
		// TODO move this from srvread() to srvopen() and just read from an opened file handle here
		// SELECT title, path FROM obj WHERE id = objid LIMIT 1
		sqlite3_bind_int(metastmt, 1, objid);
		sqlret = sqlite3_step(metastmt);
		if (sqlret == SQLITE_ROW) {
			objpath = sqlite3_column_text(metastmt, 1);
			LOG("meta query returned file path: %s", objpath);
			readstr(r, objpath);
		}
		sqlite3_reset(metastmt);
		break;
	case Qmeta:
		// SELECT title, path FROM obj WHERE id = objid LIMIT 1
		sqlite3_bind_int(metastmt, 1, objid);
		sqlret = sqlite3_step(metastmt);
		if (sqlret == SQLITE_ROW) {
			title   = sqlite3_column_text(metastmt, 0);
			LOG("meta query returned title: %s", title);
			readstr(r, title);
		}
		sqlite3_reset(metastmt);
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


int
threadmaybackground(void)
{
	return 1;
}


Srv server = {
	.attach = srvattach,
	.walk1  = srvwalk1,
	.stat   = srvstat,
	.open   = srvopen,
	.read   = srvread,
	.write  = srvwrite,
};

static void
startserver(void *arg)
{
	LOG("starting 9P server ...");
	char *mtpt = nil;
	server.foreground = 1;
	threadpostmountsrv(&server, srvname, mtpt, MREPL | MCREATE);
	LOG("9P server started.");
}


// FIXME should execute stop_server() on exit note (signal)
static void
stopserver(void)
{
	LOG("stopping server ...");
	LOG("server stopped");
}


static void
opendb(char *dbfile)
{
	LOG("opening db: %s", dbfile);
	if (sqlite3_open(dbfile, &db)) {
		sysfatal("failed to open db");
	}
	if (sqlite3_prepare_v2(db, idqry, -1, &idstmt, NULL)) {
		closedb();
		sysfatal("failed to prepare sql statement");
	}
	if (sqlite3_prepare_v2(db, countqry, -1, &countstmt, NULL)) {
		closedb();
		sysfatal("failed to prepare sql count statement");
	}
	int ret = sqlite3_step(countstmt);
	if (ret == SQLITE_ROW) {
		objcount = sqlite3_column_int(countstmt, 0);
		LOG("select of objcount returned: %d", objcount);
	}
	if (sqlite3_prepare_v2(db, metaqry, -1, &metastmt, NULL)) {
		closedb();
		sysfatal("failed to prepare sql obj meta data statement");
	}
}


static void
closedb(void)
{
	LOG("closing db ...");
	sqlite3_close(db);
	LOG("db closed");
}


void
threadmain(int argc, char **argv)
{
	if (argc == 1) {
		sysfatal("no db file provided");
	}
	if (_DEBUG_) {
		chatty9p = 1;
	}
	opendb(argv[1]);
	startserver(nil);
	stopserver();
	closedb();
}
