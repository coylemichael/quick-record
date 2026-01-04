/*
 * NVENC Hardware Encoder - Native NVIDIA API
 * HEVC encoding with async mode and separate output thread
 */

#ifndef NVENC_ENCODER_H
#define NVENC_ENCODER_H

#include <windows.h>
#include <d3d11.h>
#include "config.h"

typedef struct {
    BYTE* data;
    DWORD size;
    LONGLONG timestamp;
    LONGLONG duration;
    BOOL isKeyframe;
} EncodedFrame;

typedef struct NVENCEncoder NVENCEncoder;

// Callback for receiving completed frames (called from output thread)
typedef void (*EncodedFrameCallback)(EncodedFrame* frame, void* userData);

// Check if NVENC is available
BOOL NVENCEncoder_IsAvailable(void);

// Create encoder with D3D11 device
NVENCEncoder* NVENCEncoder_Create(ID3D11Device* d3dDevice, int width, int height, int fps, QualityPreset quality);

// Set callback for completed frames (async mode delivers via callback)
void NVENCEncoder_SetCallback(NVENCEncoder* enc, EncodedFrameCallback callback, void* userData);

// Submit texture for encoding (fast, non-blocking in async mode)
// The texture will be copied internally, so caller can reuse it immediately
BOOL NVENCEncoder_SubmitTexture(NVENCEncoder* enc, ID3D11Texture2D* nv12Texture, LONGLONG timestamp);

// Drain completed frames (for sync mode or manual draining)
int NVENCEncoder_DrainCompleted(NVENCEncoder* enc, EncodedFrameCallback callback, void* userData);

// Legacy API (wraps new API for compatibility)
BOOL NVENCEncoder_EncodeTexture(NVENCEncoder* enc, ID3D11Texture2D* nv12Texture, LONGLONG timestamp, EncodedFrame* outFrame);
BOOL NVENCEncoder_Flush(NVENCEncoder* enc, EncodedFrame* outFrame);

// Get sequence header (VPS/SPS/PPS for HEVC)
BOOL NVENCEncoder_GetSequenceHeader(NVENCEncoder* enc, BYTE* buffer, DWORD bufferSize, DWORD* outSize);

// Stats
void NVENCEncoder_GetStats(NVENCEncoder* enc, int* framesEncoded, double* avgEncodeTimeMs);

// Cleanup
void NVENCEncoder_Destroy(NVENCEncoder* enc);

#endif
