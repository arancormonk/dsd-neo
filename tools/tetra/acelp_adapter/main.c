/* dsd-neo TETRA vocoder pipe adapter; ETSI codec sources are supplied separately. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
#include "SOURCE.H"

enum { FRAME_BITS = 137, FRAME_SAMPLES = 240 };

int main(void)
{
    unsigned char input[FRAME_BITS];
    Word16 serial[FRAME_BITS + 1];
    Word16 parameters[24];
    Word16 samples[FRAME_SAMPLES];
    size_t i;
#ifdef _WIN32
    if (_setmode(_fileno(stdin), _O_BINARY) == -1 ||
        _setmode(_fileno(stdout), _O_BINARY) == -1) {
        fputs("tetra-acelp-adapter: cannot set binary pipe mode\n", stderr);
        return EXIT_FAILURE;
    }
#endif
    if (sizeof(Word16) != 2) {
        fputs("tetra-acelp-adapter: ETSI Word16 must be 16 bits\n", stderr);
        return EXIT_FAILURE;
    }
    Init_Decod_Tetra();
    for (;;) {
        size_t got = fread(input, 1, sizeof(input), stdin);
        if (got == 0 && feof(stdin))
            return EXIT_SUCCESS;
        if (got != sizeof(input)) {
            fputs("tetra-acelp-adapter: truncated input frame\n", stderr);
            return EXIT_FAILURE;
        }
        serial[0] = 0; /* dsd-neo currently sends only corrected codec bits. */
        for (i = 0; i < FRAME_BITS; ++i) {
            if (input[i] > 1) {
                fputs("tetra-acelp-adapter: input byte is not a bit\n", stderr);
                return EXIT_FAILURE;
            }
            serial[i + 1] = (Word16)input[i];
        }
        Bits2prm_Tetra(serial, parameters);
        Decod_Tetra(parameters, samples);
        Post_Process(samples, (Word16)FRAME_SAMPLES);
        if (fwrite(samples, sizeof(Word16), FRAME_SAMPLES, stdout) != FRAME_SAMPLES ||
            fflush(stdout) != 0) {
            fputs("tetra-acelp-adapter: PCM pipe write failed\n", stderr);
            return EXIT_FAILURE;
        }
    }
}
