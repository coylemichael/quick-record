/*
 * AAC Audio Encoder Implementation
 * Uses Media Foundation AAC encoder MFT
 */

#include "aac_encoder.h"
#include <mfapi.h>
#include <mftransform.h>
#include <mferror.h>
#include <wmcodecdsp.h>
#include <stdio.h>

// AAC encoder CLSID
// {93AF0C51-2275-45d2-A35B-F2BA21CAED00}
static const GUID CLSID_AACEncoder = {
    0x93AF0C51, 0x2275, 0x45d2, {0xA3, 0x5B, 0xF2, 0xBA, 0x21, 0xCA, 0xED, 0x00}
};

struct AACEncoder {
    IMFTransform* transform;
    IMFMediaType* inputType;
    IMFMediaType* outputType;
    
    AACEncoderCallback callback;
    void* userData;
    
    // Input buffer accumulator
    BYTE* inputBuffer;
    int inputBufferSize;
    int inputBufferUsed;
    
    // Samples per AAC frame (1024 for AAC-LC)
    int samplesPerFrame;
    int bytesPerFrame;
    
    // Timing
    LONGLONG nextTimestamp;
    LONGLONG frameDuration;  // Duration of one AAC frame in 100ns
    
    // Encoder config (AudioSpecificConfig)
    BYTE* configData;
    int configSize;
    
    BOOL initialized;
};

// Helper: Create PCM input type
static IMFMediaType* CreatePCMType(void) {
    IMFMediaType* type = NULL;
    HRESULT hr = MFCreateMediaType(&type);
    if (FAILED(hr)) return NULL;
    
    type->lpVtbl->SetGUID(type, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
    type->lpVtbl->SetGUID(type, &MF_MT_SUBTYPE, &MFAudioFormat_PCM);
    type->lpVtbl->SetUINT32(type, &MF_MT_AUDIO_SAMPLES_PER_SECOND, AAC_SAMPLE_RATE);
    type->lpVtbl->SetUINT32(type, &MF_MT_AUDIO_NUM_CHANNELS, AAC_CHANNELS);
    type->lpVtbl->SetUINT32(type, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    type->lpVtbl->SetUINT32(type, &MF_MT_AUDIO_BLOCK_ALIGNMENT, AAC_CHANNELS * 2);
    type->lpVtbl->SetUINT32(type, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, AAC_SAMPLE_RATE * AAC_CHANNELS * 2);
    
    return type;
}

// Helper: Create AAC output type
static IMFMediaType* CreateAACType(void) {
    IMFMediaType* type = NULL;
    HRESULT hr = MFCreateMediaType(&type);
    if (FAILED(hr)) return NULL;
    
    type->lpVtbl->SetGUID(type, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
    type->lpVtbl->SetGUID(type, &MF_MT_SUBTYPE, &MFAudioFormat_AAC);
    type->lpVtbl->SetUINT32(type, &MF_MT_AUDIO_SAMPLES_PER_SECOND, AAC_SAMPLE_RATE);
    type->lpVtbl->SetUINT32(type, &MF_MT_AUDIO_NUM_CHANNELS, AAC_CHANNELS);
    type->lpVtbl->SetUINT32(type, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    type->lpVtbl->SetUINT32(type, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, AAC_BITRATE / 8);
    type->lpVtbl->SetUINT32(type, &MF_MT_AAC_PAYLOAD_TYPE, 0);  // Raw AAC
    type->lpVtbl->SetUINT32(type, &MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);  // AAC-LC
    
    return type;
}

// Process output from encoder
static void ProcessOutput(AACEncoder* encoder) {
    if (!encoder || !encoder->transform) return;
    
    MFT_OUTPUT_DATA_BUFFER outputBuffer = {0};
    DWORD status = 0;
    
    while (1) {
        // Check if we need to provide output sample
        MFT_OUTPUT_STREAM_INFO streamInfo = {0};
        encoder->transform->lpVtbl->GetOutputStreamInfo(encoder->transform, 0, &streamInfo);
        
        IMFSample* outputSample = NULL;
        IMFMediaBuffer* outputMediaBuffer = NULL;
        
        if (!(streamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
            // We need to provide the sample
            MFCreateSample(&outputSample);
            MFCreateMemoryBuffer(streamInfo.cbSize > 0 ? streamInfo.cbSize : 8192, &outputMediaBuffer);
            outputSample->lpVtbl->AddBuffer(outputSample, outputMediaBuffer);
            outputBuffer.pSample = outputSample;
        }
        
        outputBuffer.dwStreamID = 0;
        outputBuffer.dwStatus = 0;
        outputBuffer.pEvents = NULL;
        
        HRESULT hr = encoder->transform->lpVtbl->ProcessOutput(
            encoder->transform, 0, 1, &outputBuffer, &status
        );
        
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            if (outputSample) outputSample->lpVtbl->Release(outputSample);
            if (outputMediaBuffer) outputMediaBuffer->lpVtbl->Release(outputMediaBuffer);
            break;
        }
        
        if (SUCCEEDED(hr) && outputBuffer.pSample) {
            // Get encoded data
            IMFMediaBuffer* buffer = NULL;
            outputBuffer.pSample->lpVtbl->ConvertToContiguousBuffer(outputBuffer.pSample, &buffer);
            
            if (buffer) {
                BYTE* data = NULL;
                DWORD dataLen = 0;
                buffer->lpVtbl->Lock(buffer, &data, NULL, &dataLen);
                
                if (data && dataLen > 0 && encoder->callback) {
                    AACSample sample = {0};
                    sample.data = data;
                    sample.size = dataLen;
                    sample.timestamp = encoder->nextTimestamp;
                    sample.duration = encoder->frameDuration;
                    
                    encoder->callback(&sample, encoder->userData);
                    
                    encoder->nextTimestamp += encoder->frameDuration;
                }
                
                buffer->lpVtbl->Unlock(buffer);
                buffer->lpVtbl->Release(buffer);
            }
        }
        
        if (outputBuffer.pEvents) {
            outputBuffer.pEvents->lpVtbl->Release(outputBuffer.pEvents);
        }
        if (outputSample) outputSample->lpVtbl->Release(outputSample);
        if (outputMediaBuffer) outputMediaBuffer->lpVtbl->Release(outputMediaBuffer);
        
        if (FAILED(hr)) break;
    }
}

AACEncoder* AACEncoder_Create(void) {
    AACEncoder* encoder = (AACEncoder*)calloc(1, sizeof(AACEncoder));
    if (!encoder) return NULL;
    
    // AAC-LC: 1024 samples per frame
    encoder->samplesPerFrame = 1024;
    encoder->bytesPerFrame = encoder->samplesPerFrame * AAC_CHANNELS * 2;  // 16-bit stereo
    
    // Frame duration in 100ns units
    encoder->frameDuration = (LONGLONG)encoder->samplesPerFrame * 10000000 / AAC_SAMPLE_RATE;
    
    // Allocate input buffer (hold multiple frames worth)
    encoder->inputBufferSize = encoder->bytesPerFrame * 4;
    encoder->inputBuffer = (BYTE*)malloc(encoder->inputBufferSize);
    if (!encoder->inputBuffer) {
        free(encoder);
        return NULL;
    }
    
    // Create AAC encoder MFT
    HRESULT hr = CoCreateInstance(
        &CLSID_AACEncoder,
        NULL,
        CLSCTX_INPROC_SERVER,
        &IID_IMFTransform,
        (void**)&encoder->transform
    );
    
    if (FAILED(hr)) {
        // Try alternate method - enumerate encoders
        MFT_REGISTER_TYPE_INFO inputInfo = {MFMediaType_Audio, MFAudioFormat_PCM};
        MFT_REGISTER_TYPE_INFO outputInfo = {MFMediaType_Audio, MFAudioFormat_AAC};
        
        IMFActivate** activates = NULL;
        UINT32 count = 0;
        
        hr = MFTEnumEx(
            MFT_CATEGORY_AUDIO_ENCODER,
            MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
            &inputInfo,
            &outputInfo,
            &activates,
            &count
        );
        
        if (SUCCEEDED(hr) && count > 0) {
            hr = activates[0]->lpVtbl->ActivateObject(
                activates[0],
                &IID_IMFTransform,
                (void**)&encoder->transform
            );
            
            for (UINT32 i = 0; i < count; i++) {
                activates[i]->lpVtbl->Release(activates[i]);
            }
            CoTaskMemFree(activates);
        }
    }
    
    if (!encoder->transform) {
        free(encoder->inputBuffer);
        free(encoder);
        return NULL;
    }
    
    // Set output type first (AAC encoders often need this)
    encoder->outputType = CreateAACType();
    hr = encoder->transform->lpVtbl->SetOutputType(encoder->transform, 0, encoder->outputType, 0);
    if (FAILED(hr)) {
        // Try getting available output type
        encoder->outputType->lpVtbl->Release(encoder->outputType);
        encoder->outputType = NULL;
        
        hr = encoder->transform->lpVtbl->GetOutputAvailableType(encoder->transform, 0, 0, &encoder->outputType);
        if (SUCCEEDED(hr)) {
            // Modify to our preferred settings
            encoder->outputType->lpVtbl->SetUINT32(encoder->outputType, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, AAC_BITRATE / 8);
            hr = encoder->transform->lpVtbl->SetOutputType(encoder->transform, 0, encoder->outputType, 0);
        }
    }
    
    // Set input type
    encoder->inputType = CreatePCMType();
    hr = encoder->transform->lpVtbl->SetInputType(encoder->transform, 0, encoder->inputType, 0);
    if (FAILED(hr)) {
        encoder->inputType->lpVtbl->Release(encoder->inputType);
        encoder->inputType = NULL;
        
        // Try getting available input type
        hr = encoder->transform->lpVtbl->GetInputAvailableType(encoder->transform, 0, 0, &encoder->inputType);
        if (SUCCEEDED(hr)) {
            hr = encoder->transform->lpVtbl->SetInputType(encoder->transform, 0, encoder->inputType, 0);
        }
    }
    
    if (FAILED(hr)) {
        AACEncoder_Destroy(encoder);
        return NULL;
    }
    
    // Get AudioSpecificConfig from output type
    UINT32 blobSize = 0;
    hr = encoder->outputType->lpVtbl->GetBlobSize(encoder->outputType, &MF_MT_USER_DATA, &blobSize);
    if (SUCCEEDED(hr) && blobSize > 0) {
        encoder->configData = (BYTE*)malloc(blobSize);
        encoder->outputType->lpVtbl->GetBlob(encoder->outputType, &MF_MT_USER_DATA, 
            encoder->configData, blobSize, &encoder->configSize);
    }
    
    // Start streaming
    hr = encoder->transform->lpVtbl->ProcessMessage(encoder->transform, MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    hr = encoder->transform->lpVtbl->ProcessMessage(encoder->transform, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    
    encoder->initialized = TRUE;
    return encoder;
}

void AACEncoder_Destroy(AACEncoder* encoder) {
    if (!encoder) return;
    
    if (encoder->transform) {
        encoder->transform->lpVtbl->ProcessMessage(encoder->transform, MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        encoder->transform->lpVtbl->ProcessMessage(encoder->transform, MFT_MESSAGE_COMMAND_DRAIN, 0);
        encoder->transform->lpVtbl->Release(encoder->transform);
    }
    
    if (encoder->inputType) encoder->inputType->lpVtbl->Release(encoder->inputType);
    if (encoder->outputType) encoder->outputType->lpVtbl->Release(encoder->outputType);
    if (encoder->inputBuffer) free(encoder->inputBuffer);
    if (encoder->configData) free(encoder->configData);
    
    free(encoder);
}

void AACEncoder_SetCallback(AACEncoder* encoder, AACEncoderCallback callback, void* userData) {
    if (!encoder) return;
    encoder->callback = callback;
    encoder->userData = userData;
}

BOOL AACEncoder_Feed(AACEncoder* encoder, const BYTE* pcmData, int pcmSize, LONGLONG timestamp) {
    if (!encoder || !encoder->transform || !pcmData || pcmSize <= 0) return FALSE;
    
    // Set initial timestamp if not set
    if (encoder->nextTimestamp == 0 && timestamp > 0) {
        encoder->nextTimestamp = timestamp;
    }
    
    // Add to input buffer
    int offset = 0;
    while (offset < pcmSize) {
        int toCopy = pcmSize - offset;
        int spaceAvailable = encoder->inputBufferSize - encoder->inputBufferUsed;
        if (toCopy > spaceAvailable) toCopy = spaceAvailable;
        
        memcpy(encoder->inputBuffer + encoder->inputBufferUsed, pcmData + offset, toCopy);
        encoder->inputBufferUsed += toCopy;
        offset += toCopy;
        
        // Process complete frames
        while (encoder->inputBufferUsed >= encoder->bytesPerFrame) {
            // Create input sample
            IMFSample* sample = NULL;
            IMFMediaBuffer* buffer = NULL;
            
            MFCreateSample(&sample);
            MFCreateMemoryBuffer(encoder->bytesPerFrame, &buffer);
            
            BYTE* bufData = NULL;
            buffer->lpVtbl->Lock(buffer, &bufData, NULL, NULL);
            memcpy(bufData, encoder->inputBuffer, encoder->bytesPerFrame);
            buffer->lpVtbl->Unlock(buffer);
            buffer->lpVtbl->SetCurrentLength(buffer, encoder->bytesPerFrame);
            
            sample->lpVtbl->AddBuffer(sample, buffer);
            sample->lpVtbl->SetSampleTime(sample, encoder->nextTimestamp);
            sample->lpVtbl->SetSampleDuration(sample, encoder->frameDuration);
            
            // Feed to encoder
            HRESULT hr = encoder->transform->lpVtbl->ProcessInput(encoder->transform, 0, sample, 0);
            
            buffer->lpVtbl->Release(buffer);
            sample->lpVtbl->Release(sample);
            
            // Shift input buffer
            encoder->inputBufferUsed -= encoder->bytesPerFrame;
            if (encoder->inputBufferUsed > 0) {
                memmove(encoder->inputBuffer, encoder->inputBuffer + encoder->bytesPerFrame, encoder->inputBufferUsed);
            }
            
            if (SUCCEEDED(hr)) {
                ProcessOutput(encoder);
            }
        }
    }
    
    return TRUE;
}

void AACEncoder_Flush(AACEncoder* encoder) {
    if (!encoder || !encoder->transform) return;
    
    // Pad remaining input with silence and encode
    if (encoder->inputBufferUsed > 0) {
        int padding = encoder->bytesPerFrame - encoder->inputBufferUsed;
        memset(encoder->inputBuffer + encoder->inputBufferUsed, 0, padding);
        encoder->inputBufferUsed = encoder->bytesPerFrame;
        
        // Encode final frame
        AACEncoder_Feed(encoder, NULL, 0, 0);  // Will process the buffer
    }
    
    // Drain encoder
    encoder->transform->lpVtbl->ProcessMessage(encoder->transform, MFT_MESSAGE_COMMAND_DRAIN, 0);
    ProcessOutput(encoder);
}

BOOL AACEncoder_GetConfig(AACEncoder* encoder, BYTE** configData, int* configSize) {
    if (!encoder || !configData || !configSize) return FALSE;
    
    if (encoder->configData && encoder->configSize > 0) {
        *configData = encoder->configData;
        *configSize = encoder->configSize;
        return TRUE;
    }
    
    return FALSE;
}
