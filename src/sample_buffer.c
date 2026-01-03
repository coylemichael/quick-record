/*
 * Sample Buffer Implementation
 * Thread-safe circular buffer for H.264 encoded frames
 * Muxing responsibility delegated to mp4_muxer module
 */

#include "sample_buffer.h"
#include "mp4_muxer.h"
#include "util.h"
#include "logger.h"
#include <stdio.h>

// Alias for logging
#define BufLog Logger_Log

// Free a single sample
static void FreeSample(BufferedSample* sample) {
    if (sample->data) {
        free(sample->data);
        sample->data = NULL;
    }
    sample->size = 0;
    sample->timestamp = 0;
    sample->duration = 0;
    sample->isKeyframe = FALSE;
}

// Evict oldest samples until buffer duration is under maxDuration
// Uses real timestamps: newest_timestamp - oldest_timestamp
static void EvictOldSamples(SampleBuffer* buf, LONGLONG newTimestamp) {
    if (buf->count == 0) return;
    
    // Keep evicting while (newest - oldest) > maxDuration
    while (buf->count > 0) {
        BufferedSample* oldest = &buf->samples[buf->tail];
        
        // Calculate current buffer span using real timestamps
        LONGLONG bufferSpan = newTimestamp - oldest->timestamp;
        
        if (bufferSpan <= buf->maxDuration) {
            break;  // Within limit, stop evicting
        }
        
        // Evict oldest sample
        FreeSample(oldest);
        buf->tail = (buf->tail + 1) % buf->capacity;
        buf->count--;
    }
    
    // Also check capacity limit
    while (buf->count >= buf->capacity) {
        BufferedSample* oldest = &buf->samples[buf->tail];
        FreeSample(oldest);
        buf->tail = (buf->tail + 1) % buf->capacity;
        buf->count--;
    }
}

BOOL SampleBuffer_Init(SampleBuffer* buf, int durationSeconds, int fps,
                        int width, int height, QualityPreset quality) {
    if (!buf) return FALSE;
    
    ZeroMemory(buf, sizeof(SampleBuffer));
    
    // Calculate capacity: frames for 1.5x duration (headroom)
    int capacity = (int)(durationSeconds * fps * 1.5);
    if (capacity < 100) capacity = 100;
    if (capacity > 100000) capacity = 100000;  // ~27 min at 60fps
    
    buf->samples = (BufferedSample*)calloc(capacity, sizeof(BufferedSample));
    if (!buf->samples) {
        BufLog("Failed to allocate %d samples\n", capacity);
        return FALSE;
    }
    
    buf->capacity = capacity;
    buf->count = 0;
    buf->head = 0;
    buf->tail = 0;
    buf->maxDuration = (LONGLONG)durationSeconds * 10000000LL;  // 100-ns units
    buf->width = width;
    buf->height = height;
    buf->fps = fps;
    buf->quality = quality;
    
    InitializeCriticalSection(&buf->lock);
    buf->initialized = TRUE;
    
    BufLog("SampleBuffer_Init: capacity=%d, maxDuration=%llds\n", 
           capacity, buf->maxDuration / 10000000LL);
    
    return TRUE;
}

void SampleBuffer_Shutdown(SampleBuffer* buf) {
    if (!buf) return;
    
    if (buf->initialized) {
        EnterCriticalSection(&buf->lock);
        
        // Free all samples
        for (int i = 0; i < buf->capacity; i++) {
            FreeSample(&buf->samples[i]);
        }
        
        free(buf->samples);
        buf->samples = NULL;
        
        LeaveCriticalSection(&buf->lock);
        DeleteCriticalSection(&buf->lock);
    }
    
    buf->initialized = FALSE;
    
    // Log cleanup is handled by replay_buffer.c
}

BOOL SampleBuffer_Add(SampleBuffer* buf, EncodedFrame* frame) {
    if (!buf || !buf->initialized || !frame || !frame->data) return FALSE;
    
    EnterCriticalSection(&buf->lock);
    
    // Evict old samples based on timestamp (keeps last maxDuration seconds)
    EvictOldSamples(buf, frame->timestamp);
    
    // Add to buffer (take ownership of data)
    BufferedSample* slot = &buf->samples[buf->head];
    
    // Free any existing data in slot (shouldn't happen after eviction)
    if (slot->data) {
        free(slot->data);
    }
    
    slot->data = frame->data;
    slot->size = frame->size;
    slot->timestamp = frame->timestamp;
    slot->duration = frame->duration;
    slot->isKeyframe = frame->isKeyframe;
    
    // Clear frame's pointer (we own it now)
    frame->data = NULL;
    frame->size = 0;
    
    buf->head = (buf->head + 1) % buf->capacity;
    buf->count++;
    
    LeaveCriticalSection(&buf->lock);
    
    return TRUE;
}

double SampleBuffer_GetDuration(SampleBuffer* buf) {
    if (!buf || !buf->initialized || buf->count == 0) return 0.0;
    
    EnterCriticalSection(&buf->lock);
    
    // Calculate duration from timestamps: newest - oldest
    int newestIdx = (buf->head - 1 + buf->capacity) % buf->capacity;
    BufferedSample* newest = &buf->samples[newestIdx];
    BufferedSample* oldest = &buf->samples[buf->tail];
    
    double duration = (double)(newest->timestamp - oldest->timestamp) / 10000000.0;
    
    LeaveCriticalSection(&buf->lock);
    
    return duration;
}

int SampleBuffer_GetCount(SampleBuffer* buf) {
    if (!buf || !buf->initialized) return 0;
    
    EnterCriticalSection(&buf->lock);
    int count = buf->count;
    LeaveCriticalSection(&buf->lock);
    
    return count;
}

size_t SampleBuffer_GetMemoryUsage(SampleBuffer* buf) {
    if (!buf || !buf->initialized) return 0;
    
    EnterCriticalSection(&buf->lock);
    
    size_t total = 0;
    int idx = buf->tail;
    for (int i = 0; i < buf->count; i++) {
        total += buf->samples[idx].size;
        idx = (idx + 1) % buf->capacity;
    }
    
    LeaveCriticalSection(&buf->lock);
    
    return total;
}

void SampleBuffer_Clear(SampleBuffer* buf) {
    if (!buf || !buf->initialized) return;
    
    EnterCriticalSection(&buf->lock);
    
    for (int i = 0; i < buf->capacity; i++) {
        FreeSample(&buf->samples[i]);
    }
    
    buf->head = 0;
    buf->tail = 0;
    buf->count = 0;
    
    LeaveCriticalSection(&buf->lock);
}

// Write buffered samples to MP4 file using muxer module
// Snapshots buffer contents, then delegates muxing to mp4_muxer
BOOL SampleBuffer_WriteToFile(SampleBuffer* buf, const char* outputPath) {
    if (!buf || !buf->initialized || !outputPath) return FALSE;
    
    EnterCriticalSection(&buf->lock);
    
    if (buf->count == 0) {
        LeaveCriticalSection(&buf->lock);
        BufLog("WriteToFile: buffer is empty\n");
        return FALSE;
    }
    
    // Calculate duration from timestamps
    double bufDuration = 0.0;
    if (buf->count > 0) {
        int newestIdx = (buf->head - 1 + buf->capacity) % buf->capacity;
        bufDuration = (double)(buf->samples[newestIdx].timestamp - buf->samples[buf->tail].timestamp) / 10000000.0;
    }
    BufLog("WriteToFile: %d samples, %.1fs to %s\n", buf->count, bufDuration, outputPath);
    
    // Allocate MuxerSample array to snapshot buffer contents
    MuxerSample* samples = (MuxerSample*)malloc(buf->count * sizeof(MuxerSample));
    if (!samples) {
        LeaveCriticalSection(&buf->lock);
        BufLog("WriteToFile: failed to allocate sample array\n");
        return FALSE;
    }
    
    // Snapshot buffer contents (copy pointers, muxer will only read)
    int sampleCount = 0;
    int idx = buf->tail;
    LONGLONG firstTimestamp = 0;  // Will normalize timestamps to start at 0
    
    // First pass: find the first timestamp
    for (int i = 0; i < buf->count; i++) {
        BufferedSample* src = &buf->samples[(buf->tail + i) % buf->capacity];
        if (src->data && src->size > 0) {
            firstTimestamp = src->timestamp;
            break;
        }
    }
    
    // Second pass: copy samples with normalized timestamps
    for (int i = 0; i < buf->count; i++) {
        BufferedSample* src = &buf->samples[idx];
        if (src->data && src->size > 0) {
            samples[sampleCount].data = src->data;
            samples[sampleCount].size = src->size;
            samples[sampleCount].timestamp = src->timestamp - firstTimestamp;  // Normalize to start at 0
            samples[sampleCount].duration = src->duration;
            samples[sampleCount].isKeyframe = src->isKeyframe;
            sampleCount++;
        }
        idx = (idx + 1) % buf->capacity;
    }
    
    // Build muxer config from buffer properties
    MuxerConfig config;
    config.width = buf->width;
    config.height = buf->height;
    config.fps = buf->fps;
    config.quality = buf->quality;
    
    LeaveCriticalSection(&buf->lock);
    
    // Delegate muxing to dedicated module
    BOOL success = MP4Muxer_WriteFile(outputPath, samples, sampleCount, &config);
    
    free(samples);
    
    return success;
}
