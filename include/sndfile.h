/* Project-local minimal sndfile.h to shadow broken third_party header during build */
#ifndef PROJECT_SND_FILE_H
#define PROJECT_SND_FILE_H

typedef struct SNDFILE_tag SNDFILE;

typedef long long sf_count_t;

typedef struct SF_INFO {
    sf_count_t frames;
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
sf_count_t sf_seek(SNDFILE* sndfile, sf_count_t frames, int whence);
int sf_write_sync(SNDFILE* sndfile);
int sf_close(SNDFILE* sndfile);
int sf_error(SNDFILE* sndfile);

enum { SF_ERR_NO_ERROR = 0 };

/* Common format and endian constants (minimal stubs) */
#define SF_FORMAT_WAV    0x010000
#define SF_FORMAT_RF64   0x220000
#define SF_FORMAT_PCM_16 0x0002
#define SF_FORMAT_RAW    0x040000
#define SF_FORMAT_SUBMASK  0x0000FFFF
#define SF_FORMAT_TYPEMASK 0x0FFF0000
#define SF_FORMAT_ENDMASK  0x30000000
#define SF_ENDIAN_LITTLE 0x10000000
#define SF_ENDIAN_BIG    0x20000000

/* Error helper */
const char* sf_strerror(SNDFILE* sndfile);

#endif /* PROJECT_SND_FILE_H */
