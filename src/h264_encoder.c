/*
 * H.264 Memory Encoder Implementation
 * Uses IMFTransform for hardware-accelerated encoding to memory
 */

#include "h264_encoder.h"
#include "util.h"
#include "logger.h"
#include "color_convert.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <codecapi.h>
#include <strmif.h>   // For ICodecAPI
#include <stdio.h>

// Define IID_ICodecAPI if not defined
#ifndef IID_ICodecAPI
DEFINE_GUID(IID_ICodecAPI, 0x901db4c7, 0x31ce, 0x41a2, 0x85, 0xdc, 0x8f, 0xa0, 0xbf, 0x41, 0xb8, 0xda);
#endif

// GUIDs we need
static const GUID MFT_CATEGORY_VIDEO_ENCODER_GUID = 
    {0xf79eac7d, 0xe545, 0x4387, {0xbd, 0xee, 0xd6, 0x47, 0xd7, 0xbd, 0xe4, 0x2a}};

// Alias for logging
#define H264Log Logger_Log

// Find H.264 encoder - prefer software for simplicity (no D3D11 required)
static IMFTransform* FindH264Encoder(BOOL* isHardware) {
    HRESULT hr;
    IMFActivate** activates = NULL;
    UINT32 count = 0;
    IMFTransform* encoder = NULL;
    
    *isHardware = FALSE;
    
    // Set up search criteria
    MFT_REGISTER_TYPE_INFO inputType = {MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO outputType = {MFMediaType_Video, MFVideoFormat_H264};
    
    // Try software encoder first (simpler, no D3D11 device manager needed)
    hr = MFTEnumEx(
        MFT_CATEGORY_VIDEO_ENCODER_GUID,
        MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
        &inputType,
        &outputType,
        &activates,
        &count
    );
    
    if (FAILED(hr) || count == 0) {
        H264Log("No software H.264 encoders found, trying hardware\n");
        
        // Try hardware encoders (may need D3D11 setup)
        hr = MFTEnumEx(
            MFT_CATEGORY_VIDEO_ENCODER_GUID,
            MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
            &inputType,
            &outputType,
            &activates,
            &count
        );
        
        if (SUCCEEDED(hr) && count > 0) {
            *isHardware = TRUE;
        }
    }
    
    if (SUCCEEDED(hr) && count > 0) {
        // Activate the first encoder
        hr = activates[0]->lpVtbl->ActivateObject(activates[0], &IID_IMFTransform, (void**)&encoder);
        
        if (SUCCEEDED(hr)) {
            WCHAR name[256] = {0};
            UINT32 nameLen = 0;
            activates[0]->lpVtbl->GetString(activates[0], &MFT_FRIENDLY_NAME_Attribute, name, 256, &nameLen);
            H264Log("Using encoder: %ls (%s)\n", name, *isHardware ? "hardware" : "software");
            
            // For async MFTs (hardware), we may need to unlock them
            if (*isHardware) {
                IMFAttributes* attrs = NULL;
                hr = encoder->lpVtbl->GetAttributes(encoder, &attrs);
                if (SUCCEEDED(hr) && attrs) {
                    UINT32 isAsync = 0;
                    hr = attrs->lpVtbl->GetUINT32(attrs, &MF_TRANSFORM_ASYNC, &isAsync);
                    if (SUCCEEDED(hr) && isAsync) {
                        H264Log("Unlocking async MFT\n");
                        attrs->lpVtbl->SetUINT32(attrs, &MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
                    }
                    attrs->lpVtbl->Release(attrs);
                }
            }
        }
        
        // Release all activates
        for (UINT32 i = 0; i < count; i++) {
            activates[i]->lpVtbl->Release(activates[i]);
        }
        CoTaskMemFree(activates);
    }
    
    return encoder;
}

// Create input sample from BGRA data (converts to NV12)
static IMFSample* CreateInputSample(H264MemoryEncoder* enc, const BYTE* bgraData, LONGLONG timestamp) {
    HRESULT hr;
    IMFMediaBuffer* buffer = NULL;
    IMFSample* sample = NULL;
    
    // NV12 size: Y plane (w*h) + UV plane (w*h/2)
    DWORD nv12Size = enc->width * enc->height * 3 / 2;
    
    hr = MFCreateMemoryBuffer(nv12Size, &buffer);
    if (FAILED(hr)) return NULL;
    
    BYTE* bufferData = NULL;
    hr = buffer->lpVtbl->Lock(buffer, &bufferData, NULL, NULL);
    if (FAILED(hr)) {
        buffer->lpVtbl->Release(buffer);
        return NULL;
    }
    
    // Convert BGRA to NV12
    ColorConvert_BGRAtoNV12(bgraData, bufferData, enc->width, enc->height);
    
    buffer->lpVtbl->Unlock(buffer);
    buffer->lpVtbl->SetCurrentLength(buffer, nv12Size);
    
    // Create sample
    hr = MFCreateSample(&sample);
    if (FAILED(hr)) {
        buffer->lpVtbl->Release(buffer);
        return NULL;
    }
    
    hr = sample->lpVtbl->AddBuffer(sample, buffer);
    buffer->lpVtbl->Release(buffer);
    
    if (FAILED(hr)) {
        sample->lpVtbl->Release(sample);
        return NULL;
    }
    
    // Use frame-count based timing for consistent playback
    // This ensures smooth video at target FPS regardless of capture irregularities
    LONGLONG sampleTime = (LONGLONG)(enc->frameCount * enc->frameDuration);
    
    sample->lpVtbl->SetSampleTime(sample, sampleTime);
    sample->lpVtbl->SetSampleDuration(sample, (LONGLONG)enc->frameDuration);
    
    return sample;
}

// Process output from encoder
static BOOL ProcessEncoderOutput(H264MemoryEncoder* enc, EncodedFrame* outFrame) {
    HRESULT hr;
    DWORD status = 0;
    
    MFT_OUTPUT_DATA_BUFFER outputBuffer = {0};
    outputBuffer.dwStreamID = 0;
    outputBuffer.pSample = NULL;
    outputBuffer.dwStatus = 0;
    outputBuffer.pEvents = NULL;
    
    // Check if encoder needs output buffer allocated by us
    MFT_OUTPUT_STREAM_INFO streamInfo = {0};
    hr = enc->encoder->lpVtbl->GetOutputStreamInfo(enc->encoder, 0, &streamInfo);
    
    if (!(streamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
        // We need to provide the output sample
        IMFSample* outputSample = NULL;
        IMFMediaBuffer* outputMediaBuffer = NULL;
        
        DWORD bufferSize = streamInfo.cbSize > 0 ? streamInfo.cbSize : 1024 * 1024;
        hr = MFCreateMemoryBuffer(bufferSize, &outputMediaBuffer);
        if (FAILED(hr)) return FALSE;
        
        hr = MFCreateSample(&outputSample);
        if (FAILED(hr)) {
            outputMediaBuffer->lpVtbl->Release(outputMediaBuffer);
            return FALSE;
        }
        
        outputSample->lpVtbl->AddBuffer(outputSample, outputMediaBuffer);
        outputMediaBuffer->lpVtbl->Release(outputMediaBuffer);
        
        outputBuffer.pSample = outputSample;
    }
    
    hr = enc->encoder->lpVtbl->ProcessOutput(enc->encoder, 0, 1, &outputBuffer, &status);
    
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
        if (outputBuffer.pSample) {
            outputBuffer.pSample->lpVtbl->Release(outputBuffer.pSample);
        }
        return FALSE;
    }
    
    if (FAILED(hr)) {
        H264Log("ProcessOutput failed: 0x%08X\n", hr);
        if (outputBuffer.pSample) {
            outputBuffer.pSample->lpVtbl->Release(outputBuffer.pSample);
        }
        return FALSE;
    }
    
    // Extract data from output sample
    IMFSample* sample = outputBuffer.pSample;
    if (!sample) return FALSE;
    
    IMFMediaBuffer* buffer = NULL;
    hr = sample->lpVtbl->ConvertToContiguousBuffer(sample, &buffer);
    if (FAILED(hr)) {
        sample->lpVtbl->Release(sample);
        return FALSE;
    }
    
    BYTE* data = NULL;
    DWORD length = 0;
    hr = buffer->lpVtbl->Lock(buffer, &data, NULL, &length);
    if (FAILED(hr) || length == 0) {
        buffer->lpVtbl->Release(buffer);
        sample->lpVtbl->Release(sample);
        return FALSE;
    }
    
    // Copy to output frame
    outFrame->data = (BYTE*)malloc(length);
    if (outFrame->data) {
        memcpy(outFrame->data, data, length);
        outFrame->size = length;
        
        sample->lpVtbl->GetSampleTime(sample, &outFrame->timestamp);
        sample->lpVtbl->GetSampleDuration(sample, &outFrame->duration);
        
        // Check for keyframe
        UINT32 isKeyframe = 0;
        sample->lpVtbl->GetUINT32(sample, &MFSampleExtension_CleanPoint, &isKeyframe);
        outFrame->isKeyframe = (isKeyframe != 0);
    }
    
    buffer->lpVtbl->Unlock(buffer);
    buffer->lpVtbl->Release(buffer);
    sample->lpVtbl->Release(sample);
    
    if (outputBuffer.pEvents) {
        outputBuffer.pEvents->lpVtbl->Release(outputBuffer.pEvents);
    }
    
    return (outFrame->data != NULL);
}

BOOL H264Encoder_Init(H264MemoryEncoder* enc, int width, int height, int fps, QualityPreset quality) {
    HRESULT hr;
    
    ZeroMemory(enc, sizeof(H264MemoryEncoder));
    
    enc->width = width;
    enc->height = height;
    enc->fps = fps;
    enc->quality = quality;
    enc->bitrate = Util_CalculateBitrate(width, height, fps, quality);
    enc->frameDuration = 10000000ULL / fps;
    
    H264Log("H264Encoder_Init: %dx%d @ %d fps, bitrate=%u\n", width, height, fps, enc->bitrate);
    
    // Find encoder (prefers software, falls back to hardware)
    BOOL isHardware = FALSE;
    enc->encoder = FindH264Encoder(&isHardware);
    if (!enc->encoder) {
        H264Log("Failed to find H.264 encoder\n");
        return FALSE;
    }
    
    // Configure output type (H.264)
    hr = MFCreateMediaType(&enc->outputType);
    if (FAILED(hr)) {
        H264Log("Failed to create output type\n");
        goto fail;
    }
    
    enc->outputType->lpVtbl->SetGUID(enc->outputType, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    enc->outputType->lpVtbl->SetGUID(enc->outputType, &MF_MT_SUBTYPE, &MFVideoFormat_H264);
    enc->outputType->lpVtbl->SetUINT32(enc->outputType, &MF_MT_AVG_BITRATE, enc->bitrate);
    enc->outputType->lpVtbl->SetUINT32(enc->outputType, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    enc->outputType->lpVtbl->SetUINT32(enc->outputType, &MF_MT_MPEG2_PROFILE, 100);  // High profile
    
    UINT64 frameSize = ((UINT64)width << 32) | height;
    enc->outputType->lpVtbl->SetUINT64(enc->outputType, &MF_MT_FRAME_SIZE, frameSize);
    
    UINT64 frameRate = ((UINT64)fps << 32) | 1;
    enc->outputType->lpVtbl->SetUINT64(enc->outputType, &MF_MT_FRAME_RATE, frameRate);
    
    hr = enc->encoder->lpVtbl->SetOutputType(enc->encoder, 0, enc->outputType, 0);
    if (FAILED(hr)) {
        H264Log("SetOutputType failed: 0x%08X\n", hr);
        goto fail;
    }
    H264Log("Output type set successfully\n");
    
    // Configure input type - enumerate supported types from encoder
    hr = MFCreateMediaType(&enc->inputType);
    if (FAILED(hr)) {
        H264Log("Failed to create input type\n");
        goto fail;
    }
    
    // Try to get a supported input type from the encoder
    IMFMediaType* supportedInput = NULL;
    DWORD typeIndex = 0;
    BOOL foundInput = FALSE;
    
    while (SUCCEEDED(enc->encoder->lpVtbl->GetInputAvailableType(enc->encoder, 0, typeIndex, &supportedInput))) {
        GUID subtype = {0};
        if (SUCCEEDED(supportedInput->lpVtbl->GetGUID(supportedInput, &MF_MT_SUBTYPE, &subtype))) {
            // Log what we found
            if (IsEqualGUID(&subtype, &MFVideoFormat_NV12)) {
                H264Log("Encoder supports NV12 input (index %d)\n", typeIndex);
            } else if (IsEqualGUID(&subtype, &MFVideoFormat_IYUV)) {
                H264Log("Encoder supports IYUV input (index %d)\n", typeIndex);
            } else if (IsEqualGUID(&subtype, &MFVideoFormat_YV12)) {
                H264Log("Encoder supports YV12 input (index %d)\n", typeIndex);
            } else if (IsEqualGUID(&subtype, &MFVideoFormat_YUY2)) {
                H264Log("Encoder supports YUY2 input (index %d)\n", typeIndex);
            } else {
                H264Log("Encoder supports unknown format input (index %d)\n", typeIndex);
            }
            
            // Use NV12 if available (most common for hardware encoders)
            if (IsEqualGUID(&subtype, &MFVideoFormat_NV12) && !foundInput) {
                // Copy this type and modify frame size/rate
                enc->inputType->lpVtbl->Release(enc->inputType);
                enc->inputType = supportedInput;
                supportedInput = NULL;  // Don't release, we're using it
                
                // Update frame size and rate
                enc->inputType->lpVtbl->SetUINT64(enc->inputType, &MF_MT_FRAME_SIZE, frameSize);
                enc->inputType->lpVtbl->SetUINT64(enc->inputType, &MF_MT_FRAME_RATE, frameRate);
                foundInput = TRUE;
            }
        }
        if (supportedInput) {
            supportedInput->lpVtbl->Release(supportedInput);
            supportedInput = NULL;
        }
        typeIndex++;
    }
    
    if (!foundInput) {
        H264Log("No NV12 input type found, creating manual type\n");
        enc->inputType->lpVtbl->SetGUID(enc->inputType, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
        enc->inputType->lpVtbl->SetGUID(enc->inputType, &MF_MT_SUBTYPE, &MFVideoFormat_NV12);
        enc->inputType->lpVtbl->SetUINT32(enc->inputType, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        enc->inputType->lpVtbl->SetUINT64(enc->inputType, &MF_MT_FRAME_SIZE, frameSize);
        enc->inputType->lpVtbl->SetUINT64(enc->inputType, &MF_MT_FRAME_RATE, frameRate);
    }
    
    hr = enc->encoder->lpVtbl->SetInputType(enc->encoder, 0, enc->inputType, 0);
    if (FAILED(hr)) {
        H264Log("SetInputType failed: 0x%08X\n", hr);
        goto fail;
    }
    H264Log("Input type set successfully\n");
    
    // Try to set low latency mode via codec API
    ICodecAPI* codecAPI = NULL;
    hr = enc->encoder->lpVtbl->QueryInterface(enc->encoder, &IID_ICodecAPI, (void**)&codecAPI);
    if (SUCCEEDED(hr) && codecAPI) {
        VARIANT var;
        VariantInit(&var);
        var.vt = VT_BOOL;
        var.boolVal = VARIANT_TRUE;
        codecAPI->lpVtbl->SetValue(codecAPI, &CODECAPI_AVLowLatencyMode, &var);
        codecAPI->lpVtbl->Release(codecAPI);
        H264Log("Low latency mode enabled\n");
    }
    
    // Start processing
    hr = enc->encoder->lpVtbl->ProcessMessage(enc->encoder, MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (FAILED(hr)) {
        H264Log("BEGIN_STREAMING failed: 0x%08X\n", hr);
    }
    
    hr = enc->encoder->lpVtbl->ProcessMessage(enc->encoder, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    if (FAILED(hr)) {
        H264Log("START_OF_STREAM failed: 0x%08X\n", hr);
    }
    
    enc->initialized = TRUE;
    enc->frameCount = 0;
    
    H264Log("H264Encoder initialized successfully\n");
    return TRUE;
    
fail:
    H264Encoder_Shutdown(enc);
    return FALSE;
}

BOOL H264Encoder_EncodeFrame(H264MemoryEncoder* enc, const BYTE* bgraData, LONGLONG timestamp, EncodedFrame* outFrame) {
    if (!enc->initialized || !bgraData || !outFrame) return FALSE;
    
    ZeroMemory(outFrame, sizeof(EncodedFrame));
    
    // Initialize start time on first frame
    if (enc->frameCount == 0 && enc->perfFreq.QuadPart == 0) {
        QueryPerformanceFrequency(&enc->perfFreq);
        QueryPerformanceCounter(&enc->startTime);
    }
    
    // Create input sample
    IMFSample* inputSample = CreateInputSample(enc, bgraData, timestamp);
    if (!inputSample) {
        H264Log("Failed to create input sample\n");
        return FALSE;
    }
    
    // Feed to encoder
    HRESULT hr = enc->encoder->lpVtbl->ProcessInput(enc->encoder, 0, inputSample, 0);
    inputSample->lpVtbl->Release(inputSample);
    
    if (FAILED(hr)) {
        H264Log("ProcessInput failed: 0x%08X\n", hr);
        return FALSE;
    }
    
    enc->frameCount++;
    
    // Try to get output
    return ProcessEncoderOutput(enc, outFrame);
}

BOOL H264Encoder_Flush(H264MemoryEncoder* enc, EncodedFrame* outFrame) {
    if (!enc->initialized) return FALSE;
    
    ZeroMemory(outFrame, sizeof(EncodedFrame));
    
    // Send drain message
    enc->encoder->lpVtbl->ProcessMessage(enc->encoder, MFT_MESSAGE_COMMAND_DRAIN, 0);
    
    // Try to get remaining output
    return ProcessEncoderOutput(enc, outFrame);
}

void H264Encoder_Shutdown(H264MemoryEncoder* enc) {
    if (!enc) return;
    
    if (enc->encoder) {
        enc->encoder->lpVtbl->ProcessMessage(enc->encoder, MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        enc->encoder->lpVtbl->ProcessMessage(enc->encoder, MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        enc->encoder->lpVtbl->Release(enc->encoder);
        enc->encoder = NULL;
    }
    
    if (enc->inputType) {
        enc->inputType->lpVtbl->Release(enc->inputType);
        enc->inputType = NULL;
    }
    
    if (enc->outputType) {
        enc->outputType->lpVtbl->Release(enc->outputType);
        enc->outputType = NULL;
    }
    
    enc->initialized = FALSE;
    
    // Log cleanup is handled by replay_buffer.c
}

void EncodedFrame_Free(EncodedFrame* frame) {
    if (frame && frame->data) {
        free(frame->data);
        frame->data = NULL;
        frame->size = 0;
    }
}
