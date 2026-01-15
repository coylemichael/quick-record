/*
 * Replay Buffer - ShadowPlay-style instant replay
 * 
 * Uses RAM-based circular buffer of encoded HEVC samples.
 * On save: muxes buffered samples to MP4 (no re-encoding).
 * Full GPU pipeline: DXGI capture → GPU color convert → NVENC (native API)
 */

#include "replay_buffer.h"
#include "nvenc_encoder.h"
#include "sample_buffer.h"
#include "capture.h"
#include "config.h"
#include "util.h"
#include "logger.h"
#include "audio_capture.h"
#include "aac_encoder.h"
#include "mp4_muxer.h"
#include "gpu_converter.h"
#include <stdio.h>
#include <mmsystem.h>  // For timeBeginPeriod/timeEndPeriod

#pragma comment(lib, "winmm.lib")

// Global state
static NVENCEncoder* g_encoder = NULL;
static SampleBuffer g_sampleBuffer = {0};

// HEVC sequence header (VPS/SPS/PPS) for muxing
static BYTE g_seqHeader[256];
static DWORD g_seqHeaderSize = 0;

// Audio state
static AudioCaptureContext* g_audioCapture = NULL;
static AACEncoder* g_aacEncoder = NULL;
static MuxerAudioSample* g_audioSamples = NULL;
static int g_audioSampleCount = 0;
static int g_audioSampleCapacity = 0;
static CRITICAL_SECTION g_audioLock;
static BOOL g_audioLockInitialized = FALSE;  // Track CS initialization
static BYTE* g_aacConfigData = NULL;
static int g_aacConfigSize = 0;
static LONGLONG g_audioMaxDuration = 0;  // Max duration in 100-ns units for eviction

extern CaptureState g_capture;
extern AppConfig g_config;

static DWORD WINAPI BufferThreadProc(LPVOID param);

// Alias for logging
#define ReplayLog Logger_Log

// Callback for draining completed encoded frames into sample buffer
// Called from NVENC output thread - must be thread-safe
static void DrainCallback(EncodedFrame* frame, void* userData) {
    SampleBuffer* buffer = (SampleBuffer*)userData;
    if (frame && frame->data && buffer) {
        SampleBuffer_Add(buffer, frame);
    }
}

// Audio callback - stores encoded AAC samples
static void AudioEncoderCallback(const AACSample* sample, void* userData) {
    if (!sample || !sample->data || sample->size <= 0) return;
    
    EnterCriticalSection(&g_audioLock);
    
    // Time-based eviction: remove samples older than max duration
    // This matches video buffer eviction behavior
    if (g_audioSampleCount > 0 && g_audioMaxDuration > 0) {
        int evicted = 0;
        while (g_audioSampleCount > 0) {
            LONGLONG oldest = g_audioSamples[0].timestamp;
            LONGLONG span = sample->timestamp - oldest;
            
            if (span <= g_audioMaxDuration) {
                break;  // Within duration limit
            }
            
            // Evict oldest sample
            if (g_audioSamples[0].data) {
                free(g_audioSamples[0].data);
            }
            memmove(g_audioSamples, g_audioSamples + 1, 
                    (g_audioSampleCount - 1) * sizeof(MuxerAudioSample));
            g_audioSampleCount--;
            evicted++;
        }
        
        // Log eviction periodically
        static int audioEvictLogCounter = 0;
        if (evicted > 0 && (++audioEvictLogCounter % 500) == 0) {
            double spanSec = 0;
            if (g_audioSampleCount > 0) {
                spanSec = (sample->timestamp - g_audioSamples[0].timestamp) / 10000000.0;
            }
            ReplayLog("Audio eviction: removed %d samples, count=%d, span=%.2fs\n",
                      evicted, g_audioSampleCount, spanSec);
        }
    }
    
    // Grow array if needed (capacity-based)
    if (g_audioSampleCount >= g_audioSampleCapacity) {
        int newCapacity = g_audioSampleCapacity == 0 ? 1024 : g_audioSampleCapacity * 2;
        if (newCapacity > MAX_AUDIO_SAMPLES) newCapacity = MAX_AUDIO_SAMPLES;
        
        if (g_audioSampleCount >= newCapacity) {
            // Still full after time eviction - emergency capacity eviction
            int toKeep = newCapacity * 3 / 4;
            int toRemove = g_audioSampleCount - toKeep;
            
            for (int i = 0; i < toRemove && i < g_audioSampleCount; i++) {
                if (g_audioSamples[i].data) {
                    free(g_audioSamples[i].data);
                }
            }
            
            memmove(g_audioSamples, g_audioSamples + toRemove, 
                    toKeep * sizeof(MuxerAudioSample));
            g_audioSampleCount = toKeep;
        } else {
            MuxerAudioSample* newArr = realloc(g_audioSamples, 
                                                newCapacity * sizeof(MuxerAudioSample));
            if (newArr) {
                g_audioSamples = newArr;
                g_audioSampleCapacity = newCapacity;
            } else {
                // realloc failed - log and drop sample
                static int reallocFailCount = 0;
                if (++reallocFailCount <= 5) {
                    ReplayLog("WARNING: Audio buffer realloc failed (count=%d, capacity=%d)\n", 
                              g_audioSampleCount, newCapacity);
                }
            }
        }
    }
    
    if (g_audioSampleCount < g_audioSampleCapacity) {
        MuxerAudioSample* dst = &g_audioSamples[g_audioSampleCount];
        dst->data = (BYTE*)malloc(sample->size);
        if (dst->data) {
            memcpy(dst->data, sample->data, sample->size);
            dst->size = sample->size;
            dst->timestamp = sample->timestamp;
            dst->duration = sample->duration;
            g_audioSampleCount++;
        }
    }
    
    LeaveCriticalSection(&g_audioLock);
}

// ============================================================================
// PUBLIC API
// ============================================================================

BOOL ReplayBuffer_Init(ReplayBufferState* state) {
    if (!state) return FALSE;
    ZeroMemory(state, sizeof(ReplayBufferState));
    
    // Create synchronization events
    state->hReadyEvent = CreateEvent(NULL, TRUE, FALSE, NULL);  // Manual reset
    state->hSaveRequestEvent = CreateEvent(NULL, FALSE, FALSE, NULL);  // Auto reset
    state->hSaveCompleteEvent = CreateEvent(NULL, FALSE, FALSE, NULL);  // Auto reset
    state->hStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);  // Manual reset
    
    if (!state->hReadyEvent || !state->hSaveRequestEvent || 
        !state->hSaveCompleteEvent || !state->hStopEvent) {
        ReplayLog("Failed to create synchronization events\n");
        if (state->hReadyEvent) CloseHandle(state->hReadyEvent);
        if (state->hSaveRequestEvent) CloseHandle(state->hSaveRequestEvent);
        if (state->hSaveCompleteEvent) CloseHandle(state->hSaveCompleteEvent);
        if (state->hStopEvent) CloseHandle(state->hStopEvent);
        return FALSE;
    }
    
    state->state = REPLAY_STATE_UNINITIALIZED;
    InitializeCriticalSection(&g_audioLock);
    g_audioLockInitialized = TRUE;
    return TRUE;
}

void ReplayBuffer_Shutdown(ReplayBufferState* state) {
    if (!state) return;
    ReplayBuffer_Stop(state);
    
    // Close event handles
    if (state->hReadyEvent) CloseHandle(state->hReadyEvent);
    if (state->hSaveRequestEvent) CloseHandle(state->hSaveRequestEvent);
    if (state->hSaveCompleteEvent) CloseHandle(state->hSaveCompleteEvent);
    if (state->hStopEvent) CloseHandle(state->hStopEvent);
    state->hReadyEvent = NULL;
    state->hSaveRequestEvent = NULL;
    state->hSaveCompleteEvent = NULL;
    state->hStopEvent = NULL;
    
    if (g_audioLockInitialized) {
        DeleteCriticalSection(&g_audioLock);
        g_audioLockInitialized = FALSE;
    }
    
    // Clean up audio samples
    if (g_audioSamples) {
        for (int i = 0; i < g_audioSampleCount; i++) {
            if (g_audioSamples[i].data) free(g_audioSamples[i].data);
        }
        free(g_audioSamples);
        g_audioSamples = NULL;
    }
    g_audioSampleCount = 0;
    g_audioSampleCapacity = 0;
    
    // Logger cleanup is handled by Logger_Shutdown in main.c
}

BOOL ReplayBuffer_Start(ReplayBufferState* state, const AppConfig* config) {
    if (!state || !config) return FALSE;
    if (state->isBuffering) return TRUE;
    
    state->enabled = config->replayEnabled;
    state->durationSeconds = config->replayDuration;
    state->captureSource = config->replayCaptureSource;
    state->monitorIndex = config->replayMonitorIndex;
    
    // Copy audio settings
    state->audioEnabled = config->audioEnabled;
    strncpy(state->audioSource1, config->audioSource1, sizeof(state->audioSource1) - 1);
    strncpy(state->audioSource2, config->audioSource2, sizeof(state->audioSource2) - 1);
    strncpy(state->audioSource3, config->audioSource3, sizeof(state->audioSource3) - 1);
    state->audioVolume1 = config->audioVolume1;
    state->audioVolume2 = config->audioVolume2;
    state->audioVolume3 = config->audioVolume3;
    
    if (!state->enabled) return FALSE;
    
    // Reset state machine
    InterlockedExchange(&state->state, REPLAY_STATE_STARTING);
    InterlockedExchange(&state->framesCaptured, 0);
    state->saveSuccess = FALSE;
    state->savePath[0] = '\0';
    
    // Reset events
    ResetEvent(state->hReadyEvent);
    ResetEvent(state->hSaveCompleteEvent);
    ResetEvent(state->hStopEvent);
    
    // Legacy flags
    state->bufferReady = FALSE;
    
    // Reset audio buffer
    EnterCriticalSection(&g_audioLock);
    if (g_audioSamples) {
        for (int i = 0; i < g_audioSampleCount; i++) {
            if (g_audioSamples[i].data) free(g_audioSamples[i].data);
        }
    }
    g_audioSampleCount = 0;
    LeaveCriticalSection(&g_audioLock);
    
    state->bufferThread = CreateThread(NULL, 0, BufferThreadProc, state, 0, NULL);
    state->isBuffering = (state->bufferThread != NULL);
    
    if (!state->isBuffering) {
        InterlockedExchange(&state->state, REPLAY_STATE_ERROR);
        return FALSE;
    }
    
    // Wait up to 5 seconds for the buffer thread to become ready
    DWORD waitResult = WaitForSingleObject(state->hReadyEvent, 5000);
    if (waitResult != WAIT_OBJECT_0) {
        ReplayLog("Timeout waiting for buffer thread to become ready\n");
        // Don't fail - thread is running, just not ready yet
    }
    
    return state->isBuffering;
}

void ReplayBuffer_Stop(ReplayBufferState* state) {
    if (!state || !state->isBuffering) return;
    
    // Signal stop via event (proper cross-thread communication)
    InterlockedExchange(&state->state, REPLAY_STATE_STOPPING);
    SetEvent(state->hStopEvent);
    
    if (state->bufferThread) {
        WaitForSingleObject(state->bufferThread, 10000);
        CloseHandle(state->bufferThread);
        state->bufferThread = NULL;
    }
    state->isBuffering = FALSE;
}

BOOL ReplayBuffer_Save(ReplayBufferState* state, const char* outputPath) {
    if (!state || !outputPath || !state->isBuffering) {
        ReplayLog("Save rejected: state=%p, path=%s, buffering=%d\n", 
                  state, outputPath ? outputPath : "NULL", state ? state->isBuffering : 0);
        return FALSE;
    }
    
    // Check state machine - must be in CAPTURING state
    LONG currentState = InterlockedCompareExchange(&state->state, REPLAY_STATE_CAPTURING, REPLAY_STATE_CAPTURING);
    if (currentState != REPLAY_STATE_CAPTURING) {
        ReplayLog("Save rejected: state=%d (expected CAPTURING=%d)\n", 
                  currentState, REPLAY_STATE_CAPTURING);
        return FALSE;
    }
    
    // Check minimum frames requirement
    LONG frames = InterlockedCompareExchange(&state->framesCaptured, 0, 0);
    if (frames < MIN_FRAMES_FOR_SAVE) {
        ReplayLog("Save rejected: only %d frames captured (need %d)\n", frames, MIN_FRAMES_FOR_SAVE);
        return FALSE;
    }
    
    // Set up save parameters
    strncpy(state->savePath, outputPath, MAX_PATH - 1);
    state->saveSuccess = FALSE;
    
    // Signal save request via event (proper synchronization)
    ResetEvent(state->hSaveCompleteEvent);
    SetEvent(state->hSaveRequestEvent);
    
    // Wait for completion (max 30 sec for muxing large buffers)
    DWORD waitResult = WaitForSingleObject(state->hSaveCompleteEvent, 30000);
    
    if (waitResult != WAIT_OBJECT_0) {
        ReplayLog("Save timeout after 30 seconds\n");
        return FALSE;
    }
    
    return state->saveSuccess;
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
    
    // Reset static globals at start of each run to prevent stale state
    g_encoder = NULL;
    ZeroMemory(&g_sampleBuffer, sizeof(g_sampleBuffer));
    g_seqHeaderSize = 0;
    g_audioCapture = NULL;
    g_aacEncoder = NULL;
    g_audioSamples = NULL;
    g_audioSampleCount = 0;
    g_audioSampleCapacity = 0;
    g_aacConfigData = NULL;
    g_aacConfigSize = 0;
    
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
    if (!Capture_SetRegion(capture, rect)) {
        ReplayLog("Capture_SetRegion failed - cannot capture region %d,%d,%d,%d\n",
                  rect.left, rect.top, rect.right, rect.bottom);
        return 1;
    }
    
    state->frameWidth = width;
    state->frameHeight = height;
    
    int fps = g_config.replayFPS;
    if (fps < 30) fps = 30;
    if (fps > 120) fps = 120;
    
    ReplayLog("Final capture params: %dx%d @ %d FPS, duration=%ds, quality=%d\n", 
              width, height, fps, g_config.replayDuration, g_config.quality);
    
    // Initialize GPU color converter (BGRA → NV12 on GPU)
    GPUConverter gpuConverter = {0};
    if (!GPUConverter_Init(&gpuConverter, capture->device, width, height)) {
        ReplayLog("GPUConverter_Init failed - GPU color conversion required!\n");
        return 1;
    }
    ReplayLog("GPU color converter initialized (D3D11 Video Processor)\n");
    
    // Initialize NVENC HEVC encoder with D3D11 device (native API)
    ReplayLog("Creating NVENCEncoder (%dx%d @ %d fps, quality=%d)...\n", width, height, fps, g_config.quality);
    g_encoder = NVENCEncoder_Create(capture->device, width, height, fps, g_config.quality);
    if (!g_encoder) {
        ReplayLog("NVENCEncoder_Create failed - NVIDIA GPU with NVENC required!\n");
        GPUConverter_Shutdown(&gpuConverter);
        return 1;
    }
    ReplayLog("NVENC HEVC hardware encoder initialized (native API)\n");
    
    // Extract HEVC sequence header (VPS/SPS/PPS) for MP4 muxing
    if (NVENCEncoder_GetSequenceHeader(g_encoder, g_seqHeader, sizeof(g_seqHeader), &g_seqHeaderSize)) {
        ReplayLog("HEVC sequence header extracted (%u bytes)\n", g_seqHeaderSize);
    } else {
        ReplayLog("WARNING: Failed to get HEVC sequence header - muxing may fail!\n");
        g_seqHeaderSize = 0;
    };
    
    // Initialize sample buffer BEFORE setting encoder callback
    // (callback needs valid buffer pointer)
    if (!SampleBuffer_Init(&g_sampleBuffer, g_config.replayDuration, fps, 
                           width, height, g_config.quality)) {
        ReplayLog("SampleBuffer_Init failed\n");
        NVENCEncoder_Destroy(g_encoder);
        g_encoder = NULL;
        GPUConverter_Shutdown(&gpuConverter);
        return 1;
    }
    
    // Set encoder callback to receive completed frames (async mode)
    // The output thread will call DrainCallback when frames complete
    NVENCEncoder_SetCallback(g_encoder, DrainCallback, &g_sampleBuffer);
    
    // Pass sequence header to sample buffer for video-only saves
    if (g_seqHeaderSize > 0) {
        SampleBuffer_SetSequenceHeader(&g_sampleBuffer, g_seqHeader, g_seqHeaderSize);
    }
    
    ReplayLog("Sample buffer initialized (max %ds)\n", g_config.replayDuration);
    
    // Initialize audio capture if enabled
    BOOL audioActive = FALSE;
    if (state->audioEnabled && (state->audioSource1[0] || state->audioSource2[0] || state->audioSource3[0])) {
        ReplayLog("Audio capture enabled, sources: [%s] [%s] [%s]\n",
                  state->audioSource1[0] ? state->audioSource1 : "none",
                  state->audioSource2[0] ? state->audioSource2 : "none",
                  state->audioSource3[0] ? state->audioSource3 : "none");
        
        g_audioCapture = AudioCapture_Create(
            state->audioSource1, state->audioVolume1,
            state->audioSource2, state->audioVolume2,
            state->audioSource3, state->audioVolume3
        );
        
        if (g_audioCapture) {
            g_aacEncoder = AACEncoder_Create();
            if (g_aacEncoder) {
                AACEncoder_SetCallback(g_aacEncoder, AudioEncoderCallback, NULL);
                
                // Set audio max duration to match video buffer (in 100-ns units)
                g_audioMaxDuration = (LONGLONG)g_config.replayDuration * 10000000LL;
                ReplayLog("Audio eviction enabled: max duration = %ds\n", g_config.replayDuration);
                
                // Get AAC config for muxer
                AACEncoder_GetConfig(g_aacEncoder, &g_aacConfigData, &g_aacConfigSize);
                
                if (AudioCapture_Start(g_audioCapture)) {
                    audioActive = TRUE;
                    ReplayLog("Audio capture started successfully\n");
                } else {
                    ReplayLog("AudioCapture_Start failed\n");
                }
            } else {
                ReplayLog("AACEncoder_Create failed\n");
            }
        } else {
            ReplayLog("AudioCapture_Create failed\n");
        }
    }
    
    // Timing - use floating point for precise frame intervals
    double frameIntervalMs = 1000.0 / (double)fps;  // 16.667ms for 60fps
    ReplayLog("Frame interval: %.4f ms (target fps=%d)\n", frameIntervalMs, fps);
    
    // Request high-resolution timer (1ms precision)
    timeBeginPeriod(1);
    
    LARGE_INTEGER perfFreq, lastFrameTime, captureStartTime;
    QueryPerformanceFrequency(&perfFreq);
    QueryPerformanceCounter(&lastFrameTime);
    captureStartTime = lastFrameTime;
    
    int frameCount = 0;
    int lastLogFrame = 0;
    
    // Diagnostic counters (reset each run)
    int attemptCount = 0;
    int captureNullCount = 0;
    int convertNullCount = 0;
    int encodeFailCount = 0;
    double totalCaptureMs = 0, totalConvertMs = 0, totalSubmitMs = 0;
    int timingCount = 0;
    
    // Transition to CAPTURING state and signal ready
    // (but don't signal hReadyEvent until we have frames)
    InterlockedExchange(&state->state, REPLAY_STATE_CAPTURING);
    state->bufferReady = TRUE;  // Legacy flag
    ReplayLog("Buffer thread ready, entering capture loop\n");
    
    // Build wait handle array for event-driven loop
    HANDLE waitHandles[2] = { state->hStopEvent, state->hSaveRequestEvent };
    
    while (InterlockedCompareExchange(&state->state, 0, 0) == REPLAY_STATE_CAPTURING) {
        // Check for stop/save events with 1ms timeout (allows frame timing)
        DWORD waitResult = WaitForMultipleObjects(2, waitHandles, FALSE, 1);
        
        if (waitResult == WAIT_OBJECT_0) {
            // Stop event signaled
            ReplayLog("Stop event received\n");
            break;
        }
        
        if (waitResult == WAIT_OBJECT_0 + 1) {
            // Save request event signaled
            double duration = SampleBuffer_GetDuration(&g_sampleBuffer);
            int count = SampleBuffer_GetCount(&g_sampleBuffer);
            
            // Calculate actual capture stats for diagnostics
            LARGE_INTEGER nowTime;
            QueryPerformanceCounter(&nowTime);
            double realElapsedSec = (double)(nowTime.QuadPart - captureStartTime.QuadPart) / perfFreq.QuadPart;
            double actualFPS = (realElapsedSec > 0) ? frameCount / realElapsedSec : 0;
            
            ReplayLog("SAVE REQUEST: %d video samples (%.2fs), %d audio samples, after %.2fs real time\n", 
                      count, duration, g_audioSampleCount, realElapsedSec);
            ReplayLog("  Actual capture rate: %.2f fps (target: %d fps)\n", actualFPS, fps);
            ReplayLog("  Output path: %s\n", state->savePath);
            
            // Write buffer to file (with audio if available)
            BOOL ok = FALSE;
            
            EnterCriticalSection(&g_audioLock);
            int audioCount = g_audioSampleCount;
            MuxerAudioSample* audioCopy = NULL;
            
            if (audioCount > 0 && g_audioSamples && g_aacConfigData && g_aacConfigSize > 0) {
                // Deep copy audio samples
                audioCopy = (MuxerAudioSample*)malloc(audioCount * sizeof(MuxerAudioSample));
                if (audioCopy) {
                    LONGLONG firstAudioTs = g_audioSamples[0].timestamp;
                    int copiedCount = 0;
                    BOOL copyFailed = FALSE;
                    
                    for (int i = 0; i < audioCount; i++) {
                        audioCopy[i].data = (BYTE*)malloc(g_audioSamples[i].size);
                        if (audioCopy[i].data) {
                            memcpy(audioCopy[i].data, g_audioSamples[i].data, g_audioSamples[i].size);
                            audioCopy[i].size = g_audioSamples[i].size;
                            audioCopy[i].timestamp = g_audioSamples[i].timestamp - firstAudioTs;
                            audioCopy[i].duration = g_audioSamples[i].duration;
                            copiedCount++;
                        } else {
                            // malloc failed - free all previous copies and abort
                            ReplayLog("WARNING: Audio copy malloc failed at sample %d/%d\n", i, audioCount);
                            for (int j = 0; j < i; j++) {
                                if (audioCopy[j].data) free(audioCopy[j].data);
                            }
                            free(audioCopy);
                            audioCopy = NULL;
                            copyFailed = TRUE;
                            break;
                        }
                    }
                    
                    // Update audioCount to reflect actual copied samples
                    if (!copyFailed) {
                        audioCount = copiedCount;
                    } else {
                        audioCount = 0;
                    }
                }
            }
            LeaveCriticalSection(&g_audioLock);
            
            // Get video samples
            MuxerSample* videoSamples = NULL;
            int videoCount = 0;
            if (SampleBuffer_GetSamplesForMuxing(&g_sampleBuffer, &videoSamples, &videoCount)) {
                // Build video config
                MuxerConfig videoConfig;
                videoConfig.width = g_sampleBuffer.width;
                videoConfig.height = g_sampleBuffer.height;
                videoConfig.fps = g_sampleBuffer.fps;
                videoConfig.quality = g_sampleBuffer.quality;
                videoConfig.seqHeader = g_sampleBuffer.seqHeaderSize > 0 ? g_sampleBuffer.seqHeader : NULL;
                videoConfig.seqHeaderSize = g_sampleBuffer.seqHeaderSize;
                
                if (audioCopy && audioCount > 0) {
                    // Mux with audio
                    ReplayLog("  Starting save (audio+video path, %d audio samples)...\n", audioCount);
                    MuxerAudioConfig audioConfig;
                    audioConfig.sampleRate = 48000;
                    audioConfig.channels = 2;
                    audioConfig.bitrate = 192000;
                    audioConfig.configData = g_aacConfigData;
                    audioConfig.configSize = g_aacConfigSize;
                    
                    ok = MP4Muxer_WriteFileWithAudio(state->savePath, 
                                                     videoSamples, videoCount, &videoConfig,
                                                     audioCopy, audioCount, &audioConfig);
                } else {
                    // Video only
                    ReplayLog("  Starting save (video-only path)...\n");
                    ok = MP4Muxer_WriteFile(state->savePath, videoSamples, videoCount, &videoConfig);
                }
                
                // Free video samples
                for (int i = 0; i < videoCount; i++) {
                    if (videoSamples[i].data) free(videoSamples[i].data);
                }
                free(videoSamples);
            }
            
            // Free audio copy
            if (audioCopy) {
                for (int i = 0; i < audioCount; i++) {
                    if (audioCopy[i].data) free(audioCopy[i].data);
                }
                free(audioCopy);
            }
            
            ReplayLog("SAVE %s\n", ok ? "OK" : "FAILED");
            
            state->saveSuccess = ok;
            SetEvent(state->hSaveCompleteEvent);
            continue;  // Skip frame capture this iteration
        }
        
        // === AUDIO CAPTURE ===
        if (audioActive && g_audioCapture && g_aacEncoder) {
            BYTE audioPcmBuf[8192];
            LONGLONG audioTs = 0;
            int audioBytes = AudioCapture_Read(g_audioCapture, audioPcmBuf, sizeof(audioPcmBuf), &audioTs);
            if (audioBytes > 0) {
                AACEncoder_Feed(g_aacEncoder, audioPcmBuf, audioBytes, audioTs);
            }
        }
        
        // === FRAME CAPTURE (GPU PATH) ===
        LARGE_INTEGER currentTime;
        QueryPerformanceCounter(&currentTime);
        double frameElapsedMs = (double)(currentTime.QuadPart - lastFrameTime.QuadPart) * 1000.0 / perfFreq.QuadPart;
        
        if (frameElapsedMs >= frameIntervalMs) {
            // Use ideal next frame time (prevents drift accumulation)
            lastFrameTime.QuadPart += (LONGLONG)(frameIntervalMs * perfFreq.QuadPart / 1000.0);
            
            // But if we're way behind, reset to now (prevents "catch up" burst)
            if ((currentTime.QuadPart - lastFrameTime.QuadPart) * 1000.0 / perfFreq.QuadPart > frameIntervalMs * 2) {
                lastFrameTime = currentTime;
            }
            
            attemptCount++;
            
            // Calculate real wall-clock timestamp for this frame (100-ns units)
            double realElapsedSec = (double)(currentTime.QuadPart - captureStartTime.QuadPart) / perfFreq.QuadPart;
            UINT64 realTimestamp = (UINT64)(realElapsedSec * 10000000.0);
            
            // GPU path: capture → color convert → NVENC (all on GPU)
            
            // Pipeline timing for diagnostics
            LARGE_INTEGER t1, t2, t3, t4;
            
            if (gpuConverter.initialized && g_encoder) {
                QueryPerformanceCounter(&t1);
                ID3D11Texture2D* bgraTexture = Capture_GetFrameTexture(capture, NULL);
                QueryPerformanceCounter(&t2);
                
                if (bgraTexture) {
                    ID3D11Texture2D* nv12Texture = GPUConverter_Convert(&gpuConverter, bgraTexture);
                    QueryPerformanceCounter(&t3);
                    
                    if (nv12Texture) {
                        // Async API: Submit frame (fast, non-blocking)
                        // Output thread will call DrainCallback when frame completes
                        BOOL submitted = NVENCEncoder_SubmitTexture(g_encoder, nv12Texture, realTimestamp);
                        QueryPerformanceCounter(&t4);
                        
                        if (submitted) {
                            frameCount++;  // Count submissions (frames delivered via callback)
                            
                            // Update state machine frame count
                            LONG newCount = InterlockedIncrement(&state->framesCaptured);
                            
                            // Signal ready event once we have enough frames
                            if (newCount == MIN_FRAMES_FOR_SAVE) {
                                SetEvent(state->hReadyEvent);
                                ReplayLog("Minimum frames captured (%d), ready for saves\n", MIN_FRAMES_FOR_SAVE);
                            }
                            
                            // Accumulate timing stats (submit should be <1ms in async mode)
                            totalCaptureMs += (double)(t2.QuadPart - t1.QuadPart) * 1000.0 / perfFreq.QuadPart;
                            totalConvertMs += (double)(t3.QuadPart - t2.QuadPart) * 1000.0 / perfFreq.QuadPart;
                            totalSubmitMs += (double)(t4.QuadPart - t3.QuadPart) * 1000.0 / perfFreq.QuadPart;
                            timingCount++;
                        } else {
                            encodeFailCount++;
                        }
                    } else {
                        convertNullCount++;
                    }
                } else {
                    captureNullCount++;
                }
            }
            
            // Early failure detection - if first 60 attempts all fail, log warning
            if (attemptCount == 60 && frameCount == 0) {
                ReplayLog("WARNING: First 60 capture attempts all failed! Check capture source.\n");
                ReplayLog("  capture=%d, convert=%d, encode=%d\n",
                          captureNullCount, convertNullCount, encodeFailCount);
            }
            
            // Log failures and timing periodically (every 10 seconds worth of attempts)
            if (attemptCount % (fps * 10) == 0 && attemptCount > 0) {
                if (timingCount > 0) {
                    ReplayLog("Pipeline timing (avg): capture=%.2fms, convert=%.2fms, submit=%.2fms, total=%.2fms\n",
                              totalCaptureMs / timingCount, totalConvertMs / timingCount, 
                              totalSubmitMs / timingCount,
                              (totalCaptureMs + totalConvertMs + totalSubmitMs) / timingCount);
                }
                if (captureNullCount + convertNullCount + encodeFailCount > 0) {
                    ReplayLog("Frame stats: attempts=%d, success=%d, failures: capture=%d, convert=%d, encode=%d\n",
                              attemptCount, frameCount, captureNullCount, convertNullCount, encodeFailCount);
                }
            }
            
            // Periodic log with actual FPS calculation
            if (frameCount - lastLogFrame >= fps * 5) {
                LARGE_INTEGER nowTime;
                QueryPerformanceCounter(&nowTime);
                double realElapsedSec = (double)(nowTime.QuadPart - captureStartTime.QuadPart) / perfFreq.QuadPart;
                double actualFPS = frameCount / realElapsedSec;
                double attemptFPS = attemptCount / realElapsedSec;
                
                // Get encoder stats
                int encFrames = 0;
                double avgEncMs = 0;
                NVENCEncoder_GetStats(g_encoder, &encFrames, &avgEncMs);
                
                double duration = SampleBuffer_GetDuration(&g_sampleBuffer);
                int bufCount = SampleBuffer_GetCount(&g_sampleBuffer);
                size_t memMB = SampleBuffer_GetMemoryUsage(&g_sampleBuffer) / (1024 * 1024);
                size_t memKB = SampleBuffer_GetMemoryUsage(&g_sampleBuffer) / 1024;
                int avgKBPerFrame = bufCount > 0 ? (int)(memKB / bufCount) : 0;
                ReplayLog("Status: %d/%d frames in %.1fs (encode=%.1f fps, attempt=%.1f fps, target=%d fps), buffer=%.1fs (%d samples, %zu MB, %d KB/frame)\n", 
                          frameCount, attemptCount, realElapsedSec, actualFPS, attemptFPS, fps, duration, bufCount, memMB, avgKBPerFrame);
                
                // Log failure breakdown if any
                if (captureNullCount + convertNullCount + encodeFailCount > 0) {
                    ReplayLog("  Failures: capture=%d, convert=%d, encode=%d\n",
                              captureNullCount, convertNullCount, encodeFailCount);
                }
                
                lastLogFrame = frameCount;
            }
        }
        // No Sleep() needed - WaitForMultipleObjects provides timing
    }
    
    // Cleanup
    ReplayLog("Shutting down (state=%d)...\n", InterlockedCompareExchange(&state->state, 0, 0));
    
    // Restore timer resolution
    timeEndPeriod(1);
    
    // Shutdown GPU converter
    GPUConverter_Shutdown(&gpuConverter);
    
    // Stop audio capture
    if (audioActive) {
        if (g_audioCapture) {
            AudioCapture_Stop(g_audioCapture);
            AudioCapture_Destroy(g_audioCapture);
            g_audioCapture = NULL;
        }
        if (g_aacEncoder) {
            AACEncoder_Destroy(g_aacEncoder);
            g_aacEncoder = NULL;
        }
        ReplayLog("Audio capture stopped\n");
    }
    
    // Flush encoder
    if (g_encoder) {
        EncodedFrame flushed = {0};
        while (NVENCEncoder_Flush(g_encoder, &flushed)) {
            SampleBuffer_Add(&g_sampleBuffer, &flushed);
        }
        
        NVENCEncoder_Destroy(g_encoder);
        g_encoder = NULL;
    }
    SampleBuffer_Shutdown(&g_sampleBuffer);
    
    ReplayLog("BufferThread exit\n");
    return 0;
}
