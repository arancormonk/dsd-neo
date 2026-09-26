/* Minimal fake PortAudio header for local testing only */
#ifndef PORTAUDIO_H
#define PORTAUDIO_H

#ifdef __cplusplus
extern "C" {
#endif

typedef int PaDeviceIndex;
typedef int PaError;

typedef struct PaStream PaStream;

typedef struct PaHostApiInfo {
	int type;
	const char* name;
} PaHostApiInfo;

typedef struct PaDeviceInfo {
	int structVersion;
	const char* name;
	int hostApi;
	int maxInputChannels;
	int maxOutputChannels;
	double defaultLowInputLatency;
	double defaultLowOutputLatency;
	double defaultSampleRate;
} PaDeviceInfo;

typedef struct PaStreamParameters {
	PaDeviceIndex device;
	int channelCount;
	unsigned long sampleFormat;
	double suggestedLatency;
	void* hostApiSpecificStreamInfo;
} PaStreamParameters;

typedef struct PaStreamInfo {
	int structVersion;
	double inputLatency;
	double outputLatency;
	double sampleRate;
} PaStreamInfo;

/* Common return codes and constants */
#define paNoError 0
#define paNoDevice (-1)
#define paFramesPerBufferUnspecified 0
#define paNoFlag 0
#define paInt16 0x00000010
#define paInputOverflowed 0x00010000
#define paOutputUnderflowed 0x00020000

/* Minimal API surface used by the project. Implementations are provided
 * in third_party/fake_portaudio/src/fake_portaudio.c when present.
 */
PaError Pa_Initialize(void);
PaError Pa_Terminate(void);
const char* Pa_GetErrorText(PaError err);
int Pa_GetDeviceCount(void);
PaDeviceIndex Pa_GetDefaultInputDevice(void);
PaDeviceIndex Pa_GetDefaultOutputDevice(void);
const PaDeviceInfo* Pa_GetDeviceInfo(PaDeviceIndex i);
const PaHostApiInfo* Pa_GetHostApiInfo(int hostApi);
const char* Pa_GetVersionText(void);

PaError Pa_OpenStream(PaStream** stream, const PaStreamParameters* inputParameters,
					  const PaStreamParameters* outputParameters, double sampleRate,
					  unsigned long framesPerBuffer, unsigned long streamFlags,
					  void* streamCallback, void* userData);
PaError Pa_StartStream(PaStream* stream);
PaError Pa_CloseStream(PaStream* stream);

#ifdef __cplusplus
}
#endif

#endif /* PORTAUDIO_H */
