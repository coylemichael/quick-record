/*
 * Replay Buffer - ShadowPlay-style instant replay
 * 
 * Uses RAM-based circular buffer of encoded H.264 samples.
 * On save: muxes buffered samples to MP4 (no re-encoding).
 */

#include "replay_buffer.h"
#include "h264_encoder.h"
#include "sample_buffer.h"
#include "capture.h"
#include "config.h"
#include "util.h"
#include "logger.h"
#include <stdio.h>

// Global state
static volatile BOOL g_stopBuffering = FALSE;
static H264MemoryEncoder g_encoder = {0};
static SampleBuffer g_sampleBuffer = {0};

extern CaptureState g_capture;
extern AppConfig g_config;

static DWORD WINAPI BufferThreadProc(LPVOID param);

// Alias for logging
#define ReplayLog Logger_Log

// ============================================================================
// PUBLIC API
// ============================================================================

BOOL ReplayBuffer_Init(ReplayBufferState* state) {
    if (!state) return FALSE;
    ZeroMemory(state, sizeof(ReplayBufferState));
    InitializeCriticalSection(&state->lock);
    return TRUE;
}

void ReplayBuffer_Shutdown(ReplayBufferState* state) {
    if (!state) return;
    ReplayBuffer_Stop(state);
    DeleteCriticalSection(&state->lock);
    
    // Logger cleanup is handled by Logger_Shutdown in main.c
}

BOOL ReplayBuffer_Start(ReplayBufferState* state, const AppConfig* config) {
    if (!state || !config) return FALSE;
    if (state->isBuffering) return TRUE;
    
    state->enabled = config->replayEnabled;
    state->durationSeconds = config->replayDuration;
    state->captureSource = config->replayCaptureSource;
    state->monitorIndex = config->replayMonitorIndex;
    
    if (!state->enabled) return FALSE;
    
    state->saveRequested = FALSE;
    state->saveComplete = FALSE;
    state->savePath[0] = '\0';
    
    g_stopBuffering = FALSE;
    state->bufferThread = CreateThread(NULL, 0, BufferThreadProc, state, 0, NULL);
    state->isBuffering = (state->bufferThread != NULL);
    
    return state->isBuffering;
}

void ReplayBuffer_Stop(ReplayBufferState* state) {
    if (!state || !state->isBuffering) return;
    
    g_stopBuffering = TRUE;
    if (state->bufferThread) {
        WaitForSingleObject(state->bufferThread, 10000);
        CloseHandle(state->bufferThread);
        state->bufferThread = NULL;
    }
    state->isBuffering = FALSE;
}

BOOL ReplayBuffer_Save(ReplayBufferState* state, const char* outputPath) {
    if (!state || !outputPath || !state->isBuffering) return FALSE;
    
    strncpy(state->savePath, outputPath, MAX_PATH - 1);
    state->saveComplete = FALSE;
    state->saveRequested = TRUE;
    
    // Wait for completion (max 30 sec for muxing large buffers)
    for (int i = 0; i < 3000 && !state->saveComplete; i++) {
        Sleep(10);
    }
    
    return state->saveComplete;
}

void ReplayBuffer_GetStatus(ReplayBufferState* state, char* buffer, int bufferSize) {
    if (!state || !buffer || bufferSize < 1) return;
    
    if (state->isBuffering) {
        double duration = SampleBuffer_GetDuration(&g_sampleBuffer);
        size_t memMB = SampleBuffer_GetMemoryUsage(&g_sampleBuffer) / (1024 * 1024);
        snprintf(buffer, bufferSize, "Replay: %.0fs (%zuMB)", duration, memMB);
    } else {
        strcpy(buffer, "Replay: OFF");
    }
}

int ReplayBuffer_EstimateRAMUsage(int durationSec, int w, int h, int fps) {
    // Estimate based on bitrate
    // At 90 Mbps, 60 sec = 90 * 60 / 8 = 675 MB
    float baseMbps = 75.0f;  // Medium quality default
    float megapixels = (float)(w * h) / 1000000.0f;
    float resScale = megapixels / 3.7f;
    if (resScale < 0.5f) resScale = 0.5f;
    if (resScale > 2.5f) resScale = 2.5f;
    float fpsScale = (float)fps / 60.0f;
    if (fpsScale < 0.5f) fpsScale = 0.5f;
    if (fpsScale > 2.0f) fpsScale = 2.0f;
    
    float mbps = baseMbps * resScale * fpsScale;
    float totalMB = (mbps * durationSec) / 8.0f;
    
    return (int)totalMB;
}

// ============================================================================
// CAPTURE THREAD
// ============================================================================

static DWORD WINAPI BufferThreadProc(LPVOID param) {
    ReplayBufferState* state = (ReplayBufferState*)param;
    if (!state) return 1;
    
    ReplayLog("BufferThread started (ShadowPlay RAM mode)\n");
    ReplayLog("Config: replayEnabled=%d, duration=%d, captureSource=%d, monitorIndex=%d\n",
              g_config.replayEnabled, g_config.replayDuration, 
              g_config.replayCaptureSource, g_config.replayMonitorIndex);
    ReplayLog("Config: replayFPS=%d, replayAspectRatio=%d, quality=%d\n",
              g_config.replayFPS, g_config.replayAspectRatio, g_config.quality);
    
    // Setup capture
    CaptureState* capture = &g_capture;
    RECT rect = {0};
    
    if (state->captureSource == MODE_ALL_MONITORS) {
        Capture_GetAllMonitorsBounds(&rect);
        Capture_SetAllMonitors(capture);
    } else {
        if (!Capture_GetMonitorBoundsByIndex(state->monitorIndex, &rect)) {
            POINT pt = {0, 0};
            Capture_GetMonitorFromPoint(pt, &rect, NULL);
        }
        Capture_SetMonitor(capture, state->monitorIndex);
    }
    
    int width = rect.right - rect.left;
    int height = rect.bottom - rect.top;
    ReplayLog("Raw monitor bounds: %dx%d (rect: %d,%d,%d,%d)\n", 
              width, height, rect.left, rect.top, rect.right, rect.bottom);
    
    // Apply aspect ratio adjustment if set
    if (g_config.replayAspectRatio > 0) {
        int ratioW = 0, ratioH = 0;
        Util_GetAspectRatioDimensions(g_config.replayAspectRatio, &ratioW, &ratioH);
        
        if (ratioW > 0 && ratioH > 0) {
            int oldW = width, oldH = height;
            
            // Use utility to calculate aspect-corrected rect
            rect = Util_CalculateAspectRect(rect, ratioW, ratioH);
            width = rect.right - rect.left;
            height = rect.bottom - rect.top;
            
            ReplayLog("Aspect ratio %d:%d applied: %dx%d -> %dx%d\n",
                      ratioW, ratioH, oldW, oldH, width, height);
        }
    }
    
    if (width <= 0 || height <= 0) {
        ReplayLog("Invalid capture size: %dx%d\n", width, height);
        return 1;
    }
    
    // Update capture to use cropped region
    Capture_SetRegion(capture, rect);
    
    state->frameWidth = width;
    state->frameHeight = height;
    
    int fps = g_config.replayFPS;
    if (fps < 30) fps = 30;
    if (fps > 120) fps = 120;
    
    ReplayLog("Final capture params: %dx%d @ %d FPS, duration=%ds, quality=%d\n", 
              width, height, fps, g_config.replayDuration, g_config.quality);
    
    // Initialize H.264 memory encoder
    ReplayLog("Calling H264Encoder_Init(%d, %d, %d, %d)...\n", width, height, fps, g_config.quality);
    if (!H264Encoder_Init(&g_encoder, width, height, fps, g_config.quality)) {
        ReplayLog("H264Encoder_Init failed - check h264_encoder_debug.txt for details\n");
        return 1;
    }
    ReplayLog("H264 encoder initialized\n");
    
    // Initialize sample buffer
    if (!SampleBuffer_Init(&g_sampleBuffer, g_config.replayDuration, fps, 
                           width, height, g_config.quality)) {
        ReplayLog("SampleBuffer_Init failed\n");
        H264Encoder_Shutdown(&g_encoder);
        return 1;
    }
    ReplayLog("Sample buffer initialized (max %ds)\n", g_config.replayDuration);
    
    // Timing - use floating point for precise frame intervals
    double frameIntervalMs = 1000.0 / (double)fps;  // 16.667ms for 60fps
    ReplayLog("Frame interval: %.4f ms (target fps=%d)\n", frameIntervalMs, fps);
    
    LARGE_INTEGER perfFreq, lastFrameTime, captureStartTime;
    QueryPerformanceFrequency(&perfFreq);
    QueryPerformanceCounter(&lastFrameTime);
    captureStartTime = lastFrameTime;
    
    int frameCount = 0;
    int lastLogFrame = 0;
    
    while (!g_stopBuffering) {
        // === SAVE REQUEST ===
        if (state->saveRequested) {
            double duration = SampleBuffer_GetDuration(&g_sampleBuffer);
            int count = SampleBuffer_GetCount(&g_sampleBuffer);
            
            // Calculate actual capture stats for diagnostics
            LARGE_INTEGER nowTime;
            QueryPerformanceCounter(&nowTime);
            double realElapsedSec = (double)(nowTime.QuadPart - captureStartTime.QuadPart) / perfFreq.QuadPart;
            double actualFPS = frameCount / realElapsedSec;
            
            ReplayLog("SAVE REQUEST: %d samples (video=%.2fs) after %.2fs real time\n", 
                      count, duration, realElapsedSec);
            ReplayLog("  Actual capture rate: %.2f fps (target: %d fps)\n", actualFPS, fps);
            ReplayLog("  Output path: %s\n", state->savePath);
            
            // Write buffer to file
            BOOL ok = SampleBuffer_WriteToFile(&g_sampleBuffer, state->savePath);
            
            ReplayLog("SAVE %s\n", ok ? "OK" : "FAILED");
            
            state->saveComplete = TRUE;  // Even if failed, we're done
            state->saveRequested = FALSE;
        }
        
        // === FRAME CAPTURE ===
        LARGE_INTEGER currentTime;
        QueryPerformanceCounter(&currentTime);
        double frameElapsedMs = (double)(currentTime.QuadPart - lastFrameTime.QuadPart) * 1000.0 / perfFreq.QuadPart;
        
        if (frameElapsedMs >= frameIntervalMs) {
            lastFrameTime = currentTime;
            
            // Calculate real wall-clock timestamp for this frame (100-ns units)
            double realElapsedSec = (double)(currentTime.QuadPart - captureStartTime.QuadPart) / perfFreq.QuadPart;
            UINT64 realTimestamp = (UINT64)(realElapsedSec * 10000000.0);  // Convert to 100-ns units
            
            BYTE* frameData = Capture_GetFrame(capture, NULL);
            
            if (frameData) {
                // Encode with REAL wall-clock timestamp for accurate playback
                EncodedFrame encoded = {0};
                if (H264Encoder_EncodeFrame(&g_encoder, frameData, realTimestamp, &encoded)) {
                    // Add to buffer (buffer takes ownership)
                    SampleBuffer_Add(&g_sampleBuffer, &encoded);
                }
                
                frameCount++;
                
                // Periodic log - now with actual FPS calculation
                if (frameCount - lastLogFrame >= fps * 5) {  // Every 5 seconds
                    LARGE_INTEGER nowTime;
                    QueryPerformanceCounter(&nowTime);
                    double realElapsedSec = (double)(nowTime.QuadPart - captureStartTime.QuadPart) / perfFreq.QuadPart;
                    double actualFPS = frameCount / realElapsedSec;
                    
                    double duration = SampleBuffer_GetDuration(&g_sampleBuffer);
                    size_t memMB = SampleBuffer_GetMemoryUsage(&g_sampleBuffer) / (1024 * 1024);
                    int count = SampleBuffer_GetCount(&g_sampleBuffer);
                    ReplayLog("Status: %d frames in %.1fs (actual=%.1f fps, target=%d fps), buffer=%.1fs, %zu MB\n", 
                              frameCount, realElapsedSec, actualFPS, fps, duration, memMB);
                    lastLogFrame = frameCount;
                }
            }
        } else {
            Sleep(1);
        }
    }
    
    // Cleanup
    ReplayLog("Shutting down...\n");
    
    // Flush encoder
    EncodedFrame flushed = {0};
    while (H264Encoder_Flush(&g_encoder, &flushed)) {
        SampleBuffer_Add(&g_sampleBuffer, &flushed);
    }
    
    H264Encoder_Shutdown(&g_encoder);
    SampleBuffer_Shutdown(&g_sampleBuffer);
    
    ReplayLog("BufferThread exit\n");
    return 0;
}
