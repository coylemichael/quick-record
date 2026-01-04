/*
 * Audio Device Enumeration
 * Uses Windows Core Audio (MMDevice) API to enumerate audio devices
 */

#ifndef AUDIO_DEVICE_H
#define AUDIO_DEVICE_H

#include <windows.h>

// Maximum number of audio devices we support
#define MAX_AUDIO_DEVICES 32

// Audio device type
typedef enum {
    AUDIO_DEVICE_OUTPUT = 0,    // Speakers, headphones (for loopback capture)
    AUDIO_DEVICE_INPUT          // Microphones, line-in
} AudioDeviceType;

// Audio device info
typedef struct {
    char id[128];               // Device ID (for WASAPI)
    char name[128];             // Friendly name (for display)
    AudioDeviceType type;       // Output or input
    BOOL isDefault;             // Is this the default device?
} AudioDeviceInfo;

// List of audio devices
typedef struct {
    AudioDeviceInfo devices[MAX_AUDIO_DEVICES];
    int count;
} AudioDeviceList;

// Initialize audio device enumeration (call once at startup)
BOOL AudioDevice_Init(void);

// Shutdown audio device enumeration
void AudioDevice_Shutdown(void);

// Enumerate all audio devices (both input and output)
// Returns number of devices found
int AudioDevice_Enumerate(AudioDeviceList* list);

// Get device info by ID
// Returns TRUE if found
BOOL AudioDevice_GetById(const char* deviceId, AudioDeviceInfo* info);

// Get the default output device ID
BOOL AudioDevice_GetDefaultOutput(char* deviceId, int maxLen);

// Get the default input device ID
BOOL AudioDevice_GetDefaultInput(char* deviceId, int maxLen);

#endif // AUDIO_DEVICE_H
