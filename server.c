/*
 * Copyright 2022, 2024 Jörg Bakker
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
#include <stdbool.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include <sqlite3.h>

#include "log.h"
#include "dvb/dvb.h"

#define _DEBUG_ 1

#define QTYPE(p) ((p) & 0xF)
#define QOBJID(p) (((p) >> 4) & 0xFFFFFFFF)
#define IDSTR_MAXLEN 10
#define FAVID_MAXLEN 128
#define MAX_QRY      128
#define MAX_CTL      128
#define MAX_ARGC     32

/// 9P server
static char *srvname            = "ommserve";
static char *uname              = "omm";
static char *gname              = "omm";
static char *datafname          = "data";
static char *metafname          = "meta";
static char *queryfname         = "query";
// static char *queryres           = "query result";
static char *ctlfname           = "ctl";

/// Database backend
static sqlite3 *db              = NULL;
static sqlite3_stmt *idstmt     = NULL;
static sqlite3_stmt *countstmt  = NULL;
static sqlite3_stmt *metastmt   = NULL;
static sqlite3_stmt *favaddstmt = NULL;
static sqlite3_stmt *favdelstmt = NULL;
static const char *idqry        = "SELECT id FROM obj WHERE title like ? LIMIT 1 OFFSET ?";
static const char *countqry     = "SELECT COUNT(id) FROM obj WHERE title like ? LIMIT 1";
static const char *metaqry      = "SELECT type, title, path FROM obj WHERE id = ? LIMIT 1";
static const char *favaddqry    = "INSERT INTO fav VALUES (?,?,?,?)";
static const char *favdelqry    = "DELETE FROM fav WHERE listid = ? AND objid = ?";

/* static int nrootdir = 4; */

static const int nobjdir        = 2;
/// FIXME the following static variables are mutated by all clients
static int objcount             = 0;
static char querystr[MAX_QRY]   = "%";   /// By default, no query filter, show all table entries
static char favid[FAVID_MAXLEN] = "";    /// By default, no fav list, show all table entries
static char ctlstr[MAX_CTL]     = "";

enum
{
	Qroot = 0,
	Qobj,
	Qdata,
	Qmeta,
	Qquery,
	Qctl,
};

enum
{
	OTfile = 0,
	OTdvb,
};

const char *OBJTYPESTR_FILE = "file";
const char *OBJTYPESTR_DVB  = "dvb";

// Object format could be extended in the future to also store information
// about the codec and container format
const char *OBJFMT_AUDIO = "audio";
const char *OBJFMT_VIDEO = "video";
const char *OBJFMT_IMAGE = "image";

typedef union AuxData
{
	int fh;
	struct DvbStream *st;
} AuxData;

typedef struct AuxObj
{
	int ot;
	AuxData od;
} AuxObj;

static void closedb(void);
static int xfav(int argc, char *argv[]);
static void parse_args(int *argc, char *argv[MAX_ARGC], char *cmd);

static vlong
qpath(int type, int obj)
{
	return type | (obj << 4);
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
	// vlong length;

	q.type = 0;
	q.vers = 0;
	q.path = path;
	mode = 0444;
	// length = 0;
	name = "???";

	switch(QTYPE(path)) {
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
		// length = strlen(queryres);
		break;
	case Qctl:
		q.type = QTFILE;
		name = ctlfname;
		mode = 0666;
		break;
	default:
		sysfatal("dostat %#llux", path);
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
		// dir->length = length;
	}
}


static int
rootgen(int i, Dir *d, void *v)
{
	(void)v;
	int objoff = 2;
	sqlite3_bind_text(countstmt, 1, querystr, strlen(querystr), SQLITE_STATIC);
	int ret = sqlite3_step(countstmt);
	if (ret == SQLITE_ROW) {
		objcount = sqlite3_column_int(countstmt, 0);
		LOG("objcount: %d", objcount);
	}
	sqlite3_reset(countstmt);
	if (i >= objcount + objoff) {
		// End of root directory with objcount obj dirs and one query file
		return -1;
	}
	if (i == 0) {
		LOG("rootgen: ctl file");
		dostat(qpath(Qctl, i), nil, d);
	} else if (i == 1) {
		LOG("rootgen: query file");
		dostat(qpath(Qquery, i), nil, d);
	} else {
		// SELECT id FROM obj WHERE title like t LIMIT 1 OFFSET i
		sqlite3_bind_text(idstmt, 1, querystr, strlen(querystr), SQLITE_STATIC);
		sqlite3_bind_int(idstmt, 2, i - objoff);
		int ret = sqlite3_step(idstmt);
		if (ret == SQLITE_ROW) {
			int id = sqlite3_column_int(idstmt, 0);
			LOG("rootgen: select row %d returned objid: %d", i, id);
			/// 0-clt, 1-query, 2..-obj (objid in db starts with 1)
			dostat(qpath(Qobj, id + objoff - 2), nil, d);
		}
		sqlite3_reset(idstmt);
	}
	return 0;
}


static int
objgen(int i, Dir *d, void *v)
{
	(void)v;
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
		if(strcmp(ctlfname, name) == 0) {
			path = qpath(Qctl, 0);
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
	vlong path = r->fid->qid.path;
	vlong objid = QOBJID(path);
	char *objtype, *objpath;
	LOG("server open on qid path: 0%08llo, vers: %ld, type: %d",
		path, r->fid->qid.vers, r->fid->qid.type);
	r->ofcall.qid = r->fid->qid;
	if (QTYPE(path) == Qdata && r->fid->aux == nil) {
		// SELECT type, title, path FROM obj WHERE id = objid LIMIT 1
		sqlite3_bind_int(metastmt, 1, objid);
		int sqlret = sqlite3_step(metastmt);
		if (sqlret == SQLITE_ROW) {
			objtype = (char*)sqlite3_column_text(metastmt, 0);
			objpath = (char*)sqlite3_column_text(metastmt, 2);
			LOG("meta query returned file type: %s, path: %s", objtype, objpath);
			if (strcmp(objtype, OBJTYPESTR_FILE) == 0) {
				int fh = open(objpath, OREAD);
				if (fh == -1) {
					LOG("failed to open file media object");
				}
				else {
					AuxObj *ao = malloc(sizeof(AuxObj));
					ao->ot = OTfile;
					ao->od.fh = fh;
					r->fid->aux = ao;
				}
			}
			else if (strcmp(objtype, OBJTYPESTR_DVB) == 0) {
				struct DvbStream *stream = dvb_stream(objpath);
				if (stream == nil) {
					LOG("failed to open dvb media object");
				}
				else {
					AuxObj *ao = malloc(sizeof(AuxObj));
					ao->ot = OTdvb;
					ao->od.st = stream;
					r->fid->aux = ao;
				}
			}
		}
		sqlite3_reset(metastmt);
	}
	respond(r, nil);
}


static void
srvread(Req *r)
{
	LOG("server read on qid path: 0%08llo, objid: %lld, vers: %ld, type: %d",
		r->fid->qid.path, QOBJID(r->fid->qid.path), r->fid->qid.vers, r->fid->qid.type);
	vlong path, offset;
	path = r->fid->qid.path;
	offset = r->ifcall.offset;
	vlong objid = QOBJID(path);
	long count = r->ifcall.count;
	char *title;
	int sqlret;
	AuxObj *ao = nil;
	switch(QTYPE(path)) {
	case Qroot:
		dirread9p(r, rootgen, nil);
		break;
	case Qobj:
		dirread9p(r, objgen, nil);
		break;
	case Qdata:
		if (r->fid->aux == nil) {
			break;
		}
		ao = (AuxObj*)r->fid->aux;
		if (ao->ot == OTfile) {
			seek(ao->od.fh, offset, 0);
			size_t bytesread = read(ao->od.fh, r->ofcall.data, count);
			r->ofcall.count = bytesread;
		}
		else if (ao->ot == OTdvb) {
			size_t bytesread = dvb_read_stream(ao->od.st, r->ofcall.data, count);
			r->ofcall.count = bytesread;
		}
		break;
	case Qmeta:
		// SELECT type, title, path FROM obj WHERE id = objid LIMIT 1
		sqlite3_bind_int(metastmt, 1, objid);
		sqlret = sqlite3_step(metastmt);
		if (sqlret == SQLITE_ROW) {
			title   = (char*)sqlite3_column_text(metastmt, 1);
			LOG("meta query returned title: %s", title);
			readstr(r, title);
		}
		sqlite3_reset(metastmt);
		break;
	// case Qquery:
		// readstr(r, queryres);
		// break;
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
	path = r->fid->qid.path;
	/* offset = r->ifcall.offset; */
	count = r->ifcall.count;
	switch(QTYPE(path)) {
	case Qquery:
		snprint(querystr, count, "%s", r->ifcall.data);
		LOG("query: %s", querystr);
		break;
	case Qctl:
		snprint(ctlstr, count, "%s", r->ifcall.data);
		LOG("ctl: %s", ctlstr);
		int argc = 0;
		char *argv[MAX_ARGC] = {0};
		parse_args(&argc, argv, ctlstr);
		xfav(argc, argv);
		break;
	}
	r->ofcall.count = count;
	respond(r, nil);
}


static void
srvdestroyfid(Fid *fid)
{
	if(!fid->aux)
		return;
	AuxObj *ao = (AuxObj*)(fid->aux);
	switch (ao->ot) {
	case OTfile:
		LOG("closing file data handle");
		close(ao->od.fh);
		break;
	case OTdvb:
		LOG("closing dvb data handle");
		// FIXME this cause a double free
		dvb_free_stream(ao->od.st);
		break;
	}
	free(fid->aux);
	fid->aux = nil;
}


Srv server = {
	.attach     = srvattach,
	.walk1      = srvwalk1,
	.stat       = srvstat,
	.open       = srvopen,
	.read       = srvread,
	.write      = srvwrite,
	.destroyfid = srvdestroyfid,
};


int
threadmaybackground(void)
{
	return 1;
}


static void
startserver(void *arg)
{
	(void)arg;
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
	sqlite3_bind_text(countstmt, 1, querystr, strlen(querystr), SQLITE_STATIC);
	int ret = sqlite3_step(countstmt);
	if (ret == SQLITE_ROW) {
		objcount = sqlite3_column_int(countstmt, 0);
		LOG("objcount: %d", objcount);
	}
	sqlite3_reset(countstmt);
	if (sqlite3_prepare_v2(db, metaqry, -1, &metastmt, NULL)) {
		closedb();
		sysfatal("failed to prepare sql obj meta data statement");
	}
	if (sqlite3_prepare_v2(db, favaddqry, -1, &favaddstmt, NULL)) {
		closedb();
		sysfatal("failed to prepare fav insert statement");
	}
	if (sqlite3_prepare_v2(db, favdelqry, -1, &favdelstmt, NULL)) {
		closedb();
		sysfatal("failed to prepare fav delete statement");
	}
}


static void
closedb(void)
{
	LOG("closing db ...");
	sqlite3_close(db);
	LOG("db closed");
}


static void
opendvb(char *config_xml)
{
	dvb_init(config_xml);
	dvb_open();
}


static void
closedvb(void)
{
	dvb_close();
}


static void
parse_args(int *argc, char *argv[MAX_ARGC], char *cmd)
{
	int len = strlen(cmd);
	argv[0] = cmd;
	*argc = 1;
	char *del = cmd;
	while (*argc < 5) {
		del = strchr(del, ' ');
		if (!del) break;
		del++;
		// LOG("argc: %d, argv: %s", *argc, del);
		argv[*argc] = del;
		(*argc)++;
	}
	for (int i = 0; i < len; ++i) {
		if (cmd[i] == ' ') cmd[i] = '\0';
	}
}


bool
exec_stmt(sqlite3 *db, const char *stmt)
{
	char *err_msg = 0;
    int rc = sqlite3_exec(db, stmt, 0, 0, &err_msg);
    if (rc != SQLITE_OK ) {
        LOG("SQL error %s in statement: %s", err_msg, stmt);
        sqlite3_free(err_msg);
        return false;
    }
    return true;
}


static int
xfav(int argc, char *argv[])
{
	if (argc == 1 && strcmp(argv[0], "fav") != 0) {
		LOG("fav command expected, skipping");
		return 0;
	}
	if (argc == 4) {
		if (strcmp(argv[1], "add") == 0) {
			LOG("adding %s to favlist: %s", argv[3], argv[2]);
			/// INSERT INTO fav VALUES (?,?,?,?)
			/// TODO generate a fav entry id
			sqlite3_bind_int(favaddstmt, 1, 0);
			/// TODO user specific fav lists
			sqlite3_bind_text(favaddstmt, 2, NULL, 0, SQLITE_STATIC);
			sqlite3_bind_text(favaddstmt, 3, argv[2], strlen(argv[2]), SQLITE_STATIC);
			sqlite3_bind_text(favaddstmt, 4, argv[3], strlen(argv[3]), SQLITE_STATIC);
			int rc = sqlite3_step(favaddstmt);
			if (rc != SQLITE_DONE) {
				LOG("failed to add item to fav list: %d", rc);
			}
			sqlite3_reset(favaddstmt);
		} else if (strcmp(argv[1], "del") == 0) {
			/// DELETE FROM fav WHERE listid = ? AND objid = ?
			LOG("del %s from favlist: %s", argv[3], argv[2]);
			sqlite3_bind_text(favdelstmt, 1, argv[2], strlen(argv[2]), SQLITE_STATIC);
			sqlite3_bind_text(favdelstmt, 2, argv[3], strlen(argv[3]), SQLITE_STATIC);
			sqlite3_step(favdelstmt);
			int rc = sqlite3_step(favdelstmt);
			if (rc != SQLITE_DONE) {
				LOG("failed to delete item to fav list: %d", rc);
			}
			sqlite3_reset(favdelstmt);
		} else {
			LOG("fav subcmd unknown, skipping.");
		}
		return 0;
	} else if (argc == 3) {
		if (strcmp(argv[1], "set") == 0) {
			LOG("setting favlist to: %s", argv[2]);
			memcpy(favid, argv[2], strlen(argv[2]));
		} else {
			LOG("fav subcmd unknown, skipping.");
		}
		return 0;
	} else if (argc == 2) {
		if (strcmp(argv[1], "set") == 0) {
			LOG("setting favlist to none");
			memset(favid, 0, FAVID_MAXLEN);
		} else {
			LOG("fav subcmd unknown, skipping.");
		}
		return 0;
	}
	// LOG("argc: %d", argc);
	// if (argc >= 2) LOG("cmd: %s, arg0: %s", argv[0], argv[1]);
	LOG("suspicious command, skipping");
	return 0;
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
	if (argc == 3) {
		opendvb(argv[2]);
	}
	startserver(nil);
	stopserver();
	if (argc == 3) {
		closedb();
	}
	closedvb();
}
