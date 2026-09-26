#include "../../../include/sndfile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Represent SNDFILE as a FILE for simplicity */
struct SNDFILE_tag { FILE* f; };

SNDFILE* sf_open(const char* path, int mode, SF_INFO* info) {
    const char* m = "rb";
    if (mode & SFM_WRITE) m = "wb";
    if (mode & SFM_RDWR) m = "r+b";
    FILE* f = fopen(path, m);
    if (!f) return NULL;
    SNDFILE* s = (SNDFILE*)malloc(sizeof(SNDFILE));
    if (!s) { fclose(f); return NULL; }
    s->f = f;
    if (info) {
        /* best-effort populate minimal fields */
        info->frames = 0;
        info->samplerate = 48000;
        info->channels = 1;
        info->format = 0;
        info->sections = 0;
        info->seekable = 1;
    }
    return s;
}

SNDFILE* sf_open_fd(int fd, int mode, SF_INFO* info, int close_desc) {
    (void)fd; (void)mode; (void)info; (void)close_desc;
    return NULL; /* not implemented for fake */
}

sf_count_t sf_read_short(SNDFILE* sndfile, short* ptr, sf_count_t items) {
    if (!sndfile || !sndfile->f) return 0;
    size_t r = fread(ptr, sizeof(short), (size_t)items, sndfile->f);
    return (sf_count_t)r;
}

sf_count_t sf_write_short(SNDFILE* sndfile, const short* ptr, sf_count_t items) {
    if (!sndfile || !sndfile->f) return 0;
    size_t w = fwrite(ptr, sizeof(short), (size_t)items, sndfile->f);
    return (sf_count_t)w;
}

int sf_write_sync(SNDFILE* sndfile) {
    if (!sndfile || !sndfile->f) return -1;
    return fflush(sndfile->f) == 0 ? 0 : -1;
}

int sf_close(SNDFILE* sndfile) {
    if (!sndfile) return 0;
    if (sndfile->f) fclose(sndfile->f);
    free(sndfile);
    return 0;
}
