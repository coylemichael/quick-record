/*
 * Audio Capture Implementation
 * WASAPI-based audio capture with loopback support
 */

#define COBJMACROS
#define INITGUID
#include <initguid.h>

#include "audio_capture.h"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <stdio.h>
#include <math.h>

// GUIDs
DEFINE_GUID(IID_IAudioClient_Local, 0x1CB9AD4C, 0xDBFA, 0x4c32, 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2);
DEFINE_GUID(IID_IAudioCaptureClient_Local, 0xC8ADBD64, 0xE71E, 0x48a0, 0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17);
DEFINE_GUID(CLSID_MMDeviceEnumerator_AC, 0xBCDE0395, 0xE52F, 0x467C, 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E);
DEFINE_GUID(IID_IMMDeviceEnumerator_AC, 0xA95664D2, 0x9614, 0x4F35, 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6);

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
};

// Global enumerator
static IMMDeviceEnumerator* g_audioEnumerator = NULL;

// Buffer sizes
#define MIX_BUFFER_SIZE (AUDIO_BYTES_PER_SEC * 5)  // 5 seconds
#define SOURCE_BUFFER_SIZE (AUDIO_BYTES_PER_SEC * 2)  // 2 seconds per source

// Helper: Convert wide string to UTF-8
static void WideToUtf8_AC(const WCHAR* wide, char* utf8, int maxLen) {
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, maxLen, NULL, NULL);
}

// Helper: Convert UTF-8 to wide string
static void Utf8ToWide(const char* utf8, WCHAR* wide, int maxLen) {
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, maxLen);
}

BOOL AudioCapture_Init(void) {
    if (g_audioEnumerator) return TRUE;
    
    HRESULT hr = CoCreateInstance(
        &CLSID_MMDeviceEnumerator_AC,
        NULL,
        CLSCTX_ALL,
        &IID_IMMDeviceEnumerator_AC,
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
    Utf8ToWide(deviceId, wideId, 256);
    
    HRESULT hr = g_audioEnumerator->lpVtbl->GetDevice(g_audioEnumerator, wideId, &src->device);
    if (FAILED(hr)) {
        DeleteCriticalSection(&src->lock);
        free(src);
        return NULL;
    }
    
    // Activate audio client
    hr = src->device->lpVtbl->Activate(
        src->device,
        &IID_IAudioClient_Local,
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
    
    DWORD streamFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
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
        &IID_IAudioCaptureClient_Local,
        (void**)&src->captureClient
    );
    
    if (FAILED(hr)) {
        return FALSE;
    }
    
    return TRUE;
}

// Convert audio samples to target format
static int ConvertSamples(
    const BYTE* srcData, int srcSamples, const WAVEFORMATEX* srcFmt,
    BYTE* dstData, int dstMaxBytes, const WAVEFORMATEX* dstFmt
) {
    if (!srcData || !dstData || srcSamples == 0) return 0;
    
    // Simple conversion: handles common cases
    // For full resampling, we'd use a resampler library
    
    int dstSamples = 0;
    int dstBytes = 0;
    
    // Get source format details
    int srcChannels = srcFmt->nChannels;
    int srcBits = srcFmt->wBitsPerSample;
    BOOL srcFloat = (srcFmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ||
                    (srcFmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && srcBits == 32);
    
    // Get dest format details
    int dstChannels = dstFmt->nChannels;
    int dstBits = dstFmt->wBitsPerSample;
    
    // Same sample rate assumed (for now - would need resampler otherwise)
    dstSamples = srcSamples;
    if (dstSamples * dstFmt->nBlockAlign > dstMaxBytes) {
        dstSamples = dstMaxBytes / dstFmt->nBlockAlign;
    }
    
    for (int i = 0; i < dstSamples; i++) {
        // Read source sample(s) as float
        float left = 0, right = 0;
        
        if (srcFloat && srcBits == 32) {
            const float* srcFloats = (const float*)(srcData + i * srcFmt->nBlockAlign);
            left = srcFloats[0];
            if (srcChannels >= 2) right = srcFloats[1];
            else right = left;
        } else if (srcBits == 16) {
            const short* srcShorts = (const short*)(srcData + i * srcFmt->nBlockAlign);
            left = srcShorts[0] / 32768.0f;
            if (srcChannels >= 2) right = srcShorts[1] / 32768.0f;
            else right = left;
        } else if (srcBits == 24) {
            const BYTE* p = srcData + i * srcFmt->nBlockAlign;
            int s1 = (p[0] | (p[1] << 8) | (p[2] << 16));
            if (s1 & 0x800000) s1 |= 0xFF000000;  // Sign extend
            left = s1 / 8388608.0f;
            if (srcChannels >= 2) {
                int s2 = (p[3] | (p[4] << 8) | (p[5] << 16));
                if (s2 & 0x800000) s2 |= 0xFF000000;
                right = s2 / 8388608.0f;
            } else {
                right = left;
            }
        }
        
        // Write to destination as 16-bit
        short* dstShorts = (short*)(dstData + i * dstFmt->nBlockAlign);
        
        // Clamp
        if (left > 1.0f) left = 1.0f;
        if (left < -1.0f) left = -1.0f;
        if (right > 1.0f) right = 1.0f;
        if (right < -1.0f) right = -1.0f;
        
        dstShorts[0] = (short)(left * 32767.0f);
        if (dstChannels >= 2) {
            dstShorts[1] = (short)(right * 32767.0f);
        }
    }
    
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
                    // Silent - write zeros
                    convertedBytes = numFrames * src->targetFormat.nBlockAlign;
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
    const char* deviceId1,
    const char* deviceId2,
    const char* deviceId3
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
    
    // Create sources
    const char* deviceIds[] = {deviceId1, deviceId2, deviceId3};
    for (int i = 0; i < 3; i++) {
        if (deviceIds[i] && deviceIds[i][0] != '\0') {
            AudioCaptureSource* src = CreateSource(deviceIds[i]);
            if (src) {
                ctx->sources[ctx->sourceCount++] = src;
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
    
    // Temp buffers for reading from sources
    BYTE* srcBuffers[MAX_AUDIO_SOURCES] = {0};
    int srcBytes[MAX_AUDIO_SOURCES] = {0};
    
    for (int i = 0; i < ctx->sourceCount; i++) {
        srcBuffers[i] = (BYTE*)malloc(4096);
    }
    
    const int chunkSize = 4096;  // Process in chunks
    
    while (ctx->running) {
        // Read from each source
        int maxBytes = 0;
        
        for (int i = 0; i < ctx->sourceCount; i++) {
            AudioCaptureSource* src = ctx->sources[i];
            if (!src || !src->active) continue;
            
            EnterCriticalSection(&src->lock);
            
            int available = src->bufferAvailable;
            if (available > chunkSize) available = chunkSize;
            
            if (available > 0) {
                // Read from ring buffer
                int readPos = (src->bufferWritePos - src->bufferAvailable + src->bufferSize) % src->bufferSize;
                int toEnd = src->bufferSize - readPos;
                
                if (available <= toEnd) {
                    memcpy(srcBuffers[i], src->buffer + readPos, available);
                } else {
                    memcpy(srcBuffers[i], src->buffer + readPos, toEnd);
                    memcpy(srcBuffers[i] + toEnd, src->buffer, available - toEnd);
                }
                
                src->bufferAvailable -= available;
                srcBytes[i] = available;
                
                if (available > maxBytes) maxBytes = available;
            } else {
                srcBytes[i] = 0;
            }
            
            LeaveCriticalSection(&src->lock);
        }
        
        // Mix sources
        if (maxBytes > 0) {
            BYTE* mixChunk = (BYTE*)malloc(maxBytes);
            if (mixChunk) {
                int numSamples = maxBytes / AUDIO_BLOCK_ALIGN;
                
                for (int s = 0; s < numSamples; s++) {
                    int leftSum = 0, rightSum = 0;
                    int srcCount = 0;
                    
                    for (int i = 0; i < ctx->sourceCount; i++) {
                        if (srcBytes[i] > s * AUDIO_BLOCK_ALIGN) {
                            short* samples = (short*)(srcBuffers[i] + s * AUDIO_BLOCK_ALIGN);
                            leftSum += samples[0];
                            rightSum += samples[1];
                            srcCount++;
                        }
                    }
                    
                    // Average and clamp
                    if (srcCount > 0) {
                        leftSum /= srcCount;
                        rightSum /= srcCount;
                    }
                    
                    // Clamp to prevent clipping
                    if (leftSum > 32767) leftSum = 32767;
                    if (leftSum < -32768) leftSum = -32768;
                    if (rightSum > 32767) rightSum = 32767;
                    if (rightSum < -32768) rightSum = -32768;
                    
                    short* outSamples = (short*)(mixChunk + s * AUDIO_BLOCK_ALIGN);
                    outSamples[0] = (short)leftSum;
                    outSamples[1] = (short)rightSum;
                }
                
                // Write to mix buffer
                EnterCriticalSection(&ctx->mixLock);
                
                int spaceAvailable = ctx->mixBufferSize - ctx->mixBufferAvailable;
                if (maxBytes > spaceAvailable) {
                    // Drop oldest
                    int toDrop = maxBytes - spaceAvailable;
                    ctx->mixBufferAvailable -= toDrop;
                    ctx->mixBufferReadPos = (ctx->mixBufferReadPos + toDrop) % ctx->mixBufferSize;
                }
                
                int writePos = ctx->mixBufferWritePos;
                int toEnd = ctx->mixBufferSize - writePos;
                
                if (maxBytes <= toEnd) {
                    memcpy(ctx->mixBuffer + writePos, mixChunk, maxBytes);
                } else {
                    memcpy(ctx->mixBuffer + writePos, mixChunk, toEnd);
                    memcpy(ctx->mixBuffer, mixChunk + toEnd, maxBytes - toEnd);
                }
                
                ctx->mixBufferWritePos = (writePos + maxBytes) % ctx->mixBufferSize;
                ctx->mixBufferAvailable += maxBytes;
                
                LeaveCriticalSection(&ctx->mixLock);
                
                free(mixChunk);
            }
        } else {
            Sleep(5);
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
