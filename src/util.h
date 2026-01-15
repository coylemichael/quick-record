/*
 * Utility Functions
 * Shared calculations and helpers used across modules
 */

#ifndef UTIL_H
#define UTIL_H

#include <windows.h>
#include "config.h"

// Media Foundation time units (100-nanosecond intervals)
#define MF_UNITS_PER_SECOND 10000000LL

// Calculate video bitrate based on quality preset
// Uses ShadowPlay-style scaling: base bitrate scales with resolution and FPS
// Returns bitrate in bits per second
UINT32 Util_CalculateBitrate(int width, int height, int fps, QualityPreset quality);

// Calculate precise timestamp for a frame (avoids cumulative rounding errors)
// Returns timestamp in 100-ns units
LONGLONG Util_CalculateTimestamp(int frameNumber, int fps);

// Calculate precise frame duration
// Returns duration in 100-ns units
LONGLONG Util_CalculateFrameDuration(int frameNumber, int fps);

// Calculate aspect ratio crop rectangle centered on source bounds
// Returns the cropped RECT; ratioW/ratioH define the target aspect (e.g., 16, 9)
RECT Util_CalculateAspectRect(RECT sourceBounds, int ratioW, int ratioH);

// Get aspect ratio dimensions from config index
// Index: 0=Native, 1=16:9, 2=9:16, 3=1:1, 4=4:5, 5=16:10, 6=4:3, 7=21:9, 8=32:9
void Util_GetAspectRatioDimensions(int aspectIndex, int* ratioW, int* ratioH);

// ============================================================================
// String Conversion Utilities
// ============================================================================

// Convert wide string to UTF-8
// Returns number of bytes written (excluding null terminator), or 0 on failure
int Util_WideToUtf8(const WCHAR* wide, char* utf8, int maxLen);

// Convert UTF-8 string to wide string
// Returns number of characters written (excluding null terminator), or 0 on failure
int Util_Utf8ToWide(const char* utf8, WCHAR* wide, int maxLen);

#endif // UTIL_H
