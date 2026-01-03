/*
 * Sample Buffer - Thread-safe circular buffer for encoded H.264 frames
 * Implements duration-based eviction for instant replay
 */

#ifndef SAMPLE_BUFFER_H
#define SAMPLE_BUFFER_H

#include <windows.h>
#include "h264_encoder.h"
#include "config.h"

// Stored sample in the buffer
typedef struct {
    BYTE* data;             // H.264 NAL unit data
    DWORD size;             // Size in bytes
    LONGLONG timestamp;     // Presentation time (100-ns units)
    LONGLONG duration;      // Frame duration (100-ns units)
    BOOL isKeyframe;        // TRUE if IDR frame
} BufferedSample;

// Circular sample buffer
typedef struct {
    BufferedSample* samples;    // Array of samples
    int capacity;               // Max samples in buffer
    int count;                  // Current sample count
    int head;                   // Next write position
    int tail;                   // Oldest sample position
    
    LONGLONG maxDuration;       // Target max duration (100-ns units)
    
    int width;                  // Video width
    int height;                 // Video height
    int fps;                    // Frame rate
    QualityPreset quality;      // Quality preset
    
    CRITICAL_SECTION lock;      // Thread safety
    BOOL initialized;
    
} SampleBuffer;

// Initialize buffer for given duration
BOOL SampleBuffer_Init(SampleBuffer* buf, int durationSeconds, int fps, 
                        int width, int height, QualityPreset quality);

// Shutdown and free all resources
void SampleBuffer_Shutdown(SampleBuffer* buf);

// Add an encoded frame to the buffer
// Takes ownership of frame->data, caller should not free it
BOOL SampleBuffer_Add(SampleBuffer* buf, EncodedFrame* frame);

// Get current buffered duration in seconds
double SampleBuffer_GetDuration(SampleBuffer* buf);

// Get current sample count
int SampleBuffer_GetCount(SampleBuffer* buf);

// Get total memory usage in bytes
size_t SampleBuffer_GetMemoryUsage(SampleBuffer* buf);

// Write all buffered samples to an MP4 file
// Uses passthrough muxing (no re-encoding)
BOOL SampleBuffer_WriteToFile(SampleBuffer* buf, const char* outputPath);

// Clear all samples from buffer
void SampleBuffer_Clear(SampleBuffer* buf);

#endif // SAMPLE_BUFFER_H
