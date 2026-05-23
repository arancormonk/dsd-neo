// SPDX-License-Identifier: ISC
#include <stdio.h>

#ifdef USE_PORTAUDIO
#include <portaudio.h>

#ifdef WIN32
#include <windows.h>
#endif

/**
 * @brief Enumerate and print PortAudio devices.
 *
 * Prints version info plus all known input/output devices, marking defaults.
 * When PortAudio is unavailable, emits a stub message instead.
 */
void
printPortAudioDevices() {
    int i, numDevices, defaultDisplayed;
    const PaDeviceInfo* deviceInfo;
    PaError err;

    Pa_Initialize();

    DSD_FPRINTF(stderr, "\nPortAudio version number = %d\nPortAudio version text = '%s'\n", Pa_GetVersion(),
                Pa_GetVersionText());

    numDevices = Pa_GetDeviceCount();
    if (numDevices < 0) {
        DSD_FPRINTF(stderr, "ERROR: Pa_GetDeviceCount returned 0x%x\n", numDevices);
        err = numDevices;
        goto error;
    }

    DSD_FPRINTF(stderr, "Number of devices = %d\n", numDevices);
    for (i = 0; i < numDevices; i++) {
        deviceInfo = Pa_GetDeviceInfo(i);
        DSD_FPRINTF(stderr, "--------------------------------------- device #%d\n", i);

        /* Mark global and API specific default devices */
        defaultDisplayed = 0;
        if (i == Pa_GetDefaultInputDevice()) {
            DSD_FPRINTF(stderr, "[ Default Input");
            defaultDisplayed = 1;
        } else if (i == Pa_GetHostApiInfo(deviceInfo->hostApi)->defaultInputDevice) {
            const PaHostApiInfo* hostInfo = Pa_GetHostApiInfo(deviceInfo->hostApi);
            DSD_FPRINTF(stderr, "[ Default %s Input", hostInfo->name);
            defaultDisplayed = 1;
        }

        if (i == Pa_GetDefaultOutputDevice()) {
            DSD_FPRINTF(stderr, (defaultDisplayed ? "," : "["));
            DSD_FPRINTF(stderr, " Default Output");
            defaultDisplayed = 1;
        } else if (i == Pa_GetHostApiInfo(deviceInfo->hostApi)->defaultOutputDevice) {
            const PaHostApiInfo* hostInfo = Pa_GetHostApiInfo(deviceInfo->hostApi);
            DSD_FPRINTF(stderr, (defaultDisplayed ? "," : "["));
            DSD_FPRINTF(stderr, " Default %s Output", hostInfo->name);
            defaultDisplayed = 1;
        }

        if (defaultDisplayed) {
            DSD_FPRINTF(stderr, " ]\n");
        }

        /* print device info fields */
#ifdef WIN32
        { /* Use wide char on windows, so we can show UTF-8 encoded device names */
            wchar_t wideName[MAX_PATH];
            MultiByteToWideChar(CP_UTF8, 0, deviceInfo->name, -1, wideName, MAX_PATH - 1);
            wprintf(L"Name                        = %s\n", wideName);
        }
#else
        DSD_FPRINTF(stderr, "Name                        = %s\n", deviceInfo->name);
#endif
        DSD_FPRINTF(stderr, "Host API                    = %s\n", Pa_GetHostApiInfo(deviceInfo->hostApi)->name);
        DSD_FPRINTF(stderr, "Max inputs = %d", deviceInfo->maxInputChannels);
        DSD_FPRINTF(stderr, ", Max outputs = %d\n", deviceInfo->maxOutputChannels);
        DSD_FPRINTF(stderr, "Default sample rate         = %8.2f\n", deviceInfo->defaultSampleRate);
    }

    Pa_Terminate();

    DSD_FPRINTF(stderr, "----------------------------------------------\n");
    return;

error:
    Pa_Terminate();
    DSD_FPRINTF(stderr, "An error occured while using the portaudio stream\n");
    DSD_FPRINTF(stderr, "Error number: %d\n", err);
    DSD_FPRINTF(stderr, "Error message: %s\n", Pa_GetErrorText(err));
}

#else

/** @brief Stub printer when PortAudio support is not compiled in. */
void
printPortAudioDevices() {
    DSD_FPRINTF(stderr, "PortAudio not supported in this build of dsd\n");
}

#endif
