/*
 * Audio Capture Implementation
 * WASAPI-based audio capture with loopback support
 */

#define COBJMACROS
#include <windows.h>

#include "audio_capture.h"
#include "audio_guids.h"
#include "util.h"
#include "logger.h"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <stdio.h>
#include <math.h>
#include <limits.h>

// Individual audio source capture
struct AudioCaptureSource {
    char deviceId[256];
    AudioDeviceType type;
    BOOL isLoopback;
    
    IMMDevice* device;
    IAudioClient* audioClient;
    IAudioCaptureClient* captureClient;
    
    WAVEFORMATEX* deviceFormat;
    WAVEFORMATEX targetFormat;
    
    // Per-source buffer
    BYTE* buffer;
    int bufferSize;
    int bufferWritePos;
    int bufferAvailable;
    CRITICAL_SECTION lock;
    
    HANDLE captureThread;  // Thread handle for proper cleanup
    BOOL active;
    
    // Timing for event-driven sources (virtual devices that don't send continuous packets)
    LARGE_INTEGER lastPacketTime;   // Last time we received a packet from this source
    LARGE_INTEGER perfFreq;         // Performance counter frequency
    BOOL hasReceivedPacket;         // TRUE once we've received at least one packet
};

// Global enumerator
static IMMDeviceEnumerator* g_audioEnumerator = NULL;

// Buffer sizes
#define MIX_BUFFER_SIZE (AUDIO_BYTES_PER_SEC * 5)  // 5 seconds
#define SOURCE_BUFFER_SIZE (AUDIO_BYTES_PER_SEC * 2)  // 2 seconds per source

BOOL AudioCapture_Init(void) {
    if (g_audioEnumerator) return TRUE;
    
    HRESULT hr = CoCreateInstance(
        &CLSID_MMDeviceEnumerator_Shared,
        NULL,
        CLSCTX_ALL,
        &IID_IMMDeviceEnumerator_Shared,
        (void**)&g_audioEnumerator
    );
    
    return SUCCEEDED(hr);
}

void AudioCapture_Shutdown(void) {
    if (g_audioEnumerator) {
        g_audioEnumerator->lpVtbl->Release(g_audioEnumerator);
        g_audioEnumerator = NULL;
    }
}

// Create a single capture source
static AudioCaptureSource* CreateSource(const char* deviceId) {
    if (!deviceId || deviceId[0] == '\0' || !g_audioEnumerator) {
        return NULL;
    }
    
    AudioCaptureSource* src = (AudioCaptureSource*)calloc(1, sizeof(AudioCaptureSource));
    if (!src) return NULL;
    
    strncpy(src->deviceId, deviceId, sizeof(src->deviceId) - 1);
    InitializeCriticalSection(&src->lock);
    
    // Get device info to determine if loopback
    AudioDeviceInfo info;
    if (AudioDevice_GetById(deviceId, &info)) {
        src->type = info.type;
        src->isLoopback = (info.type == AUDIO_DEVICE_OUTPUT);
    }
    
    // Get device by ID
    WCHAR wideId[256];
    Util_Utf8ToWide(deviceId, wideId, 256);
    
    HRESULT hr = g_audioEnumerator->lpVtbl->GetDevice(g_audioEnumerator, wideId, &src->device);
    if (FAILED(hr)) {
        DeleteCriticalSection(&src->lock);
        free(src);
        return NULL;
    }
    
    // Activate audio client
    hr = src->device->lpVtbl->Activate(
        src->device,
        &IID_IAudioClient_Shared,
        CLSCTX_ALL,
        NULL,
        (void**)&src->audioClient
    );
    
    if (FAILED(hr)) {
        src->device->lpVtbl->Release(src->device);
        DeleteCriticalSection(&src->lock);
        free(src);
        return NULL;
    }
    
    // Get device format
    hr = src->audioClient->lpVtbl->GetMixFormat(src->audioClient, &src->deviceFormat);
    if (FAILED(hr)) {
        src->audioClient->lpVtbl->Release(src->audioClient);
        src->device->lpVtbl->Release(src->device);
        DeleteCriticalSection(&src->lock);
        free(src);
        return NULL;
    }
    
    // Log device format for debugging
    Logger_Log("Audio device format: %d Hz, %d ch, %d bit, tag=%d (target: %d Hz)\n",
        src->deviceFormat->nSamplesPerSec,
        src->deviceFormat->nChannels,
        src->deviceFormat->wBitsPerSample,
        src->deviceFormat->wFormatTag,
        AUDIO_SAMPLE_RATE);
    
    // Set up target format (what we want)
    src->targetFormat.wFormatTag = WAVE_FORMAT_PCM;
    src->targetFormat.nChannels = AUDIO_CHANNELS;
    src->targetFormat.nSamplesPerSec = AUDIO_SAMPLE_RATE;
    src->targetFormat.wBitsPerSample = AUDIO_BITS_PER_SAMPLE;
    src->targetFormat.nBlockAlign = AUDIO_BLOCK_ALIGN;
    src->targetFormat.nAvgBytesPerSec = AUDIO_BYTES_PER_SEC;
    src->targetFormat.cbSize = 0;
    
    // Allocate buffer
    src->bufferSize = SOURCE_BUFFER_SIZE;
    src->buffer = (BYTE*)malloc(src->bufferSize);
    if (!src->buffer) {
        CoTaskMemFree(src->deviceFormat);
        src->audioClient->lpVtbl->Release(src->audioClient);
        src->device->lpVtbl->Release(src->device);
        DeleteCriticalSection(&src->lock);
        free(src);
        return NULL;
    }
    
    return src;
}

// Destroy a capture source
static void DestroySource(AudioCaptureSource* src) {
    if (!src) return;
    
    if (src->captureClient) {
        src->captureClient->lpVtbl->Release(src->captureClient);
    }
    if (src->deviceFormat) {
        CoTaskMemFree(src->deviceFormat);
    }
    if (src->audioClient) {
        src->audioClient->lpVtbl->Release(src->audioClient);
    }
    if (src->device) {
        src->device->lpVtbl->Release(src->device);
    }
    if (src->buffer) {
        free(src->buffer);
    }
    
    DeleteCriticalSection(&src->lock);
    free(src);
}

// Initialize a source for capture
static BOOL InitSourceCapture(AudioCaptureSource* src) {
    if (!src || !src->audioClient) return FALSE;
    
    // Buffer duration in 100ns units (100ms)
    REFERENCE_TIME bufferDuration = 1000000;  // 100ms
    
    // NOTE: Don't use AUDCLNT_STREAMFLAGS_EVENTCALLBACK - we poll instead
    // Using event callback requires SetEventHandle which adds complexity
    DWORD streamFlags = 0;
    if (src->isLoopback) {
        streamFlags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
    }
    
    // Initialize audio client
    // Use device's native format - we'll convert later
    HRESULT hr = src->audioClient->lpVtbl->Initialize(
        src->audioClient,
        AUDCLNT_SHAREMODE_SHARED,
        streamFlags,
        bufferDuration,
        0,
        src->deviceFormat,
        NULL
    );
    
    if (FAILED(hr)) {
        return FALSE;
    }
    
    // Get capture client
    hr = src->audioClient->lpVtbl->GetService(
        src->audioClient,
        &IID_IAudioCaptureClient_Shared,
        (void**)&src->captureClient
    );
    
    if (FAILED(hr)) {
        return FALSE;
    }
    
    return TRUE;
}

// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT for WAVEFORMATEXTENSIBLE
static const GUID KSDATAFORMAT_SUBTYPE_IEEE_FLOAT_Local = 
    {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

// Convert audio samples to target format WITH RESAMPLING
// Uses linear interpolation for sample rate conversion
static int ConvertSamples(
    const BYTE* srcData, int srcSamples, const WAVEFORMATEX* srcFmt,
    BYTE* dstData, int dstMaxBytes, const WAVEFORMATEX* dstFmt
) {
    if (!srcData || !dstData || srcSamples == 0) return 0;
    
    // Get source format details
    int srcChannels = srcFmt->nChannels;
    int srcBits = srcFmt->wBitsPerSample;
    int srcRate = srcFmt->nSamplesPerSec;
    
    // Detect float format - check both plain tag and extensible SubFormat
    BOOL srcFloat = (srcFmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
    if (srcFmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && srcFmt->cbSize >= 22) {
        // WAVEFORMATEXTENSIBLE - check SubFormat GUID
        typedef struct {
            WAVEFORMATEX Format;
            union {
                WORD wValidBitsPerSample;
                WORD wSamplesPerBlock;
                WORD wReserved;
            } Samples;
            DWORD dwChannelMask;
            GUID SubFormat;
        } WAVEFORMATEXTENSIBLE_LOCAL;
        
        const WAVEFORMATEXTENSIBLE_LOCAL* extFmt = (const WAVEFORMATEXTENSIBLE_LOCAL*)srcFmt;
        if (memcmp(&extFmt->SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT_Local, sizeof(GUID)) == 0) {
            srcFloat = TRUE;
        }
    }
    
    // Get dest format details
    int dstChannels = dstFmt->nChannels;
    int dstRate = dstFmt->nSamplesPerSec;
    
    // Calculate output sample count based on sample rate ratio
    int dstSamples = (int)((double)srcSamples * dstRate / srcRate);
    if (dstSamples * dstFmt->nBlockAlign > dstMaxBytes) {
        dstSamples = dstMaxBytes / dstFmt->nBlockAlign;
    }
    if (dstSamples <= 0) return 0;
    
    // Helper function to read a source sample as float stereo
    #define READ_SRC_SAMPLE(idx, pLeft, pRight) do { \
        int _i = (idx); \
        if (_i >= srcSamples) _i = srcSamples - 1; \
        if (_i < 0) _i = 0; \
        float _l = 0, _r = 0; \
        if (srcFloat && srcBits == 32) { \
            const float* sf = (const float*)(srcData + _i * srcFmt->nBlockAlign); \
            _l = sf[0]; \
            _r = (srcChannels >= 2) ? sf[1] : _l; \
        } else if (srcBits == 16) { \
            const short* ss = (const short*)(srcData + _i * srcFmt->nBlockAlign); \
            _l = ss[0] / 32768.0f; \
            _r = (srcChannels >= 2) ? ss[1] / 32768.0f : _l; \
        } else if (srcBits == 24) { \
            const BYTE* p = srcData + _i * srcFmt->nBlockAlign; \
            int s1 = (p[0] | (p[1] << 8) | (p[2] << 16)); \
            if (s1 & 0x800000) s1 |= 0xFF000000; \
            _l = s1 / 8388608.0f; \
            if (srcChannels >= 2) { \
                int s2 = (p[3] | (p[4] << 8) | (p[5] << 16)); \
                if (s2 & 0x800000) s2 |= 0xFF000000; \
                _r = s2 / 8388608.0f; \
            } else { _r = _l; } \
        } \
        *(pLeft) = _l; *(pRight) = _r; \
    } while(0)
    
    // Resample using linear interpolation
    double srcPos = 0.0;
    double srcStep = (double)srcRate / dstRate;
    
    for (int i = 0; i < dstSamples; i++) {
        int srcIdx = (int)srcPos;
        double frac = srcPos - srcIdx;
        
        // Read two adjacent source samples
        float left0, right0, left1, right1;
        READ_SRC_SAMPLE(srcIdx, &left0, &right0);
        READ_SRC_SAMPLE(srcIdx + 1, &left1, &right1);
        
        // Linear interpolation
        float left = left0 + (float)(frac * (left1 - left0));
        float right = right0 + (float)(frac * (right1 - right0));
        
        // Clamp
        if (left > 1.0f) left = 1.0f;
        if (left < -1.0f) left = -1.0f;
        if (right > 1.0f) right = 1.0f;
        if (right < -1.0f) right = -1.0f;
        
        // Write to destination as 16-bit
        short* dstShorts = (short*)(dstData + i * dstFmt->nBlockAlign);
        dstShorts[0] = (short)(left * 32767.0f);
        if (dstChannels >= 2) {
            dstShorts[1] = (short)(right * 32767.0f);
        }
        
        srcPos += srcStep;
    }
    
    #undef READ_SRC_SAMPLE
    
    return dstSamples * dstFmt->nBlockAlign;
}

// Capture thread for a single source
static DWORD WINAPI SourceCaptureThread(LPVOID param) {
    AudioCaptureSource* src = (AudioCaptureSource*)param;
    if (!src) return 0;
    
    // Temporary buffer for format conversion
    BYTE* convBuffer = (BYTE*)malloc(SOURCE_BUFFER_SIZE);
    if (!convBuffer) return 0;
    
    while (src->active) {
        UINT32 packetLength = 0;
        HRESULT hr = src->captureClient->lpVtbl->GetNextPacketSize(
            src->captureClient, &packetLength
        );
        
        if (FAILED(hr)) {
            Sleep(5);
            continue;
        }
        
        while (packetLength > 0 && src->active) {
            BYTE* data = NULL;
            UINT32 numFrames = 0;
            DWORD flags = 0;
            
            hr = src->captureClient->lpVtbl->GetBuffer(
                src->captureClient,
                &data,
                &numFrames,
                &flags,
                NULL,
                NULL
            );
            
            if (FAILED(hr)) break;
            
            if (numFrames > 0 && data) {
                int convertedBytes = 0;
                
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    // Silent - write zeros with proper resampling
                    // numFrames is in source sample rate, convert to target rate
                    int srcRate = src->deviceFormat->nSamplesPerSec;
                    int dstRate = src->targetFormat.nSamplesPerSec;
                    int dstFrames = (int)((double)numFrames * dstRate / srcRate);
                    convertedBytes = dstFrames * src->targetFormat.nBlockAlign;
                    if (convertedBytes > SOURCE_BUFFER_SIZE) {
                        convertedBytes = SOURCE_BUFFER_SIZE;
                    }
                    memset(convBuffer, 0, convertedBytes);
                } else {
                    // Convert to target format
                    convertedBytes = ConvertSamples(
                        data, numFrames, src->deviceFormat,
                        convBuffer, SOURCE_BUFFER_SIZE, &src->targetFormat
                    );
                }
                
                // Write to source ring buffer
                if (convertedBytes > 0) {
                    EnterCriticalSection(&src->lock);
                    
                    int spaceAvailable = src->bufferSize - src->bufferAvailable;
                    if (convertedBytes > spaceAvailable) {
                        // Buffer full - drop oldest data
                        int toDrop = convertedBytes - spaceAvailable;
                        src->bufferAvailable -= toDrop;
                    }
                    
                    // Write in possibly two parts (ring buffer wrap)
                    int writePos = src->bufferWritePos;
                    int toEnd = src->bufferSize - writePos;
                    
                    if (convertedBytes <= toEnd) {
                        memcpy(src->buffer + writePos, convBuffer, convertedBytes);
                    } else {
                        memcpy(src->buffer + writePos, convBuffer, toEnd);
                        memcpy(src->buffer, convBuffer + toEnd, convertedBytes - toEnd);
                    }
                    
                    src->bufferWritePos = (writePos + convertedBytes) % src->bufferSize;
                    src->bufferAvailable += convertedBytes;
                    
                    // Record that we received a packet (for event-driven source detection)
                    QueryPerformanceCounter(&src->lastPacketTime);
                    src->hasReceivedPacket = TRUE;
                    
                    LeaveCriticalSection(&src->lock);
                }
            }
            
            src->captureClient->lpVtbl->ReleaseBuffer(src->captureClient, numFrames);
            
            hr = src->captureClient->lpVtbl->GetNextPacketSize(
                src->captureClient, &packetLength
            );
            if (FAILED(hr)) break;
        }
        
        Sleep(5);  // ~5ms between checks
    }
    
    free(convBuffer);
    return 0;
}

AudioCaptureContext* AudioCapture_Create(
    const char* deviceId1, int volume1,
    const char* deviceId2, int volume2,
    const char* deviceId3, int volume3
) {
    if (!AudioCapture_Init()) {
        return NULL;
    }
    
    AudioCaptureContext* ctx = (AudioCaptureContext*)calloc(1, sizeof(AudioCaptureContext));
    if (!ctx) return NULL;
    
    InitializeCriticalSection(&ctx->mixLock);
    QueryPerformanceFrequency(&ctx->perfFreq);
    
    // Allocate mix buffer
    ctx->mixBufferSize = MIX_BUFFER_SIZE;
    ctx->mixBuffer = (BYTE*)malloc(ctx->mixBufferSize);
    if (!ctx->mixBuffer) {
        DeleteCriticalSection(&ctx->mixLock);
        free(ctx);
        return NULL;
    }
    
    // Create sources and assign volumes to match source index
    // This ensures volumes[i] corresponds to sources[i]
    const char* deviceIds[] = {deviceId1, deviceId2, deviceId3};
    int volumes[] = {volume1, volume2, volume3};
    
    for (int i = 0; i < 3; i++) {
        if (deviceIds[i] && deviceIds[i][0] != '\0') {
            AudioCaptureSource* src = CreateSource(deviceIds[i]);
            if (src) {
                // Assign volume to match source index, not device slot index
                int srcIdx = ctx->sourceCount;
                ctx->volumes[srcIdx] = (volumes[i] < 0) ? 0 : (volumes[i] > 100) ? 100 : volumes[i];
                ctx->sources[srcIdx] = src;
                ctx->sourceCount++;
                Logger_Log("Audio source %d: device slot %d, volume=%d%%\n", srcIdx, i, ctx->volumes[srcIdx]);
            }
        }
    }
    
    return ctx;
}

void AudioCapture_Destroy(AudioCaptureContext* ctx) {
    if (!ctx) return;
    
    AudioCapture_Stop(ctx);
    
    for (int i = 0; i < ctx->sourceCount; i++) {
        DestroySource(ctx->sources[i]);
    }
    
    if (ctx->mixBuffer) {
        free(ctx->mixBuffer);
    }
    
    DeleteCriticalSection(&ctx->mixLock);
    free(ctx);
}

// Mix capture thread - reads from all sources and mixes
static DWORD WINAPI MixCaptureThread(LPVOID param) {
    AudioCaptureContext* ctx = (AudioCaptureContext*)param;
    if (!ctx) return 0;
    
    // Peak tracking for logging - reset each session
    int peakLeft = 0, peakRight = 0;
    int logCounter = 0;
    
    // Temp buffers for reading from sources
    BYTE* srcBuffers[MAX_AUDIO_SOURCES] = {0};
    int srcBytes[MAX_AUDIO_SOURCES] = {0};
    BOOL srcDormant[MAX_AUDIO_SOURCES] = {0};  // TRUE if source is event-driven and currently silent
    
    for (int i = 0; i < ctx->sourceCount; i++) {
        srcBuffers[i] = (BYTE*)malloc(4096);
    }
    
    const int chunkSize = 4096;  // Process in chunks
    const double dormantThresholdMs = 100.0;  // Consider source dormant after 100ms of no packets
    
    // Rate limiting: track how much audio we should output based on elapsed time
    LARGE_INTEGER rateStartTime;
    QueryPerformanceCounter(&rateStartTime);
    LONGLONG totalBytesOutput = 0;  // Total bytes we've written to mix buffer
    
    while (ctx->running) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        
        // Check how much data is available from each source
        int maxBytes = 0;
        int activeSources = 0;
        int nonDormantSources = 0;
        int availableBytes[MAX_AUDIO_SOURCES] = {0};
        
        for (int i = 0; i < ctx->sourceCount; i++) {
            AudioCaptureSource* src = ctx->sources[i];
            if (!src || !src->active) continue;
            
            activeSources++;
            EnterCriticalSection(&src->lock);
            availableBytes[i] = src->bufferAvailable;
            
            // Check if this source is dormant (event-driven virtual device with no recent packets)
            // A source is dormant if:
            // 1. It has received at least one packet (so we know it works)
            // 2. It hasn't received a packet recently (> dormantThresholdMs)
            // 3. Its buffer is empty
            srcDormant[i] = FALSE;
            if (src->hasReceivedPacket && availableBytes[i] == 0) {
                double msSincePacket = (double)(now.QuadPart - src->lastPacketTime.QuadPart) * 1000.0 / src->perfFreq.QuadPart;
                if (msSincePacket > dormantThresholdMs) {
                    srcDormant[i] = TRUE;
                }
            }
            
            LeaveCriticalSection(&src->lock);
            
            if (!srcDormant[i]) {
                nonDormantSources++;
                if (availableBytes[i] > maxBytes) {
                    maxBytes = availableBytes[i];
                }
            }
        }
        
        // Need at least one non-dormant source with data to proceed
        if (nonDormantSources == 0 || maxBytes == 0) {
            Sleep(2);  // Wait for data
            continue;
        }
        
        // Rate limiting: calculate how many bytes we SHOULD have output by now
        // This prevents sources from "running ahead" of wall-clock time
        double elapsedSec = (double)(now.QuadPart - rateStartTime.QuadPart) / ctx->perfFreq.QuadPart;
        LONGLONG expectedBytes = (LONGLONG)(elapsedSec * AUDIO_BYTES_PER_SEC);
        LONGLONG bytesAllowed = expectedBytes - totalBytesOutput;
        
        // If we're ahead of schedule, wait
        if (bytesAllowed < chunkSize / 2) {
            Sleep(2);
            continue;
        }
        
        // Process up to chunkSize bytes from non-dormant sources
        int processBytes = maxBytes;
        if (processBytes > chunkSize) processBytes = chunkSize;
        // Also limit by rate
        if (processBytes > bytesAllowed) processBytes = (int)bytesAllowed;
        // Align to sample boundaries (4 bytes per sample for stereo 16-bit)
        processBytes = (processBytes / AUDIO_BLOCK_ALIGN) * AUDIO_BLOCK_ALIGN;
        if (processBytes <= 0) {
            Sleep(1);
            continue;
        }
        
        // Read from each source - dormant sources contribute silence
        for (int i = 0; i < ctx->sourceCount; i++) {
            AudioCaptureSource* src = ctx->sources[i];
            if (!src || !src->active || srcDormant[i]) {
                // Dormant sources contribute silence (srcBytes[i] = 0 means silence in the mixer)
                srcBytes[i] = 0;
                continue;
            }
            
            EnterCriticalSection(&src->lock);
            
            // Read whatever is available up to processBytes
            int toRead = src->bufferAvailable;
            if (toRead > processBytes) toRead = processBytes;
            
            if (toRead > 0) {
                int readPos = (src->bufferWritePos - src->bufferAvailable + src->bufferSize) % src->bufferSize;
                int toEnd = src->bufferSize - readPos;
                
                if (toRead <= toEnd) {
                    memcpy(srcBuffers[i], src->buffer + readPos, toRead);
                } else {
                    memcpy(srcBuffers[i], src->buffer + readPos, toEnd);
                    memcpy(srcBuffers[i] + toEnd, src->buffer, toRead - toEnd);
                }
                
                src->bufferAvailable -= toRead;
                srcBytes[i] = toRead;
            } else {
                // Source has no data - will contribute silence
                srcBytes[i] = 0;
            }
            
            LeaveCriticalSection(&src->lock);
        }
        
        // Find how many bytes to actually process (max of what any source provided)
        int bytesToMix = 0;
        for (int i = 0; i < ctx->sourceCount; i++) {
            if (srcBytes[i] > bytesToMix) bytesToMix = srcBytes[i];
        }
        
        // Mix sources - each source contributes what it has, silence for the rest
        if (bytesToMix > 0) {
            BYTE* mixChunk = (BYTE*)malloc(bytesToMix);
            if (mixChunk) {
                int numSamples = bytesToMix / AUDIO_BLOCK_ALIGN;
                
                for (int s = 0; s < numSamples; s++) {
                    int leftSum = 0, rightSum = 0;
                    int srcCount = 0;
                    
                    for (int i = 0; i < ctx->sourceCount; i++) {
                        // Check if this source has data for this sample
                        if (srcBytes[i] > s * AUDIO_BLOCK_ALIGN) {
                            short* samples = (short*)(srcBuffers[i] + s * AUDIO_BLOCK_ALIGN);
                            // Apply per-source volume (0-100)
                            int vol = ctx->volumes[i];
                            leftSum += (samples[0] * vol) / 100;
                            rightSum += (samples[1] * vol) / 100;
                            srcCount++;
                        }
                        // Sources without data for this sample contribute silence (0) implicitly
                    }
                    
                    // Don't divide by srcCount - just sum the sources that have data
                    // This prevents volume drops when one source is silent
                    
                    // Track peaks
                    int absL = leftSum < 0 ? -leftSum : leftSum;
                    int absR = rightSum < 0 ? -rightSum : rightSum;
                    if (absL > peakLeft) peakLeft = absL;
                    if (absR > peakRight) peakRight = absR;
                    
                    // Clamp to prevent clipping
                    if (leftSum > 32767) leftSum = 32767;
                    if (leftSum < -32768) leftSum = -32768;
                    if (rightSum > 32767) rightSum = 32767;
                    if (rightSum < -32768) rightSum = -32768;
                    
                    short* outSamples = (short*)(mixChunk + s * AUDIO_BLOCK_ALIGN);
                    outSamples[0] = (short)leftSum;
                    outSamples[1] = (short)rightSum;
                }
                
                // Log peak levels periodically
                logCounter++;
                if (logCounter % 500 == 0) {
                    // Convert to dB-ish scale (peak/32767 as percentage)
                    float peakPctL = (float)peakLeft / 32767.0f * 100.0f;
                    float peakPctR = (float)peakRight / 32767.0f * 100.0f;
                    double rateElapsed = (double)(now.QuadPart - rateStartTime.QuadPart) / ctx->perfFreq.QuadPart;
                    double actualRate = (rateElapsed > 0) ? (totalBytesOutput / rateElapsed) : 0;
                    Logger_Log("Audio: L=%.1f%% R=%.1f%% bytes=[%d,%d,%d] dormant=[%d,%d,%d] rate=%.0f/s (target=%d)\n", 
                               peakPctL, peakPctR,
                               srcBytes[0], srcBytes[1], srcBytes[2],
                               srcDormant[0], srcDormant[1], srcDormant[2],
                               actualRate, AUDIO_BYTES_PER_SEC);
                    peakLeft = 0;
                    peakRight = 0;
                }
                
                // Write to mix buffer
                EnterCriticalSection(&ctx->mixLock);
                
                int spaceAvailable = ctx->mixBufferSize - ctx->mixBufferAvailable;
                if (bytesToMix > spaceAvailable) {
                    // Drop oldest
                    int toDrop = bytesToMix - spaceAvailable;
                    ctx->mixBufferAvailable -= toDrop;
                    ctx->mixBufferReadPos = (ctx->mixBufferReadPos + toDrop) % ctx->mixBufferSize;
                }
                
                int writePos = ctx->mixBufferWritePos;
                int toEnd = ctx->mixBufferSize - writePos;
                
                if (bytesToMix <= toEnd) {
                    memcpy(ctx->mixBuffer + writePos, mixChunk, bytesToMix);
                } else {
                    memcpy(ctx->mixBuffer + writePos, mixChunk, toEnd);
                    memcpy(ctx->mixBuffer, mixChunk + toEnd, bytesToMix - toEnd);
                }
                
                ctx->mixBufferWritePos = (writePos + bytesToMix) % ctx->mixBufferSize;
                ctx->mixBufferAvailable += bytesToMix;
                
                LeaveCriticalSection(&ctx->mixLock);
                
                // Track total output for rate limiting
                totalBytesOutput += bytesToMix;
                
                free(mixChunk);
            }
        }
    }
    
    for (int i = 0; i < ctx->sourceCount; i++) {
        if (srcBuffers[i]) free(srcBuffers[i]);
    }
    
    return 0;
}

BOOL AudioCapture_Start(AudioCaptureContext* ctx) {
    if (!ctx || ctx->running) return FALSE;
    
    // Initialize and start each source
    for (int i = 0; i < ctx->sourceCount; i++) {
        AudioCaptureSource* src = ctx->sources[i];
        if (!src) continue;
        
        if (!InitSourceCapture(src)) {
            continue;
        }
        
        src->active = TRUE;
        
        // Initialize timing for event-driven source detection
        QueryPerformanceFrequency(&src->perfFreq);
        QueryPerformanceCounter(&src->lastPacketTime);
        src->hasReceivedPacket = FALSE;
        
        // Start audio client
        HRESULT hr = src->audioClient->lpVtbl->Start(src->audioClient);
        if (FAILED(hr)) {
            src->active = FALSE;
            continue;
        }
        
        // Start capture thread for this source
        src->captureThread = CreateThread(NULL, 0, SourceCaptureThread, src, 0, NULL);
    }
    
    // Record start time
    QueryPerformanceCounter(&ctx->startTime);
    
    // Start mix thread
    ctx->running = TRUE;
    ctx->captureThread = CreateThread(NULL, 0, MixCaptureThread, ctx, 0, NULL);
    
    return TRUE;
}

void AudioCapture_Stop(AudioCaptureContext* ctx) {
    if (!ctx) return;
    
    ctx->running = FALSE;
    
    // Stop sources and wait for their threads
    for (int i = 0; i < ctx->sourceCount; i++) {
        AudioCaptureSource* src = ctx->sources[i];
        if (!src) continue;
        
        src->active = FALSE;
        
        if (src->audioClient) {
            src->audioClient->lpVtbl->Stop(src->audioClient);
        }
        
        // Wait for source capture thread to finish
        if (src->captureThread) {
            WaitForSingleObject(src->captureThread, 1000);
            CloseHandle(src->captureThread);
            src->captureThread = NULL;
        }
    }
    
    // Wait for mix thread
    if (ctx->captureThread) {
        WaitForSingleObject(ctx->captureThread, 1000);
        CloseHandle(ctx->captureThread);
        ctx->captureThread = NULL;
    }
}

int AudioCapture_Read(AudioCaptureContext* ctx, BYTE* buffer, int maxBytes, LONGLONG* timestamp) {
    if (!ctx || !buffer) return 0;
    
    EnterCriticalSection(&ctx->mixLock);
    
    int available = ctx->mixBufferAvailable;
    if (available > maxBytes) available = maxBytes;
    
    if (available > 0) {
        int readPos = ctx->mixBufferReadPos;
        int toEnd = ctx->mixBufferSize - readPos;
        
        if (available <= toEnd) {
            memcpy(buffer, ctx->mixBuffer + readPos, available);
        } else {
            memcpy(buffer, ctx->mixBuffer + readPos, toEnd);
            memcpy(buffer + toEnd, ctx->mixBuffer, available - toEnd);
        }
        
        ctx->mixBufferReadPos = (readPos + available) % ctx->mixBufferSize;
        ctx->mixBufferAvailable -= available;
    }
    
    LeaveCriticalSection(&ctx->mixLock);
    
    // Calculate timestamp
    if (timestamp) {
        *timestamp = AudioCapture_GetTimestamp(ctx);
    }
    
    return available;
}

LONGLONG AudioCapture_GetTimestamp(AudioCaptureContext* ctx) {
    if (!ctx) return 0;
    
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    
    // Return time since start in 100ns units
    LONGLONG elapsed = now.QuadPart - ctx->startTime.QuadPart;
    return (elapsed * 10000000) / ctx->perfFreq.QuadPart;
}

BOOL AudioCapture_HasData(AudioCaptureContext* ctx) {
    if (!ctx) return FALSE;
    
    EnterCriticalSection(&ctx->mixLock);
    BOOL hasData = ctx->mixBufferAvailable > 0;
    LeaveCriticalSection(&ctx->mixLock);
    
    return hasData;
}
