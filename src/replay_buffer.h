/*
 * Replay Buffer - ShadowPlay-style instant replay
 */

#ifndef REPLAY_BUFFER_H
#define REPLAY_BUFFER_H

#include <windows.h>
#include "config.h"

// Maximum encoded audio samples to store
#define MAX_AUDIO_SAMPLES 16384

typedef struct {
    BOOL enabled;
    int durationSeconds;
    CaptureMode captureSource;
    int monitorIndex;
    
    BOOL isBuffering;
    HANDLE bufferThread;
    CRITICAL_SECTION lock;
    
    volatile BOOL saveRequested;
    volatile BOOL saveComplete;
    char savePath[MAX_PATH];
    int frameWidth;
    int frameHeight;
    
    // Audio state
    BOOL audioEnabled;
    char audioSource1[256];
    char audioSource2[256];
    char audioSource3[256];
} ReplayBufferState;

BOOL ReplayBuffer_Init(ReplayBufferState* state);
void ReplayBuffer_Shutdown(ReplayBufferState* state);
BOOL ReplayBuffer_Start(ReplayBufferState* state, const AppConfig* config);
void ReplayBuffer_Stop(ReplayBufferState* state);
BOOL ReplayBuffer_Save(ReplayBufferState* state, const char* outputPath);
int ReplayBuffer_EstimateRAMUsage(int durationSeconds, int width, int height, int fps);
void ReplayBuffer_GetStatus(ReplayBufferState* state, char* buffer, int bufferSize);

#endif
