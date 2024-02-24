#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <dirent.h>

#include <vlc/vlc.h>
#include <sqlite3.h>


char *basedir                  = NULL;
static sqlite3 *db             = NULL;
static sqlite3_stmt *insstmt   = NULL;
static const char *insqry      = "INSERT INTO obj VALUES (?,?,?,?,?)";
static int id = 0;


int
tag(char *fpath, libvlc_instance_t *libvlc, sqlite3 *db)
{
    libvlc_media_t* media = libvlc_media_new_path(libvlc, fpath);
    libvlc_media_parse(media);
    char *title = libvlc_media_get_meta(media, libvlc_meta_Title);
    id++;
	sqlite3_bind_int(insstmt, 1, id);
	sqlite3_bind_text(insstmt, 2, "file", strlen("file"), SQLITE_STATIC);
	sqlite3_bind_text(insstmt, 3, "audio", strlen("audio"), SQLITE_STATIC);
	sqlite3_bind_text(insstmt, 4, title, strlen(title), SQLITE_STATIC);
	sqlite3_bind_text(insstmt, 5, fpath, strlen(fpath), SQLITE_STATIC);
	sqlite3_step(insstmt);
	sqlite3_reset(insstmt);
    fprintf(stderr, "title: %s\n", title);
    libvlc_media_release(media);
    return 0;
}


int
main(int argc, char *argv[])
{
	if (argc != 3) {
		fprintf(stderr, "usage: %s dir db\n", argv[0]);
		return EXIT_FAILURE;
	} 
	basedir = argv[1];
    libvlc_instance_t* libvlc = libvlc_new(0, NULL);
    if (sqlite3_open(argv[2], &db)) {
    	fprintf(stderr, "failed to open db: %s\n", argv[2]);
    	return EXIT_FAILURE;
	}
	sqlite3_prepare_v2(db, insqry, -1, &insstmt, NULL);
	fprintf(stderr, "scanning dir: %s\n", basedir);
	DIR *basedirfd = opendir(basedir);
	if (!basedirfd) {
		fprintf(stderr, "failed to open dir %s\n", basedir);
		return EXIT_FAILURE;
	}
    char fpath[1024];
	struct dirent *entryfd;
	while ((entryfd = readdir(basedirfd))) {
		if (entryfd->d_name[0] == '.') continue;
		// fprintf(stderr, "tagging: %s\n", entryfd->d_name);
		sprintf(fpath, "%s/%s", basedir, entryfd->d_name);
		tag(fpath, libvlc, db);
		// switch (entryfd->d_type) {
			// case DT_REG:
				// fprintf(stderr, "tagging: %s\n", entryfd->d_name);
				// break;
			// case DT_DIR:
				// fprintf(stderr, "DIR: %s\n", entryfd->d_name);
				// break;
			// default:
				// fprintf(stderr, "other: %s\n", entryfd->d_name);
		// }
	}
	closedir(basedirfd);
	sqlite3_close(db);
	libvlc_release(libvlc);

    return EXIT_SUCCESS;
}
