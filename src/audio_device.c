/*
 * Audio Device Enumeration Implementation
 * Uses Windows Core Audio (MMDevice) API
 */

#define INITGUID
#include <initguid.h>
#include "audio_device.h"
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <stdio.h>

// Define the GUIDs we need
DEFINE_GUID(CLSID_MMDeviceEnumerator_Local, 0xBCDE0395, 0xE52F, 0x467C, 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E);
DEFINE_GUID(IID_IMMDeviceEnumerator_Local, 0xA95664D2, 0x9614, 0x4F35, 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6);

// COM interfaces
static IMMDeviceEnumerator* g_deviceEnumerator = NULL;

// Convert wide string to UTF-8
static void WideToUtf8(const WCHAR* wide, char* utf8, int maxLen) {
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, maxLen, NULL, NULL);
}

BOOL AudioDevice_Init(void) {
    if (g_deviceEnumerator) return TRUE;  // Already initialized
    
    HRESULT hr = CoCreateInstance(
        &CLSID_MMDeviceEnumerator_Local,
        NULL,
        CLSCTX_ALL,
        &IID_IMMDeviceEnumerator_Local,
        (void**)&g_deviceEnumerator
    );
    
    return SUCCEEDED(hr);
}

void AudioDevice_Shutdown(void) {
    if (g_deviceEnumerator) {
        g_deviceEnumerator->lpVtbl->Release(g_deviceEnumerator);
        g_deviceEnumerator = NULL;
    }
}

// Enumerate devices of a specific type (render or capture)
static int EnumerateDeviceType(AudioDeviceList* list, EDataFlow dataFlow, AudioDeviceType type) {
    if (!g_deviceEnumerator || !list) return 0;
    
    IMMDeviceCollection* collection = NULL;
    HRESULT hr = g_deviceEnumerator->lpVtbl->EnumAudioEndpoints(
        g_deviceEnumerator,
        dataFlow,
        DEVICE_STATE_ACTIVE,
        &collection
    );
    
    if (FAILED(hr)) return 0;
    
    UINT count = 0;
    collection->lpVtbl->GetCount(collection, &count);
    
    // Get default device ID for comparison
    char defaultId[128] = {0};
    IMMDevice* defaultDevice = NULL;
    hr = g_deviceEnumerator->lpVtbl->GetDefaultAudioEndpoint(
        g_deviceEnumerator,
        dataFlow,
        eConsole,
        &defaultDevice
    );
    
    if (SUCCEEDED(hr) && defaultDevice) {
        LPWSTR wideId = NULL;
        defaultDevice->lpVtbl->GetId(defaultDevice, &wideId);
        if (wideId) {
            WideToUtf8(wideId, defaultId, sizeof(defaultId));
            CoTaskMemFree(wideId);
        }
        defaultDevice->lpVtbl->Release(defaultDevice);
    }
    
    int added = 0;
    for (UINT i = 0; i < count && list->count < MAX_AUDIO_DEVICES; i++) {
        IMMDevice* device = NULL;
        hr = collection->lpVtbl->Item(collection, i, &device);
        if (FAILED(hr)) continue;
        
        AudioDeviceInfo* info = &list->devices[list->count];
        
        // Get device ID
        LPWSTR wideId = NULL;
        device->lpVtbl->GetId(device, &wideId);
        if (wideId) {
            WideToUtf8(wideId, info->id, sizeof(info->id));
            CoTaskMemFree(wideId);
        }
        
        // Get friendly name from properties
        IPropertyStore* props = NULL;
        hr = device->lpVtbl->OpenPropertyStore(device, STGM_READ, &props);
        if (SUCCEEDED(hr)) {
            PROPVARIANT varName;
            PropVariantInit(&varName);
            hr = props->lpVtbl->GetValue(props, &PKEY_Device_FriendlyName, &varName);
            if (SUCCEEDED(hr) && varName.vt == VT_LPWSTR) {
                WideToUtf8(varName.pwszVal, info->name, sizeof(info->name));
            }
            PropVariantClear(&varName);
            props->lpVtbl->Release(props);
        }
        
        // If no name, use ID
        if (info->name[0] == '\0') {
            strncpy(info->name, info->id, sizeof(info->name) - 1);
        }
        
        info->type = type;
        info->isDefault = (strcmp(info->id, defaultId) == 0);
        
        device->lpVtbl->Release(device);
        
        list->count++;
        added++;
    }
    
    collection->lpVtbl->Release(collection);
    return added;
}

int AudioDevice_Enumerate(AudioDeviceList* list) {
    if (!list) return 0;
    
    // Auto-initialize if needed
    if (!g_deviceEnumerator) {
        if (!AudioDevice_Init()) {
            return 0;
        }
    }
    
    list->count = 0;
    
    // Enumerate output devices (for loopback - system audio)
    EnumerateDeviceType(list, eRender, AUDIO_DEVICE_OUTPUT);
    
    // Enumerate input devices (microphones)
    EnumerateDeviceType(list, eCapture, AUDIO_DEVICE_INPUT);
    
    return list->count;
}

BOOL AudioDevice_GetById(const char* deviceId, AudioDeviceInfo* info) {
    if (!deviceId || !info) return FALSE;
    
    AudioDeviceList list;
    AudioDevice_Enumerate(&list);
    
    for (int i = 0; i < list.count; i++) {
        if (strcmp(list.devices[i].id, deviceId) == 0) {
            *info = list.devices[i];
            return TRUE;
        }
    }
    
    return FALSE;
}

BOOL AudioDevice_GetDefaultOutput(char* deviceId, int maxLen) {
    if (!g_deviceEnumerator || !deviceId) return FALSE;
    
    IMMDevice* device = NULL;
    HRESULT hr = g_deviceEnumerator->lpVtbl->GetDefaultAudioEndpoint(
        g_deviceEnumerator,
        eRender,
        eConsole,
        &device
    );
    
    if (FAILED(hr) || !device) return FALSE;
    
    LPWSTR wideId = NULL;
    device->lpVtbl->GetId(device, &wideId);
    if (wideId) {
        WideToUtf8(wideId, deviceId, maxLen);
        CoTaskMemFree(wideId);
    }
    
    device->lpVtbl->Release(device);
    return TRUE;
}

BOOL AudioDevice_GetDefaultInput(char* deviceId, int maxLen) {
    if (!g_deviceEnumerator || !deviceId) return FALSE;
    
    IMMDevice* device = NULL;
    HRESULT hr = g_deviceEnumerator->lpVtbl->GetDefaultAudioEndpoint(
        g_deviceEnumerator,
        eCapture,
        eConsole,
        &device
    );
    
    if (FAILED(hr) || !device) return FALSE;
    
    LPWSTR wideId = NULL;
    device->lpVtbl->GetId(device, &wideId);
    if (wideId) {
        WideToUtf8(wideId, deviceId, maxLen);
        CoTaskMemFree(wideId);
    }
    
    device->lpVtbl->Release(device);
    return TRUE;
}
