#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>

#include <vlc/vlc.h>
#include <sqlite3.h>


char *basedir                  = NULL;
libvlc_instance_t *libvlc      = NULL;
sqlite3 *db                    = NULL;

/// Table obj
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

/// Table fav
/// Length of fav.userid should be LOGIN_NAME_MAX (POSIX)
/// id is not primary key, currently id is always set to 0
const char *crtfav_qry        =      \
"CREATE TABLE IF NOT EXISTS fav ("   \
"id     INTEGER(8), "                \
"userid TEXT(32), "                  \
"listid TEXT(16), "                  \
"objid  INTEGER(8) "                 \
")";
#if 0
const char *idxfav_qry        =      \
"CREATE INDEX favid_idx ON fav(id)";
#endif
const char *drpfav_qry        =      \
"DROP TABLE IF EXISTS fav";

/// Insert
sqlite3_stmt *insstmt          = NULL;
const char *ins_qry           =      \
"INSERT INTO obj VALUES (?,?,?,?,?,?)";

#define LOG(...) {fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n");};

enum {
	TYPE_NONE = 0, TYPE_AUDIO, TYPE_VIDEO, TYPE_IMG, TYPE_COUNT,
};

char *media_types[] = {
	"-", "audio", "video", "img", NULL,
};

char *audio_types[] = {
	"mp3", "wma", "ogg", "wav", NULL,
};

char *video_types[] = {
	"mp4", "mpeg", "mpg", "avi", "wmv", "flv", "opus", NULL,
};

char *img_types[] = {
	"jpg", "jpeg", "png", "gif", NULL,
};

int objid = 0;


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
	// if (!exec_stmt(db, idxfav_qry)) return false;
	return true;
}


int
media_type(char *fpath)
{
	/// FIXME a real hash map could be faster here,
	/// or better write an ffmpeg backend
	char *ext = strrchr(fpath, '.') + 1;
	if (!ext) return TYPE_NONE;
	for (char **t = audio_types; *t; ++t) {
		if (strcmp(ext, *t) == 0) return TYPE_AUDIO;
	}
	for (char **t = video_types; *t; ++t) {
		if (strcmp(ext, *t) == 0) return TYPE_VIDEO;
	}
	for (char **t = img_types; *t; ++t) {
		if (strcmp(ext, *t) == 0) return TYPE_IMG;
	}
	return TYPE_NONE;
}


int
tag(char *fpath)
{
	libvlc_media_t *media = libvlc_media_new_path(libvlc, fpath);
	if (!media) {
		LOG("failed to parse: %s, skipping", fpath);
	}
	libvlc_media_parse(media);
	char *title = libvlc_media_get_meta(media, libvlc_meta_Title);
	/// FIXME better insert NULL, not "" into the database
	if (!title) title = "";
	char *artist = libvlc_media_get_meta(media, libvlc_meta_Artist);
	if (!artist) artist = "";
	char *mtype = media_types[media_type(fpath)];
	if (!mtype) mtype = "";
	objid++;
	sqlite3_bind_int(insstmt, 1, objid);
	sqlite3_bind_text(insstmt, 2, "file", strlen("file"), SQLITE_STATIC);
	sqlite3_bind_text(insstmt, 3, mtype, strlen(mtype), SQLITE_STATIC);
	sqlite3_bind_text(insstmt, 4, artist, strlen(artist), SQLITE_STATIC);
	sqlite3_bind_text(insstmt, 5, title, strlen(title), SQLITE_STATIC);
	sqlite3_bind_text(insstmt, 6, fpath, strlen(fpath), SQLITE_STATIC);
	sqlite3_step(insstmt);
	sqlite3_reset(insstmt);
	LOG("title: %s", title);
	libvlc_media_release(media);
	return 0;
}


void
scan(char *basedir)
{
	LOG("scanning dir: %s", basedir);
	DIR *basedirfd = opendir(basedir);
	if (!basedirfd) {
		LOG("failed to open dir %s", basedir);
		return;
	}
	char fpath[PATH_MAX];
	struct dirent *entryfd;
	struct stat statbuf;
	while ((entryfd = readdir(basedirfd))) {
		if (entryfd->d_name[0] == '.') continue;
		sprintf(fpath, "%s/%s", basedir, entryfd->d_name);
		if (stat(fpath, &statbuf) == -1) continue;
		switch (statbuf.st_mode & S_IFMT) {
			case S_IFREG:
				tag(fpath);
				break;
			case S_IFDIR:
				scan(fpath);
				break;
			default:
				LOG("skipping: %s", entryfd->d_name);
		}
	}
	closedir(basedirfd);
}


int
main(int argc, char *argv[])
{
	if (argc != 3) {
		LOG("usage: %s dir db", argv[0]);
		return EXIT_FAILURE;
	} 
	basedir = argv[1];
    libvlc = libvlc_new(0, NULL);
    if (sqlite3_open(argv[2], &db)) {
    	LOG("failed to open db: %s", argv[2]);
    	return EXIT_FAILURE;
	}
	drop_tables(db);
	create_tables(db);
	sqlite3_prepare_v2(db, ins_qry, -1, &insstmt, NULL);
	exec_stmt(db, "BEGIN TRANSACTION");
	scan(basedir);
	exec_stmt(db, "COMMIT");
	sqlite3_close(db);
	libvlc_release(libvlc);

    return EXIT_SUCCESS;
}
