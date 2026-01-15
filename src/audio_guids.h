/*
 * Shared Audio GUIDs
 * Windows Core Audio API GUIDs used across audio modules
 * 
 * Usage:
 *   In exactly ONE .c file (e.g., audio_device.c), define DEFINE_AUDIO_GUIDS
 *   before including this header:
 *     #define DEFINE_AUDIO_GUIDS
 *     #include "audio_guids.h"
 *   
 *   In all other files, just include without the define:
 *     #include "audio_guids.h"
 */

#ifndef AUDIO_GUIDS_H
#define AUDIO_GUIDS_H

#include <windows.h>
#include <initguid.h>

// MMDevice API GUIDs
// CLSID_MMDeviceEnumerator: {BCDE0395-E52F-467C-8E3D-C4579291692E}
// IID_IMMDeviceEnumerator:  {A95664D2-9614-4F35-A746-DE8DB63617E6}

#ifdef DEFINE_AUDIO_GUIDS
// Define the GUIDs (only in one translation unit)
DEFINE_GUID(CLSID_MMDeviceEnumerator_Shared, 
    0xBCDE0395, 0xE52F, 0x467C, 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E);
DEFINE_GUID(IID_IMMDeviceEnumerator_Shared, 
    0xA95664D2, 0x9614, 0x4F35, 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6);
DEFINE_GUID(IID_IAudioClient_Shared, 
    0x1CB9AD4C, 0xDBFA, 0x4c32, 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2);
DEFINE_GUID(IID_IAudioCaptureClient_Shared, 
    0xC8ADBD64, 0xE71E, 0x48a0, 0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17);
#else
// Declare as extern (all other translation units)
DEFINE_GUID(CLSID_MMDeviceEnumerator_Shared, 
    0xBCDE0395, 0xE52F, 0x467C, 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E);
DEFINE_GUID(IID_IMMDeviceEnumerator_Shared, 
    0xA95664D2, 0x9614, 0x4F35, 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6);
DEFINE_GUID(IID_IAudioClient_Shared, 
    0x1CB9AD4C, 0xDBFA, 0x4c32, 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2);
DEFINE_GUID(IID_IAudioCaptureClient_Shared, 
    0xC8ADBD64, 0xE71E, 0x48a0, 0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17);
#endif

#endif // AUDIO_GUIDS_H
