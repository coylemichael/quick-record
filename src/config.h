/*
 * Configuration Management
 * Persistent settings via INI file
 */

#ifndef CONFIG_H
#define CONFIG_H

#include <windows.h>

// Output formats
typedef enum {
    FORMAT_MP4 = 0,
    FORMAT_AVI,
    FORMAT_WMV,
    FORMAT_COUNT
} OutputFormat;

// Capture modes
typedef enum {
    MODE_AREA = 0,
    MODE_WINDOW,
    MODE_MONITOR,
    MODE_ALL_MONITORS
} CaptureMode;

// Quality presets
typedef enum {
    QUALITY_LOW = 0,
    QUALITY_MEDIUM,
    QUALITY_HIGH,
    QUALITY_LOSSLESS
} QualityPreset;

typedef struct {
    // Recording settings
    OutputFormat outputFormat;
    QualityPreset quality;
    BOOL captureMouse;
    BOOL showRecordingBorder;
    int maxRecordingSeconds;  // 0 = unlimited
    
    // Save location
    char savePath[MAX_PATH];
    
    // Last capture area (for quick re-record)
    RECT lastCaptureRect;
    CaptureMode lastMode;
    
} AppConfig;

// Get config file path
void Config_GetPath(char* buffer, size_t size);

// Load config from INI file
void Config_Load(AppConfig* config);

// Save config to INI file
void Config_Save(const AppConfig* config);

// Get format extension
const char* Config_GetFormatExtension(OutputFormat format);

// Get format display name
const char* Config_GetFormatName(OutputFormat format);

#endif // CONFIG_H
