/* Minimal fake sndfile.h for CMake configure testing only */
#ifndef SND_FILE_H
#define SND_FILE_H

typedef struct SNDFILE_tag SNDFILE;

typedef long long sf_count_t;

typedef struct SF_INFO {
    int frames;
    int samplerate;
    int channels;
    int format;
    int sections;
    int seekable;
} SF_INFO;

/* Mode flags (subset) */
#define SFM_READ 0x10
#define SFM_WRITE 0x20
#define SFM_RDWR 0x30

/* Minimal API surface used by the project */
SNDFILE* sf_open(const char* path, int mode, SF_INFO* info);
SNDFILE* sf_open_fd(int fd, int mode, SF_INFO* info, int close_desc);
sf_count_t sf_read_short(SNDFILE* sndfile, short* ptr, sf_count_t items);
sf_count_t sf_write_short(SNDFILE* sndfile, const short* ptr, sf_count_t items);
int sf_write_sync(SNDFILE* sndfile);
int sf_close(SNDFILE* sndfile);

#endif /* SND_FILE_H */
