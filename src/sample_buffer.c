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
    
    int evicted = 0;
    
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
        evicted++;
    }
    
    // Also check capacity limit
    while (buf->count >= buf->capacity) {
        BufferedSample* oldest = &buf->samples[buf->tail];
        FreeSample(oldest);
        buf->tail = (buf->tail + 1) % buf->capacity;
        buf->count--;
        evicted++;
    }
    
    // Log eviction occasionally to show buffer is working
    static int evictLogCounter = 0;
    if (evicted > 0 && (++evictLogCounter % 300) == 0 && buf->count > 0) {
        double span = (double)(newTimestamp - buf->samples[buf->tail].timestamp) / 10000000.0;
        BufLog("Eviction: removed %d samples, count now %d, span=%.2fs\n", 
               evicted, buf->count, span);
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
// Deep copies all data under lock to prevent use-after-free from eviction,
// then releases lock before muxing (which can be slow)
BOOL SampleBuffer_WriteToFile(SampleBuffer* buf, const char* outputPath) {
    if (!buf || !buf->initialized || !outputPath) return FALSE;
    
    BufLog("WriteToFile: entering, getting lock...\n");
    EnterCriticalSection(&buf->lock);
    BufLog("WriteToFile: lock acquired\n");
    
    if (buf->count == 0) {
        LeaveCriticalSection(&buf->lock);
        BufLog("WriteToFile: buffer is empty\n");
        return FALSE;
    }
    
    int count = buf->count;
    
    // Calculate duration from timestamps
    double bufDuration = 0.0;
    int newestIdx = (buf->head - 1 + buf->capacity) % buf->capacity;
    bufDuration = (double)(buf->samples[newestIdx].timestamp - buf->samples[buf->tail].timestamp) / 10000000.0;
    BufLog("WriteToFile: %d samples, %.1fs to %s\n", count, bufDuration, outputPath);
    
    // Allocate sample array
    BufLog("WriteToFile: allocating %d samples (%zu bytes)\n", count, count * sizeof(MuxerSample));
    MuxerSample* samples = (MuxerSample*)malloc(count * sizeof(MuxerSample));
    if (!samples) {
        LeaveCriticalSection(&buf->lock);
        BufLog("WriteToFile: failed to allocate samples array\n");
        return FALSE;
    }
    
    // Find first timestamp for normalization
    LONGLONG firstTimestamp = 0;
    for (int i = 0; i < count; i++) {
        BufferedSample* src = &buf->samples[(buf->tail + i) % buf->capacity];
        if (src->data && src->size > 0) {
            firstTimestamp = src->timestamp;
            break;
        }
    }
    
    // Deep copy all samples while holding lock (prevents use-after-free)
    BufLog("WriteToFile: deep copying samples...\n");
    int copiedCount = 0;
    size_t totalBytes = 0;
    for (int i = 0; i < count; i++) {
        BufferedSample* src = &buf->samples[(buf->tail + i) % buf->capacity];
        if (src->data && src->size > 0) {
            samples[copiedCount].data = (BYTE*)malloc(src->size);
            if (samples[copiedCount].data) {
                memcpy(samples[copiedCount].data, src->data, src->size);
                samples[copiedCount].size = src->size;
                samples[copiedCount].timestamp = src->timestamp - firstTimestamp;
                samples[copiedCount].duration = src->duration;
                samples[copiedCount].isKeyframe = src->isKeyframe;
                totalBytes += src->size;
                copiedCount++;
            }
        }
    }
    BufLog("WriteToFile: copied %d samples (%zu bytes total)\n", copiedCount, totalBytes);
    
    // Capture config
    MuxerConfig config;
    config.width = buf->width;
    config.height = buf->height;
    config.fps = buf->fps;
    config.quality = buf->quality;
    config.seqHeader = buf->seqHeaderSize > 0 ? buf->seqHeader : NULL;
    config.seqHeaderSize = buf->seqHeaderSize;
    
    BufLog("WriteToFile: releasing lock, calling muxer...\n");
    LeaveCriticalSection(&buf->lock);
    // Lock released! All data is now in our own deep-copied memory
    
    // Mux to file (this can be slow, but we're not holding the lock)
    BOOL success = MP4Muxer_WriteFile(outputPath, samples, copiedCount, &config);
    BufLog("WriteToFile: muxer returned %s\n", success ? "OK" : "FAILED");
    
    // Free deep-copied sample data
    BufLog("WriteToFile: freeing sample copies...\n");
    for (int i = 0; i < copiedCount; i++) {
        if (samples[i].data) free(samples[i].data);
    }
    free(samples);
    BufLog("WriteToFile: done\n");
    
    return success;
}

// Get copies of samples for external muxing (caller must free)
// Deep copies all data under lock to prevent use-after-free from eviction
BOOL SampleBuffer_GetSamplesForMuxing(SampleBuffer* buf, MuxerSample** outSamples, int* outCount) {
    if (!buf || !buf->initialized || !outSamples || !outCount) return FALSE;
    
    *outSamples = NULL;
    *outCount = 0;
    
    EnterCriticalSection(&buf->lock);
    
    int count = buf->count;
    if (count == 0) {
        LeaveCriticalSection(&buf->lock);
        return FALSE;
    }
    
    // Allocate output array
    MuxerSample* samples = (MuxerSample*)malloc(count * sizeof(MuxerSample));
    if (!samples) {
        LeaveCriticalSection(&buf->lock);
        return FALSE;
    }
    
    // Find first timestamp for normalization
    LONGLONG firstTimestamp = 0;
    for (int i = 0; i < count; i++) {
        BufferedSample* src = &buf->samples[(buf->tail + i) % buf->capacity];
        if (src->data && src->size > 0) {
            firstTimestamp = src->timestamp;
            break;
        }
    }
    
    // Deep copy all samples while holding lock (prevents use-after-free)
    int copiedCount = 0;
    for (int i = 0; i < count; i++) {
        BufferedSample* src = &buf->samples[(buf->tail + i) % buf->capacity];
        if (src->data && src->size > 0) {
            samples[copiedCount].data = (BYTE*)malloc(src->size);
            if (samples[copiedCount].data) {
                memcpy(samples[copiedCount].data, src->data, src->size);
                samples[copiedCount].size = src->size;
                samples[copiedCount].timestamp = src->timestamp - firstTimestamp;
                samples[copiedCount].duration = src->duration;
                samples[copiedCount].isKeyframe = src->isKeyframe;
                copiedCount++;
            }
        }
    }
    
    LeaveCriticalSection(&buf->lock);
    
    *outSamples = samples;
    *outCount = copiedCount;
    return copiedCount > 0;
}

void SampleBuffer_SetSequenceHeader(SampleBuffer* buf, const BYTE* header, DWORD size) {
    if (!buf || !header || size == 0 || size > sizeof(buf->seqHeader)) return;
    
    EnterCriticalSection(&buf->lock);
    memcpy(buf->seqHeader, header, size);
    buf->seqHeaderSize = size;
    LeaveCriticalSection(&buf->lock);
    BufLog("SetSequenceHeader: %u bytes\n", size);
}
