// Minimal stub implementations for mbe-neo functions to satisfy link-time.
#include <stdlib.h>
#include <string.h>
#include "../../../../include/mbelib.h"

void mbe_initMbeParms(mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced) {
    if (cur_mp) { cur_mp->initialized = 1; }
    if (prev_mp) { prev_mp->initialized = 1; }
    if (prev_mp_enhanced) { prev_mp_enhanced->initialized = 1; }
}

const char* mbe_versionString(void) {
    return "mbelib-stub";
}

int mbe_decodeAmbe2400Parms(char* ambe_d, mbe_parms* cur_mp, mbe_parms* prev_mp) {
    (void)ambe_d; (void)cur_mp; (void)prev_mp; return 0;
}
int mbe_decodeAmbe2450Parms(char* ambe_d, mbe_parms* cur_mp, mbe_parms* prev_mp) {
    (void)ambe_d; (void)cur_mp; (void)prev_mp; return 0;
}
int mbe_decodeImbe4400Parms(char* imbe_d, mbe_parms* cur_mp, mbe_parms* prev_mp) {
    (void)imbe_d; (void)cur_mp; (void)prev_mp; return 0;
}

void mbe_processAmbe2400Dataf(float* aout_buf, int* errs, int* errs2, char* err_str, char ambe_d[49],
                              mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced, int uvquality) {
    (void)aout_buf; (void)errs; (void)errs2; (void)err_str; (void)ambe_d; (void)cur_mp; (void)prev_mp; (void)prev_mp_enhanced; (void)uvquality;
}

void mbe_processAmbe2400Data(short* aout_buf, int* errs, int* errs2, char* err_str, char ambe_d[49],
                             mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced, int uvquality) {
    (void)aout_buf; (void)errs; (void)errs2; (void)err_str; (void)ambe_d; (void)cur_mp; (void)prev_mp; (void)prev_mp_enhanced; (void)uvquality;
}

void mbe_processAmbe3600x2400Framef(float* aout_buf, int* errs, int* errs2, char* err_str, char ambe_fr[4][24],
                                    char ambe_d[49], mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced, int uvquality) {
    (void)aout_buf; (void)errs; (void)errs2; (void)err_str; (void)ambe_fr; (void)ambe_d; (void)cur_mp; (void)prev_mp; (void)prev_mp_enhanced; (void)uvquality;
}

void mbe_processAmbe3600x2400Frame(short* aout_buf, int* errs, int* errs2, char* err_str, char ambe_fr[4][24],
                                   char ambe_d[49], mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced, int uvquality) {
    (void)aout_buf; (void)errs; (void)errs2; (void)err_str; (void)ambe_fr; (void)ambe_d; (void)cur_mp; (void)prev_mp; (void)prev_mp_enhanced; (void)uvquality;
}

void mbe_processAmbe2450Dataf(float* aout_buf, int* errs, int* errs2, char* err_str, char ambe_d[49],
                              mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced, int uvquality) {
    (void)aout_buf; (void)errs; (void)errs2; (void)err_str; (void)ambe_d; (void)cur_mp; (void)prev_mp; (void)prev_mp_enhanced; (void)uvquality;
}

void mbe_processAmbe2450Data(short* aout_buf, int* errs, int* errs2, char* err_str, char ambe_d[49],
                             mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced, int uvquality) {
    (void)aout_buf; (void)errs; (void)errs2; (void)err_str; (void)ambe_d; (void)cur_mp; (void)prev_mp; (void)prev_mp_enhanced; (void)uvquality;
}

void mbe_processImbe4400Dataf(float* aout_buf, int* errs, int* errs2, char* err_str, char imbe_d[88],
                              mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced, int uvquality) {
    (void)aout_buf; (void)errs; (void)errs2; (void)err_str; (void)imbe_d; (void)cur_mp; (void)prev_mp; (void)prev_mp_enhanced; (void)uvquality;
}

void mbe_processImbe4400Data(short* aout_buf, int* errs, int* errs2, char* err_str, char imbe_d[88],
                             mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced, int uvquality) {
    (void)aout_buf; (void)errs; (void)errs2; (void)err_str; (void)imbe_d; (void)cur_mp; (void)prev_mp; (void)prev_mp_enhanced; (void)uvquality;
}

void mbe_processImbe7200x4400Framef(float* aout_buf, int* errs, int* errs2, char* err_str, char imbe_fr[8][23],
                                    char imbe_d[88], mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced, int uvquality) {
    (void)aout_buf; (void)errs; (void)errs2; (void)err_str; (void)imbe_fr; (void)imbe_d; (void)cur_mp; (void)prev_mp; (void)prev_mp_enhanced; (void)uvquality;
}

void mbe_processImbe7200x4400Frame(short* aout_buf, int* errs, int* errs2, char* err_str, char imbe_fr[8][23],
                                   char imbe_d[88], mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced, int uvquality) {
    (void)aout_buf; (void)errs; (void)errs2; (void)err_str; (void)imbe_fr; (void)imbe_d; (void)cur_mp; (void)prev_mp; (void)prev_mp_enhanced; (void)uvquality;
}

void mbe_demodulateAmbe3600x2400Data(char ambe_fr[4][24]) { (void)ambe_fr; }
void mbe_demodulateAmbe3600x2450Data(char ambe_fr[4][24]) { (void)ambe_fr; }
void mbe_demodulateImbe7200x4400Data(char imbe_fr[8][23]) { (void)imbe_fr; }
