/*
 * MP4 Muxer Implementation
 * Writes H.264 encoded samples to MP4 file using IMFSinkWriter passthrough
 */

#include "mp4_muxer.h"
#include "util.h"
#include "logger.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <stdio.h>

// Alias for logging
#define MuxLog Logger_Log

BOOL MP4Muxer_WriteFile(
    const char* outputPath,
    const MuxerSample* samples,
    int sampleCount,
    const MuxerConfig* config)
{
    if (!outputPath || !samples || sampleCount <= 0 || !config) {
        MuxLog("MP4Muxer: Invalid parameters\n");
        return FALSE;
    }
    
    MuxLog("MP4Muxer: Writing %d samples to %s\n", sampleCount, outputPath);
    
    // Convert path to wide string
    WCHAR wPath[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, outputPath, -1, wPath, MAX_PATH);
    
    // Create SinkWriter with hardware acceleration enabled
    IMFSinkWriter* writer = NULL;
    IMFAttributes* attrs = NULL;
    
    HRESULT hr = MFCreateAttributes(&attrs, 2);
    if (SUCCEEDED(hr)) {
        attrs->lpVtbl->SetUINT32(attrs, &MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        attrs->lpVtbl->SetUINT32(attrs, &MF_LOW_LATENCY, TRUE);
    }
    
    hr = MFCreateSinkWriterFromURL(wPath, NULL, attrs, &writer);
    if (attrs) attrs->lpVtbl->Release(attrs);
    
    if (FAILED(hr)) {
        MuxLog("MP4Muxer: MFCreateSinkWriterFromURL failed 0x%08X\n", hr);
        return FALSE;
    }
    
    // Configure output type (H.264)
    IMFMediaType* outputType = NULL;
    hr = MFCreateMediaType(&outputType);
    if (FAILED(hr)) {
        writer->lpVtbl->Release(writer);
        return FALSE;
    }
    
    // Calculate bitrate using shared utility
    UINT32 bitrate = Util_CalculateBitrate(config->width, config->height, config->fps, config->quality);
    
    outputType->lpVtbl->SetGUID(outputType, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    outputType->lpVtbl->SetGUID(outputType, &MF_MT_SUBTYPE, &MFVideoFormat_H264);
    outputType->lpVtbl->SetUINT32(outputType, &MF_MT_AVG_BITRATE, bitrate);
    outputType->lpVtbl->SetUINT32(outputType, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    outputType->lpVtbl->SetUINT32(outputType, &MF_MT_MPEG2_PROFILE, 100);  // High profile
    
    UINT64 frameSize = ((UINT64)config->width << 32) | config->height;
    outputType->lpVtbl->SetUINT64(outputType, &MF_MT_FRAME_SIZE, frameSize);
    
    UINT64 frameRate = ((UINT64)config->fps << 32) | 1;
    outputType->lpVtbl->SetUINT64(outputType, &MF_MT_FRAME_RATE, frameRate);
    
    UINT64 pixelAspect = ((UINT64)1 << 32) | 1;  // Square pixels
    outputType->lpVtbl->SetUINT64(outputType, &MF_MT_PIXEL_ASPECT_RATIO, pixelAspect);
    
    MuxLog("MP4Muxer: %dx%d @ %d fps, bitrate=%u\n", 
           config->width, config->height, config->fps, bitrate);
    
    // Add stream
    DWORD streamIndex = 0;
    hr = writer->lpVtbl->AddStream(writer, outputType, &streamIndex);
    if (FAILED(hr)) {
        MuxLog("MP4Muxer: AddStream failed 0x%08X\n", hr);
        outputType->lpVtbl->Release(outputType);
        writer->lpVtbl->Release(writer);
        return FALSE;
    }
    
    // For H.264 passthrough, use the SAME type for input as output
    // This triggers passthrough mode - no transcoding
    hr = writer->lpVtbl->SetInputMediaType(writer, streamIndex, outputType, NULL);
    outputType->lpVtbl->Release(outputType);
    
    if (FAILED(hr)) {
        MuxLog("MP4Muxer: SetInputMediaType failed 0x%08X\n", hr);
        writer->lpVtbl->Release(writer);
        return FALSE;
    }
    
    // Begin writing
    hr = writer->lpVtbl->BeginWriting(writer);
    if (FAILED(hr)) {
        MuxLog("MP4Muxer: BeginWriting failed 0x%08X\n", hr);
        writer->lpVtbl->Release(writer);
        return FALSE;
    }
    
    // Write all samples with precise sequential timestamps
    int samplesWritten = 0;
    int keyframeCount = 0;
    
    for (int i = 0; i < sampleCount; i++) {
        const MuxerSample* sample = &samples[i];
        
        if (!sample->data || sample->size == 0) {
            continue;
        }
        
        // Create MF buffer
        IMFMediaBuffer* mfBuffer = NULL;
        hr = MFCreateMemoryBuffer(sample->size, &mfBuffer);
        if (FAILED(hr)) continue;
        
        BYTE* bufData = NULL;
        hr = mfBuffer->lpVtbl->Lock(mfBuffer, &bufData, NULL, NULL);
        if (FAILED(hr)) {
            mfBuffer->lpVtbl->Release(mfBuffer);
            continue;
        }
        
        memcpy(bufData, sample->data, sample->size);
        mfBuffer->lpVtbl->Unlock(mfBuffer);
        mfBuffer->lpVtbl->SetCurrentLength(mfBuffer, sample->size);
        
        // Create MF sample with REAL wall-clock timestamp from capture
        IMFSample* mfSample = NULL;
        hr = MFCreateSample(&mfSample);
        if (FAILED(hr)) {
            mfBuffer->lpVtbl->Release(mfBuffer);
            continue;
        }
        
        // Use the actual timestamp from capture (already normalized to start at 0)
        LONGLONG sampleTime = sample->timestamp;
        LONGLONG sampleDuration = sample->duration;
        
        mfSample->lpVtbl->AddBuffer(mfSample, mfBuffer);
        mfSample->lpVtbl->SetSampleTime(mfSample, sampleTime);
        mfSample->lpVtbl->SetSampleDuration(mfSample, sampleDuration);
        
        if (sample->isKeyframe) {
            mfSample->lpVtbl->SetUINT32(mfSample, &MFSampleExtension_CleanPoint, TRUE);
            keyframeCount++;
        }
        
        hr = writer->lpVtbl->WriteSample(writer, streamIndex, mfSample);
        mfSample->lpVtbl->Release(mfSample);
        mfBuffer->lpVtbl->Release(mfBuffer);
        
        if (SUCCEEDED(hr)) {
            samplesWritten++;
        } else {
            MuxLog("MP4Muxer: WriteSample failed at %d: 0x%08X\n", i, hr);
        }
    }
    
    // Log final stats - use last sample's actual timestamp for accurate duration
    LONGLONG finalDuration = (sampleCount > 0) ? samples[sampleCount-1].timestamp + samples[sampleCount-1].duration : 0;
    MuxLog("MP4Muxer: Wrote %d/%d samples (%.3fs real-time), keyframes: %d\n", 
           samplesWritten, sampleCount, (double)finalDuration / 10000000.0, keyframeCount);
    
    // Finalize
    hr = writer->lpVtbl->Finalize(writer);
    writer->lpVtbl->Release(writer);
    
    BOOL success = SUCCEEDED(hr) && samplesWritten > 0;
    MuxLog("MP4Muxer: Finalize %s\n", success ? "OK" : "FAILED");
    
    return success;
}
