/*
 * AAC Audio Encoder
 * Uses Media Foundation AAC encoder
 */

#ifndef AAC_ENCODER_H
#define AAC_ENCODER_H

#include <windows.h>
#include "audio_capture.h"

// AAC output configuration
#define AAC_SAMPLE_RATE     48000
#define AAC_CHANNELS        2
#define AAC_BITRATE         192000  // 192 kbps

// Forward declaration
typedef struct AACEncoder AACEncoder;

// Encoded AAC sample
typedef struct {
    BYTE* data;
    int size;
    LONGLONG timestamp;   // 100ns units
    LONGLONG duration;    // 100ns units
} AACSample;

// Callback for encoded samples
typedef void (*AACEncoderCallback)(const AACSample* sample, void* userData);

// Create AAC encoder
AACEncoder* AACEncoder_Create(void);

// Destroy encoder
void AACEncoder_Destroy(AACEncoder* encoder);

// Set callback for encoded samples
void AACEncoder_SetCallback(AACEncoder* encoder, AACEncoderCallback callback, void* userData);

// Feed PCM samples to encoder
// pcmData: 16-bit stereo PCM at 48kHz
// pcmSize: size in bytes
// timestamp: presentation time in 100ns units
BOOL AACEncoder_Feed(AACEncoder* encoder, const BYTE* pcmData, int pcmSize, LONGLONG timestamp);

// Flush any remaining samples
void AACEncoder_Flush(AACEncoder* encoder);

// Get encoder info for muxer
BOOL AACEncoder_GetConfig(AACEncoder* encoder, BYTE** configData, int* configSize);

#endif // AAC_ENCODER_H
