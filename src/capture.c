/*
 * DXGI Desktop Duplication Implementation
 * Using proper C-style COM vtable calls
 */

#include "capture.h"
#include <stdio.h>

// Monitor enumeration data
typedef struct {
    int targetIndex;
    int currentIndex;
    RECT bounds;
    BOOL found;
} MonitorSearchData;

static BOOL CALLBACK MonitorEnumProcInternal(HMONITOR hMonitor, HDC hdcMonitor, 
                                              LPRECT lprcMonitor, LPARAM dwData) {
    (void)hMonitor;
    (void)hdcMonitor;
    MonitorSearchData* data = (MonitorSearchData*)dwData;
    
    if (data->currentIndex == data->targetIndex) {
        data->bounds = *lprcMonitor;
        data->found = TRUE;
        return FALSE; // Stop enumeration
    }
    
    data->currentIndex++;
    return TRUE;
}

static BOOL CALLBACK MonitorEnumProcAll(HMONITOR hMonitor, HDC hdcMonitor,
                                         LPRECT lprcMonitor, LPARAM dwData) {
    (void)hMonitor;
    (void)hdcMonitor;
    RECT* bounds = (RECT*)dwData;
    
    if (IsRectEmpty(bounds)) {
        *bounds = *lprcMonitor;
    } else {
        UnionRect(bounds, bounds, lprcMonitor);
    }
    
    return TRUE;
}

typedef struct {
    MonitorEnumProc callback;
    void* userData;
    int index;
} EnumCallbackData;

static BOOL CALLBACK MonitorEnumProcCallback(HMONITOR hMonitor, HDC hdcMonitor,
                                              LPRECT lprcMonitor, LPARAM dwData) {
    (void)hdcMonitor;
    EnumCallbackData* data = (EnumCallbackData*)dwData;
    
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    GetMonitorInfo(hMonitor, &mi);
    
    BOOL isPrimary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    BOOL continueEnum = data->callback(data->index, *lprcMonitor, isPrimary, data->userData);
    data->index++;
    
    return continueEnum;
}

void Capture_EnumMonitors(MonitorEnumProc callback, void* userData) {
    EnumCallbackData data;
    data.callback = callback;
    data.userData = userData;
    data.index = 0;
    EnumDisplayMonitors(NULL, NULL, MonitorEnumProcCallback, (LPARAM)&data);
}

BOOL Capture_GetMonitorFromPoint(POINT pt, RECT* monitorRect, int* monitorIndex) {
    HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    if (!hMon) return FALSE;
    
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfo(hMon, &mi)) return FALSE;
    
    *monitorRect = mi.rcMonitor;
    *monitorIndex = 0; // Default to primary for now
    
    return TRUE;
}

BOOL Capture_GetAllMonitorsBounds(RECT* bounds) {
    SetRectEmpty(bounds);
    EnumDisplayMonitors(NULL, NULL, MonitorEnumProcAll, (LPARAM)bounds);
    return !IsRectEmpty(bounds);
}

BOOL Capture_GetWindowRect(HWND hwnd, RECT* rect) {
    if (!IsWindow(hwnd)) return FALSE;
    
    // Use DwmGetWindowAttribute for accurate bounds if available
    typedef HRESULT (WINAPI *DwmGetWindowAttributeFunc)(HWND, DWORD, PVOID, DWORD);
    static DwmGetWindowAttributeFunc pDwmGetWindowAttribute = NULL;
    static BOOL tried = FALSE;
    
    if (!tried) {
        HMODULE hDwm = LoadLibraryA("dwmapi.dll");
        if (hDwm) {
            pDwmGetWindowAttribute = (DwmGetWindowAttributeFunc)
                GetProcAddress(hDwm, "DwmGetWindowAttribute");
        }
        tried = TRUE;
    }
    
    if (pDwmGetWindowAttribute) {
        // DWMWA_EXTENDED_FRAME_BOUNDS = 9
        if (SUCCEEDED(pDwmGetWindowAttribute(hwnd, 9, rect, sizeof(RECT)))) {
            return TRUE;
        }
    }
    
    return GetWindowRect(hwnd, rect);
}

BOOL Capture_Init(CaptureState* state) {
    ZeroMemory(state, sizeof(CaptureState));
    
    // Create D3D11 device
    D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL featureLevel;
    
    HRESULT hr = D3D11CreateDevice(
        NULL,
        D3D_DRIVER_TYPE_HARDWARE,
        NULL,
        0,
        featureLevels,
        1,
        D3D11_SDK_VERSION,
        &state->device,
        &featureLevel,
        &state->context
    );
    
    if (FAILED(hr)) {
        return FALSE;
    }
    
    // Get DXGI device
    IDXGIDevice* dxgiDevice = NULL;
    hr = state->device->lpVtbl->QueryInterface(state->device, &IID_IDXGIDevice, (void**)&dxgiDevice);
    if (FAILED(hr)) {
        state->device->lpVtbl->Release(state->device);
        return FALSE;
    }
    
    // Get DXGI adapter
    IDXGIAdapter* dxgiAdapter = NULL;
    hr = dxgiDevice->lpVtbl->GetAdapter(dxgiDevice, &dxgiAdapter);
    dxgiDevice->lpVtbl->Release(dxgiDevice);
    if (FAILED(hr)) {
        state->device->lpVtbl->Release(state->device);
        return FALSE;
    }
    
    // Get primary output
    IDXGIOutput* dxgiOutput = NULL;
    hr = dxgiAdapter->lpVtbl->EnumOutputs(dxgiAdapter, 0, &dxgiOutput);
    dxgiAdapter->lpVtbl->Release(dxgiAdapter);
    if (FAILED(hr)) {
        state->device->lpVtbl->Release(state->device);
        return FALSE;
    }
    
    // Get output description
    hr = dxgiOutput->lpVtbl->GetDesc(dxgiOutput, &state->outputDesc);
    if (FAILED(hr)) {
        dxgiOutput->lpVtbl->Release(dxgiOutput);
        state->device->lpVtbl->Release(state->device);
        return FALSE;
    }
    
    // Get refresh rate from display mode
    DXGI_MODE_DESC desiredMode = {0};
    desiredMode.Width = state->outputDesc.DesktopCoordinates.right - state->outputDesc.DesktopCoordinates.left;
    desiredMode.Height = state->outputDesc.DesktopCoordinates.bottom - state->outputDesc.DesktopCoordinates.top;
    desiredMode.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    
    DXGI_MODE_DESC closestMode = {0};
    hr = dxgiOutput->lpVtbl->FindClosestMatchingMode(dxgiOutput, &desiredMode, &closestMode, (IUnknown*)state->device);
    
    if (SUCCEEDED(hr) && closestMode.RefreshRate.Denominator > 0) {
        state->monitorRefreshRate = closestMode.RefreshRate.Numerator / closestMode.RefreshRate.Denominator;
    } else {
        state->monitorRefreshRate = 60; // Default fallback
    }
    
    // Get Output1 interface for duplication
    IDXGIOutput1* dxgiOutput1 = NULL;
    hr = dxgiOutput->lpVtbl->QueryInterface(dxgiOutput, &IID_IDXGIOutput1, (void**)&dxgiOutput1);
    dxgiOutput->lpVtbl->Release(dxgiOutput);
    if (FAILED(hr)) {
        state->device->lpVtbl->Release(state->device);
        return FALSE;
    }
    
    // Create desktop duplication
    hr = dxgiOutput1->lpVtbl->DuplicateOutput(dxgiOutput1, (IUnknown*)state->device, &state->duplication);
    dxgiOutput1->lpVtbl->Release(dxgiOutput1);
    if (FAILED(hr)) {
        state->device->lpVtbl->Release(state->device);
        return FALSE;
    }
    
    state->monitorWidth = state->outputDesc.DesktopCoordinates.right - 
                          state->outputDesc.DesktopCoordinates.left;
    state->monitorHeight = state->outputDesc.DesktopCoordinates.bottom - 
                           state->outputDesc.DesktopCoordinates.top;
    
    // Default capture region is full monitor
    state->captureRect = state->outputDesc.DesktopCoordinates;
    state->captureWidth = state->monitorWidth;
    state->captureHeight = state->monitorHeight;
    
    state->initialized = TRUE;
    return TRUE;
}

BOOL Capture_SetRegion(CaptureState* state, RECT region) {
    if (!state->initialized) return FALSE;
    
    // Clamp to monitor bounds
    IntersectRect(&state->captureRect, &region, &state->outputDesc.DesktopCoordinates);
    
    state->captureWidth = state->captureRect.right - state->captureRect.left;
    state->captureHeight = state->captureRect.bottom - state->captureRect.top;
    
    // Ensure even dimensions for video encoding
    state->captureWidth &= ~1;
    state->captureHeight &= ~1;
    state->captureRect.right = state->captureRect.left + state->captureWidth;
    state->captureRect.bottom = state->captureRect.top + state->captureHeight;
    
    // Reallocate frame buffer if needed
    size_t newSize = (size_t)state->captureWidth * state->captureHeight * 4;
    if (newSize > state->frameBufferSize) {
        free(state->frameBuffer);
        state->frameBuffer = (BYTE*)malloc(newSize);
        state->frameBufferSize = newSize;
    }
    
    return state->frameBuffer != NULL;
}

BOOL Capture_SetMonitor(CaptureState* state, int monitorIndex) {
    MonitorSearchData search = {0};
    search.targetIndex = monitorIndex;
    search.currentIndex = 0;
    search.found = FALSE;
    
    EnumDisplayMonitors(NULL, NULL, MonitorEnumProcInternal, (LPARAM)&search);
    
    if (search.found) {
        return Capture_SetRegion(state, search.bounds);
    }
    
    return FALSE;
}

BOOL Capture_SetAllMonitors(CaptureState* state) {
    RECT bounds;
    if (Capture_GetAllMonitorsBounds(&bounds)) {
        return Capture_SetRegion(state, bounds);
    }
    return FALSE;
}

BYTE* Capture_GetFrame(CaptureState* state, UINT64* timestamp) {
    if (!state->initialized || !state->duplication) return NULL;
    
    IDXGIResource* desktopResource = NULL;
    DXGI_OUTDUPL_FRAME_INFO frameInfo;
    
    // Try to acquire next frame
    HRESULT hr = state->duplication->lpVtbl->AcquireNextFrame(
        state->duplication, 0, &frameInfo, &desktopResource);
    
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        // No new frame, return last frame if available
        if (state->frameBuffer && state->lastFrameTime > 0) {
            *timestamp = state->lastFrameTime;
            return state->frameBuffer;
        }
        return NULL;
    }
    
    if (FAILED(hr)) {
        // Duplication might need to be recreated (resolution change, etc.)
        return NULL;
    }
    
    // Get texture from resource
    ID3D11Texture2D* desktopTexture = NULL;
    hr = desktopResource->lpVtbl->QueryInterface(desktopResource, &IID_ID3D11Texture2D, 
                                                  (void**)&desktopTexture);
    desktopResource->lpVtbl->Release(desktopResource);
    
    if (FAILED(hr)) {
        state->duplication->lpVtbl->ReleaseFrame(state->duplication);
        return NULL;
    }
    
    // Create or update staging texture
    D3D11_TEXTURE2D_DESC desc;
    desktopTexture->lpVtbl->GetDesc(desktopTexture, &desc);
    
    if (!state->stagingTexture) {
        D3D11_TEXTURE2D_DESC stagingDesc = {0};
        stagingDesc.Width = desc.Width;
        stagingDesc.Height = desc.Height;
        stagingDesc.MipLevels = 1;
        stagingDesc.ArraySize = 1;
        stagingDesc.Format = desc.Format;
        stagingDesc.SampleDesc.Count = 1;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.BindFlags = 0;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stagingDesc.MiscFlags = 0;
        
        hr = state->device->lpVtbl->CreateTexture2D(state->device, &stagingDesc, NULL, &state->stagingTexture);
        if (FAILED(hr)) {
            desktopTexture->lpVtbl->Release(desktopTexture);
            state->duplication->lpVtbl->ReleaseFrame(state->duplication);
            return NULL;
        }
    }
    
    // Copy region to staging texture
    D3D11_BOX srcBox;
    srcBox.left = state->captureRect.left - state->outputDesc.DesktopCoordinates.left;
    srcBox.top = state->captureRect.top - state->outputDesc.DesktopCoordinates.top;
    srcBox.right = srcBox.left + state->captureWidth;
    srcBox.bottom = srcBox.top + state->captureHeight;
    srcBox.front = 0;
    srcBox.back = 1;
    
    state->context->lpVtbl->CopySubresourceRegion(
        state->context,
        (ID3D11Resource*)state->stagingTexture, 0, 0, 0, 0,
        (ID3D11Resource*)desktopTexture, 0, &srcBox
    );
    
    desktopTexture->lpVtbl->Release(desktopTexture);
    
    // Map staging texture to CPU memory
    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = state->context->lpVtbl->Map(state->context, (ID3D11Resource*)state->stagingTexture,
                                      0, D3D11_MAP_READ, 0, &mapped);
    
    if (FAILED(hr)) {
        state->duplication->lpVtbl->ReleaseFrame(state->duplication);
        return NULL;
    }
    
    // Copy to frame buffer (handle pitch difference)
    BYTE* src = (BYTE*)mapped.pData;
    BYTE* dst = state->frameBuffer;
    int rowBytes = state->captureWidth * 4;
    
    for (int y = 0; y < state->captureHeight; y++) {
        memcpy(dst, src, rowBytes);
        src += mapped.RowPitch;
        dst += rowBytes;
    }
    
    state->context->lpVtbl->Unmap(state->context, (ID3D11Resource*)state->stagingTexture, 0);
    state->duplication->lpVtbl->ReleaseFrame(state->duplication);
    
    state->lastFrameTime = frameInfo.LastPresentTime.QuadPart;
    *timestamp = state->lastFrameTime;
    
    return state->frameBuffer;
}

void Capture_ReleaseFrame(CaptureState* state) {
    (void)state; // Frame is already released in GetFrame
}

int Capture_GetRefreshRate(CaptureState* state) {
    return state->monitorRefreshRate;
}

void Capture_Shutdown(CaptureState* state) {
    if (state->frameBuffer) {
        free(state->frameBuffer);
        state->frameBuffer = NULL;
    }
    
    if (state->stagingTexture) {
        state->stagingTexture->lpVtbl->Release(state->stagingTexture);
        state->stagingTexture = NULL;
    }
    
    if (state->duplication) {
        state->duplication->lpVtbl->Release(state->duplication);
        state->duplication = NULL;
    }
    
    if (state->context) {
        state->context->lpVtbl->Release(state->context);
        state->context = NULL;
    }
    
    if (state->device) {
        state->device->lpVtbl->Release(state->device);
        state->device = NULL;
    }
    
    state->initialized = FALSE;
}
