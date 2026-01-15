/*
 * Audio Device Enumeration Implementation
 * Uses Windows Core Audio (MMDevice) API
 */

#define COBJMACROS
#define DEFINE_AUDIO_GUIDS  // This file defines the shared audio GUIDs
#include "audio_device.h"
#include "audio_guids.h"
#include "util.h"
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <stdio.h>

// COM interfaces
static IMMDeviceEnumerator* g_deviceEnumerator = NULL;

BOOL AudioDevice_Init(void) {
    if (g_deviceEnumerator) return TRUE;  // Already initialized
    
    HRESULT hr = CoCreateInstance(
        &CLSID_MMDeviceEnumerator_Shared,
        NULL,
        CLSCTX_ALL,
        &IID_IMMDeviceEnumerator_Shared,
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
            Util_WideToUtf8(wideId, defaultId, sizeof(defaultId));
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
            Util_WideToUtf8(wideId, info->id, sizeof(info->id));
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
                Util_WideToUtf8(varName.pwszVal, info->name, sizeof(info->name));
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
        Util_WideToUtf8(wideId, deviceId, maxLen);
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
        Util_WideToUtf8(wideId, deviceId, maxLen);
        CoTaskMemFree(wideId);
    }
    
    device->lpVtbl->Release(device);
    return TRUE;
}
