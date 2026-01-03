/*
 * H.264 Memory Encoder using IMFTransform
 * Encodes BGRA frames to H.264 NAL units in memory (no disk I/O)
 * Used for ShadowPlay-style instant replay buffer
 */

#ifndef H264_ENCODER_H
#define H264_ENCODER_H

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include "config.h"

// Encoded frame structure
typedef struct {
    BYTE* data;             // H.264 NAL unit data (caller must free)
    DWORD size;             // Size in bytes
    LONGLONG timestamp;     // Presentation time (100-ns units)
    LONGLONG duration;      // Frame duration (100-ns units)
    BOOL isKeyframe;        // TRUE if IDR frame
} EncodedFrame;

// Memory encoder state
typedef struct {
    IMFTransform* encoder;          // H.264 encoder MFT
    IMFMediaType* inputType;        // BGRA input format
    IMFMediaType* outputType;       // H.264 output format
    
    // Settings
    int width;
    int height;
    int fps;
    UINT32 bitrate;
    QualityPreset quality;
    
    // Timing
    UINT64 frameDuration;           // Ideal duration in 100-ns units (for fallback)
    UINT64 frameCount;
    LONGLONG lastTimestamp;         // Last frame's real timestamp (for duration calc)
    LARGE_INTEGER startTime;        // Real-time start (for timestamps)
    LARGE_INTEGER perfFreq;         // Performance counter frequency
    
    // State
    BOOL initialized;
    BOOL codecDataExtracted;
    
} H264MemoryEncoder;

// Initialize encoder with settings
// Returns TRUE on success
BOOL H264Encoder_Init(H264MemoryEncoder* enc, int width, int height, int fps, QualityPreset quality);

// Encode a single BGRA frame with explicit timestamp (100-ns units, 0 = use internal timing)
// outFrame->data is allocated by this function, caller must free it
// Returns TRUE on success
BOOL H264Encoder_EncodeFrame(H264MemoryEncoder* enc, const BYTE* bgraData, LONGLONG timestamp, EncodedFrame* outFrame);

// Flush encoder (get any remaining buffered frames)
// Call repeatedly until returns FALSE
BOOL H264Encoder_Flush(H264MemoryEncoder* enc, EncodedFrame* outFrame);

// Shutdown and release resources
void H264Encoder_Shutdown(H264MemoryEncoder* enc);

// Helper: Free an encoded frame's data
void EncodedFrame_Free(EncodedFrame* frame);

#endif // H264_ENCODER_H
