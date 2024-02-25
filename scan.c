#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <dirent.h>
#include <limits.h>

#include <vlc/vlc.h>
#include <sqlite3.h>


char *basedir                  = NULL;
sqlite3 *db                    = NULL;
const char *crtobj_qry         =     \
"CREATE TABLE IF NOT EXISTS obj ("   \
"id     INTEGER(8) PRIMARY KEY, "    \
"type   TEXT(16), "                  \
"fmt    TEXT(16), "                  \
"orig   TEXT(16), "                  \
"title  TEXT, "                      \
"path   TEXT "                       \
")";
const char *idxobj_qry        =      \
"CREATE INDEX objid_idx ON obj(id)";
const char *drpobj_qry        =      \
"DROP TABLE IF EXISTS obj";
const char *crtfav_qry        =      \
"CREATE TABLE IF NOT EXISTS fav ("   \
"id     INTEGER(8) PRIMARY KEY, "    \
"userid TEXT(128), "                 \
"listid TEXT(16), "                  \
"objid  INTEGER(8) "                 \
")";
const char *idxfav_qry        =      \
"CREATE INDEX favid_idx ON fav(id)";
const char *drpfav_qry        =      \
"DROP TABLE IF EXISTS fav";
sqlite3_stmt *insstmt          = NULL;
const char *ins_qry           =      \
"INSERT INTO obj VALUES (?,?,?,?,?,?)";
int objid = 0;

#define LOG(...) {fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n");};

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


bool
drop_tables(sqlite3 *db)
{
	if (!exec_stmt(db, drpobj_qry)) return false;
	if (!exec_stmt(db, drpfav_qry)) return false;
	return true;
}


bool
create_tables(sqlite3 *db)
{
	if (!exec_stmt(db, crtobj_qry)) return false;
	if (!exec_stmt(db, idxobj_qry)) return false;
	if (!exec_stmt(db, crtfav_qry)) return false;
	if (!exec_stmt(db, idxfav_qry)) return false;
	return true;
}


int
tag(sqlite3 *db, libvlc_instance_t *libvlc, char *fpath)
{
    libvlc_media_t* media = libvlc_media_new_path(libvlc, fpath);
    libvlc_media_parse(media);
    char *title = libvlc_media_get_meta(media, libvlc_meta_Title);
    if (!title) title = "";
    char *artist = libvlc_media_get_meta(media, libvlc_meta_Artist);
    if (!artist) artist = "";
    objid++;
	sqlite3_bind_int(insstmt,  1, objid);
	sqlite3_bind_text(insstmt, 2, "file",  strlen("file"),  SQLITE_STATIC);
	sqlite3_bind_text(insstmt, 3, "audio", strlen("audio"), SQLITE_STATIC);
	sqlite3_bind_text(insstmt, 4, artist,  strlen(artist),  SQLITE_STATIC);
	sqlite3_bind_text(insstmt, 5, title,   strlen(title),   SQLITE_STATIC);
	sqlite3_bind_text(insstmt, 6, fpath,   strlen(fpath),   SQLITE_STATIC);
	sqlite3_step(insstmt);
	sqlite3_reset(insstmt);
    LOG("title: %s", title);
    libvlc_media_release(media);
    return 0;
}


int
main(int argc, char *argv[])
{
	if (argc != 3) {
		LOG("usage: %s dir db", argv[0]);
		return EXIT_FAILURE;
	} 
	basedir = argv[1];
    libvlc_instance_t* libvlc = libvlc_new(0, NULL);
    if (sqlite3_open(argv[2], &db)) {
    	LOG("failed to open db: %s", argv[2]);
    	return EXIT_FAILURE;
	}
	drop_tables(db);
	create_tables(db);
	sqlite3_prepare_v2(db, ins_qry, -1, &insstmt, NULL);
	LOG("scanning dir: %s", basedir);
	DIR *basedirfd = opendir(basedir);
	if (!basedirfd) {
		LOG("failed to open dir %s", basedir);
		return EXIT_FAILURE;
	}
	exec_stmt(db, "BEGIN TRANSACTION");
    char fpath[PATH_MAX];
	struct dirent *entryfd;
	while ((entryfd = readdir(basedirfd))) {
		if (entryfd->d_name[0] == '.') continue;
		// LOG("tagging: %s", entryfd->d_name);
		sprintf(fpath, "%s/%s", basedir, entryfd->d_name);
		tag(db, libvlc, fpath);
		// switch (entryfd->d_type) {
			// case DT_REG:
				// LOG("tagging: %s", entryfd->d_name);
				// break;
			// case DT_DIR:
				// LOG("DIR: %s", entryfd->d_name);
				// break;
			// default:
				// LOG("other: %s", entryfd->d_name);
		// }
	}
	closedir(basedirfd);
	exec_stmt(db, "COMMIT");
	sqlite3_close(db);
	libvlc_release(libvlc);

    return EXIT_SUCCESS;
}
