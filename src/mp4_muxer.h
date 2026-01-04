/*
 * MP4 Muxer
 * Writes H.264 encoded samples to MP4 file using IMFSinkWriter passthrough
 * Separated from sample buffer for single responsibility
 */

#ifndef MP4_MUXER_H
#define MP4_MUXER_H

#include <windows.h>
#include "config.h"

// Sample data for muxing (copies data from buffer)
typedef struct {
    BYTE* data;             // H.264 NAL unit data
    DWORD size;             // Size in bytes
    LONGLONG timestamp;     // Sample time (100-ns units)
    LONGLONG duration;      // Sample duration (100-ns units)
    BOOL isKeyframe;        // TRUE if IDR frame
} MuxerSample;

// Audio sample for muxing
typedef struct {
    BYTE* data;             // AAC frame data
    DWORD size;             // Size in bytes
    LONGLONG timestamp;     // Sample time (100-ns units)
    LONGLONG duration;      // Sample duration (100-ns units)
} MuxerAudioSample;

// Muxer configuration
typedef struct {
    int width;              // Video width
    int height;             // Video height
    int fps;                // Frame rate
    QualityPreset quality;  // For bitrate calculation
    BYTE* seqHeader;        // HEVC VPS/SPS/PPS sequence header
    DWORD seqHeaderSize;    // Size of sequence header
} MuxerConfig;

// Audio configuration
typedef struct {
    int sampleRate;         // e.g. 48000
    int channels;           // e.g. 2
    int bitrate;            // e.g. 192000
    BYTE* configData;       // AAC AudioSpecificConfig
    int configSize;
} MuxerAudioConfig;

// Write an array of samples to an MP4 file (video only)
// Uses H.264 passthrough muxing (no re-encoding)
// Returns TRUE on success
BOOL MP4Muxer_WriteFile(
    const char* outputPath,
    const MuxerSample* samples,
    int sampleCount,
    const MuxerConfig* config
);

// Write video and audio to MP4 file
BOOL MP4Muxer_WriteFileWithAudio(
    const char* outputPath,
    const MuxerSample* videoSamples,
    int videoSampleCount,
    const MuxerConfig* videoConfig,
    const MuxerAudioSample* audioSamples,
    int audioSampleCount,
    const MuxerAudioConfig* audioConfig
);

#endif // MP4_MUXER_H
