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
    BOOL isKeyframe;        // TRUE if IDR frame
} MuxerSample;

// Muxer configuration
typedef struct {
    int width;              // Video width
    int height;             // Video height
    int fps;                // Frame rate
    QualityPreset quality;  // For bitrate calculation
} MuxerConfig;

// Write an array of samples to an MP4 file
// Uses H.264 passthrough muxing (no re-encoding)
// Returns TRUE on success
BOOL MP4Muxer_WriteFile(
    const char* outputPath,
    const MuxerSample* samples,
    int sampleCount,
    const MuxerConfig* config
);

#endif // MP4_MUXER_H
