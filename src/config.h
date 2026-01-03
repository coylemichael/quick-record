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
    MODE_NONE = 0,    // No mode selected (initial state)
    MODE_AREA,
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
    
    // UI settings
    int cancelKey;  // Virtual key code to close overlay (default: VK_ESCAPE)
    
    // Replay buffer settings (instant replay)
    BOOL replayEnabled;              // Enable replay buffer
    int replayDuration;              // Buffer duration in seconds (60-1200)
    CaptureMode replayCaptureSource; // What to capture for replay
    int replayMonitorIndex;          // Which monitor (if MODE_MONITOR)
    int replaySaveKey;               // Hotkey to save replay (default: F9)
    RECT replayAreaRect;             // Custom area for replay (if MODE_AREA)
    int replayAspectRatio;           // 0=Native, 1=16:9, 2=16:10, 3=4:3, 4=21:9, 5=32:9
    int replayFPS;                   // 30 or 60
    
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
