#pragma once

typedef int PaError;
typedef int PaDeviceIndex;
typedef unsigned long PaStreamCallbackFlags;
typedef void PaStream;

typedef struct PaStreamCallbackTimeInfo {
    double inputBufferAdcTime;
    double currentTime;
    double outputBufferDacTime;
} PaStreamCallbackTimeInfo;

typedef struct PaStreamParameters {
    PaDeviceIndex device;
    int channelCount;
    unsigned long sampleFormat;
    double suggestedLatency;
    void *hostApiSpecificStreamInfo;
} PaStreamParameters;

#define paNoError 0
#define paInt16 8
#define paClipOff 0
#define paContinue 0

PaError Pa_Initialize(void);
PaError Pa_Terminate(void);
PaDeviceIndex Pa_GetDefaultOutputDevice(void);
PaError Pa_OpenStream(PaStream **stream,
                      const PaStreamParameters *inputParameters,
                      const PaStreamParameters *outputParameters,
                      double sampleRate,
                      unsigned long framesPerBuffer,
                      unsigned long streamFlags,
                      int (*streamCallback)(const void *,
                                            void *,
                                            unsigned long,
                                            const PaStreamCallbackTimeInfo *,
                                            PaStreamCallbackFlags,
                                            void *),
                      void *userData);
PaError Pa_StartStream(PaStream *stream);
PaError Pa_StopStream(PaStream *stream);
PaError Pa_CloseStream(PaStream *stream);
const char *Pa_GetErrorText(PaError errorCode);
