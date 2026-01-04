/*
 * MP4 Muxer Implementation
 * Writes HEVC (H.265) encoded samples to MP4 file using IMFSinkWriter passthrough
 */

#include "mp4_muxer.h"
#include "util.h"
#include "logger.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <stdio.h>

// HEVC format GUID: {43564548-0000-0010-8000-00AA00389B71}
static const GUID MFVideoFormat_HEVC_Local = 
    {0x43564548, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

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
    outputType->lpVtbl->SetGUID(outputType, &MF_MT_SUBTYPE, &MFVideoFormat_HEVC_Local);
    outputType->lpVtbl->SetUINT32(outputType, &MF_MT_AVG_BITRATE, bitrate);
    outputType->lpVtbl->SetUINT32(outputType, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    outputType->lpVtbl->SetUINT32(outputType, &MF_MT_MPEG2_PROFILE, 1);  // HEVC Main profile
    
    UINT64 frameSize = ((UINT64)config->width << 32) | config->height;
    outputType->lpVtbl->SetUINT64(outputType, &MF_MT_FRAME_SIZE, frameSize);
    
    UINT64 frameRate = ((UINT64)config->fps << 32) | 1;
    outputType->lpVtbl->SetUINT64(outputType, &MF_MT_FRAME_RATE, frameRate);
    
    UINT64 pixelAspect = ((UINT64)1 << 32) | 1;  // Square pixels
    outputType->lpVtbl->SetUINT64(outputType, &MF_MT_PIXEL_ASPECT_RATIO, pixelAspect);
    
    // Set HEVC sequence header (VPS/SPS/PPS) if provided
    if (config->seqHeader && config->seqHeaderSize > 0) {
        hr = outputType->lpVtbl->SetBlob(outputType, &MF_MT_MPEG_SEQUENCE_HEADER, 
                                         config->seqHeader, config->seqHeaderSize);
        if (SUCCEEDED(hr)) {
            MuxLog("MP4Muxer: Set sequence header (%u bytes)\n", config->seqHeaderSize);
        } else {
            MuxLog("MP4Muxer: SetBlob sequence header failed 0x%08X\n", hr);
        }
    }
    
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
    if (FAILED(hr)) {
        MuxLog("MP4Muxer: Finalize failed with HRESULT 0x%08X\n", hr);
    }
    writer->lpVtbl->Release(writer);
    
    BOOL success = SUCCEEDED(hr) && samplesWritten > 0;
    MuxLog("MP4Muxer: Finalize %s\n", success ? "OK" : "FAILED");
    
    return success;
}

// Write video and audio to MP4 file
BOOL MP4Muxer_WriteFileWithAudio(
    const char* outputPath,
    const MuxerSample* videoSamples,
    int videoSampleCount,
    const MuxerConfig* videoConfig,
    const MuxerAudioSample* audioSamples,
    int audioSampleCount,
    const MuxerAudioConfig* audioConfig)
{
    if (!outputPath || !videoSamples || videoSampleCount <= 0 || !videoConfig) {
        MuxLog("MP4Muxer: Invalid video parameters\n");
        return FALSE;
    }
    
    // If no audio, fall back to video-only
    if (!audioSamples || audioSampleCount <= 0 || !audioConfig) {
        return MP4Muxer_WriteFile(outputPath, videoSamples, videoSampleCount, videoConfig);
    }
    
    MuxLog("MP4Muxer: Writing %d video + %d audio samples to %s\n", 
           videoSampleCount, audioSampleCount, outputPath);
    
    // Convert path to wide string
    WCHAR wPath[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, outputPath, -1, wPath, MAX_PATH);
    
    // Create SinkWriter
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
    
    // === VIDEO STREAM ===
    IMFMediaType* videoType = NULL;
    hr = MFCreateMediaType(&videoType);
    if (FAILED(hr)) {
        writer->lpVtbl->Release(writer);
        return FALSE;
    }
    
    UINT32 bitrate = Util_CalculateBitrate(videoConfig->width, videoConfig->height, 
                                           videoConfig->fps, videoConfig->quality);
    
    videoType->lpVtbl->SetGUID(videoType, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    videoType->lpVtbl->SetGUID(videoType, &MF_MT_SUBTYPE, &MFVideoFormat_HEVC_Local);
    videoType->lpVtbl->SetUINT32(videoType, &MF_MT_AVG_BITRATE, bitrate);
    videoType->lpVtbl->SetUINT32(videoType, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    videoType->lpVtbl->SetUINT32(videoType, &MF_MT_MPEG2_PROFILE, 1);  // HEVC Main
    
    UINT64 frameSize = ((UINT64)videoConfig->width << 32) | videoConfig->height;
    videoType->lpVtbl->SetUINT64(videoType, &MF_MT_FRAME_SIZE, frameSize);
    
    UINT64 frameRate = ((UINT64)videoConfig->fps << 32) | 1;
    videoType->lpVtbl->SetUINT64(videoType, &MF_MT_FRAME_RATE, frameRate);
    
    UINT64 pixelAspect = ((UINT64)1 << 32) | 1;
    videoType->lpVtbl->SetUINT64(videoType, &MF_MT_PIXEL_ASPECT_RATIO, pixelAspect);
    
    // Set HEVC sequence header (VPS/SPS/PPS) if provided
    if (videoConfig->seqHeader && videoConfig->seqHeaderSize > 0) {
        hr = videoType->lpVtbl->SetBlob(videoType, &MF_MT_MPEG_SEQUENCE_HEADER, 
                                        videoConfig->seqHeader, videoConfig->seqHeaderSize);
        if (SUCCEEDED(hr)) {
            MuxLog("MP4Muxer: Set video sequence header (%u bytes)\n", videoConfig->seqHeaderSize);
        }
    }
    
    DWORD videoStreamIndex = 0;
    hr = writer->lpVtbl->AddStream(writer, videoType, &videoStreamIndex);
    if (FAILED(hr)) {
        MuxLog("MP4Muxer: AddStream (video) failed 0x%08X\n", hr);
        videoType->lpVtbl->Release(videoType);
        writer->lpVtbl->Release(writer);
        return FALSE;
    }
    
    hr = writer->lpVtbl->SetInputMediaType(writer, videoStreamIndex, videoType, NULL);
    videoType->lpVtbl->Release(videoType);
    
    if (FAILED(hr)) {
        MuxLog("MP4Muxer: SetInputMediaType (video) failed 0x%08X\n", hr);
        writer->lpVtbl->Release(writer);
        return FALSE;
    }
    
    // === AUDIO STREAM ===
    IMFMediaType* audioType = NULL;
    hr = MFCreateMediaType(&audioType);
    if (FAILED(hr)) {
        writer->lpVtbl->Release(writer);
        return FALSE;
    }
    
    audioType->lpVtbl->SetGUID(audioType, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
    audioType->lpVtbl->SetGUID(audioType, &MF_MT_SUBTYPE, &MFAudioFormat_AAC);
    audioType->lpVtbl->SetUINT32(audioType, &MF_MT_AUDIO_SAMPLES_PER_SECOND, audioConfig->sampleRate);
    audioType->lpVtbl->SetUINT32(audioType, &MF_MT_AUDIO_NUM_CHANNELS, audioConfig->channels);
    audioType->lpVtbl->SetUINT32(audioType, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    audioType->lpVtbl->SetUINT32(audioType, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, audioConfig->bitrate / 8);
    audioType->lpVtbl->SetUINT32(audioType, &MF_MT_AAC_PAYLOAD_TYPE, 0);  // Raw AAC
    
    // Set AudioSpecificConfig if available
    if (audioConfig->configData && audioConfig->configSize > 0) {
        audioType->lpVtbl->SetBlob(audioType, &MF_MT_USER_DATA, 
            audioConfig->configData, audioConfig->configSize);
    }
    
    DWORD audioStreamIndex = 0;
    hr = writer->lpVtbl->AddStream(writer, audioType, &audioStreamIndex);
    if (FAILED(hr)) {
        MuxLog("MP4Muxer: AddStream (audio) failed 0x%08X\n", hr);
        audioType->lpVtbl->Release(audioType);
        writer->lpVtbl->Release(writer);
        return FALSE;
    }
    
    hr = writer->lpVtbl->SetInputMediaType(writer, audioStreamIndex, audioType, NULL);
    audioType->lpVtbl->Release(audioType);
    
    if (FAILED(hr)) {
        MuxLog("MP4Muxer: SetInputMediaType (audio) failed 0x%08X\n", hr);
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
    
    // === INTERLEAVED WRITING ===
    // Write samples in timestamp order for proper interleaving
    int videoIdx = 0;
    int audioIdx = 0;
    int videoWritten = 0;
    int audioWritten = 0;
    
    while (videoIdx < videoSampleCount || audioIdx < audioSampleCount) {
        BOOL writeVideo = FALSE;
        
        if (videoIdx >= videoSampleCount) {
            writeVideo = FALSE;
        } else if (audioIdx >= audioSampleCount) {
            writeVideo = TRUE;
        } else {
            // Compare timestamps - write whichever is earlier
            writeVideo = (videoSamples[videoIdx].timestamp <= audioSamples[audioIdx].timestamp);
        }
        
        if (writeVideo && videoIdx < videoSampleCount) {
            const MuxerSample* sample = &videoSamples[videoIdx++];
            if (!sample->data || sample->size == 0) continue;
            
            IMFMediaBuffer* mfBuffer = NULL;
            hr = MFCreateMemoryBuffer(sample->size, &mfBuffer);
            if (FAILED(hr)) continue;
            
            BYTE* bufData = NULL;
            mfBuffer->lpVtbl->Lock(mfBuffer, &bufData, NULL, NULL);
            memcpy(bufData, sample->data, sample->size);
            mfBuffer->lpVtbl->Unlock(mfBuffer);
            mfBuffer->lpVtbl->SetCurrentLength(mfBuffer, sample->size);
            
            IMFSample* mfSample = NULL;
            MFCreateSample(&mfSample);
            mfSample->lpVtbl->AddBuffer(mfSample, mfBuffer);
            mfSample->lpVtbl->SetSampleTime(mfSample, sample->timestamp);
            mfSample->lpVtbl->SetSampleDuration(mfSample, sample->duration);
            
            if (sample->isKeyframe) {
                mfSample->lpVtbl->SetUINT32(mfSample, &MFSampleExtension_CleanPoint, TRUE);
            }
            
            hr = writer->lpVtbl->WriteSample(writer, videoStreamIndex, mfSample);
            mfSample->lpVtbl->Release(mfSample);
            mfBuffer->lpVtbl->Release(mfBuffer);
            
            if (SUCCEEDED(hr)) videoWritten++;
        } else if (audioIdx < audioSampleCount) {
            const MuxerAudioSample* sample = &audioSamples[audioIdx++];
            if (!sample->data || sample->size == 0) continue;
            
            IMFMediaBuffer* mfBuffer = NULL;
            hr = MFCreateMemoryBuffer(sample->size, &mfBuffer);
            if (FAILED(hr)) continue;
            
            BYTE* bufData = NULL;
            mfBuffer->lpVtbl->Lock(mfBuffer, &bufData, NULL, NULL);
            memcpy(bufData, sample->data, sample->size);
            mfBuffer->lpVtbl->Unlock(mfBuffer);
            mfBuffer->lpVtbl->SetCurrentLength(mfBuffer, sample->size);
            
            IMFSample* mfSample = NULL;
            MFCreateSample(&mfSample);
            mfSample->lpVtbl->AddBuffer(mfSample, mfBuffer);
            mfSample->lpVtbl->SetSampleTime(mfSample, sample->timestamp);
            mfSample->lpVtbl->SetSampleDuration(mfSample, sample->duration);
            
            hr = writer->lpVtbl->WriteSample(writer, audioStreamIndex, mfSample);
            mfSample->lpVtbl->Release(mfSample);
            mfBuffer->lpVtbl->Release(mfBuffer);
            
            if (SUCCEEDED(hr)) audioWritten++;
        }
    }
    
    MuxLog("MP4Muxer: Wrote %d/%d video, %d/%d audio samples\n",
           videoWritten, videoSampleCount, audioWritten, audioSampleCount);
    
    // Finalize
    hr = writer->lpVtbl->Finalize(writer);
    if (FAILED(hr)) {
        MuxLog("MP4Muxer: Finalize failed with HRESULT 0x%08X\n", hr);
    }
    writer->lpVtbl->Release(writer);
    
    BOOL success = SUCCEEDED(hr) && videoWritten > 0;
    MuxLog("MP4Muxer: Finalize %s\n", success ? "OK" : "FAILED");
    
    return success;
}
