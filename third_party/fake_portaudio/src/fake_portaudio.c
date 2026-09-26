/* Minimal fake PortAudio implementation for local testing only.
 * Provides basic stubs for the subset of the PortAudio API used by the
 * project so builds can complete when the real PortAudio is not present.
 */
#include "../include/portaudio.h"
#include <string.h>
#include <stdlib.h>

/* Simple fake device info */
static PaHostApiInfo fake_host = {0, "FakeHost"};
static PaDeviceInfo fake_devices[] = {
    {0, "Fake Default Device", 0, 2, 2, 0.01, 0.01, 48000.0}
};

PaError Pa_Initialize(void) { return paNoError; }
PaError Pa_Terminate(void) { return paNoError; }

const char* Pa_GetErrorText(PaError err) { (void)err; return "fake-portaudio: no error"; }
int Pa_GetDeviceCount(void) { return (int)(sizeof(fake_devices)/sizeof(fake_devices[0])); }
PaDeviceIndex Pa_GetDefaultInputDevice(void) { return 0; }
PaDeviceIndex Pa_GetDefaultOutputDevice(void) { return 0; }
const PaDeviceInfo* Pa_GetDeviceInfo(PaDeviceIndex i) {
    if (i < 0 || (size_t)i >= sizeof(fake_devices)/sizeof(fake_devices[0])) return NULL;
    return &fake_devices[i];
}
const PaHostApiInfo* Pa_GetHostApiInfo(int hostApi) { (void)hostApi; return &fake_host; }
const char* Pa_GetVersionText(void) { return "fake-portaudio 0.0"; }

PaError Pa_OpenStream(PaStream** stream, const PaStreamParameters* in, const PaStreamParameters* out,
                      double sampleRate, unsigned long framesPerBuffer, unsigned long streamFlags,
                      void* streamCallback, void* userData) {
    (void)streamCallback; (void)userData; (void)streamFlags; (void)framesPerBuffer;
    if (!stream) return paNoError;
    *stream = (PaStream*)malloc(1);
    return paNoError;
}
PaError Pa_StartStream(PaStream* stream) { (void)stream; return paNoError; }
PaError Pa_CloseStream(PaStream* stream) { if (stream) free(stream); return paNoError; }
