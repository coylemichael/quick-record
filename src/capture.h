/*
 * Screen Capture using DXGI Desktop Duplication
 * Hardware-accelerated, zero-copy where possible
 */

#ifndef CAPTURE_H
#define CAPTURE_H

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

typedef struct {
    // D3D11 resources
    ID3D11Device* device;
    ID3D11DeviceContext* context;
    IDXGIOutputDuplication* duplication;
    ID3D11Texture2D* stagingTexture;      // CPU-accessible staging texture
    ID3D11Texture2D* gpuTexture;          // GPU texture for zero-copy path
    IDXGIAdapter* adapter;                // Keep adapter for switching outputs
    
    // Monitor info
    DXGI_OUTPUT_DESC outputDesc;
    int monitorIndex;
    int monitorWidth;
    int monitorHeight;
    int monitorRefreshRate;
    
    // Capture region
    RECT captureRect;
    int captureWidth;
    int captureHeight;
    
    // Frame data
    BYTE* frameBuffer;
    size_t frameBufferSize;
    UINT64 lastFrameTime;
    
    // State
    BOOL initialized;
    BOOL capturing;
    
} CaptureState;

// Initialize the capture system
BOOL Capture_Init(CaptureState* state);

// Set capture region (screen coordinates)
BOOL Capture_SetRegion(CaptureState* state, RECT region);

// Set to capture specific monitor
BOOL Capture_SetMonitor(CaptureState* state, int monitorIndex);

// Set to capture all monitors
BOOL Capture_SetAllMonitors(CaptureState* state);

// Get a frame (returns pointer to RGB data)
BYTE* Capture_GetFrame(CaptureState* state, UINT64* timestamp);

// Release the current frame
void Capture_ReleaseFrame(CaptureState* state);

// Get monitor refresh rate
int Capture_GetRefreshRate(CaptureState* state);

// Shutdown capture system
void Capture_Shutdown(CaptureState* state);

// Get frame as GPU texture (stays on GPU, no CPU copy)
// Returns a BGRA texture that can be used for GPU processing
// Caller must NOT release the texture - it's owned by capture state
ID3D11Texture2D* Capture_GetFrameTexture(CaptureState* state, UINT64* timestamp);

// Helper: Get window rect for window capture mode
BOOL Capture_GetWindowRect(HWND hwnd, RECT* rect);

// Helper: Get monitor rect from point
BOOL Capture_GetMonitorFromPoint(POINT pt, RECT* monitorRect, int* monitorIndex);

// Helper: Get all monitors bounding rect
BOOL Capture_GetAllMonitorsBounds(RECT* bounds);

// Helper: Enumerate monitors
typedef BOOL (*MonitorEnumProc)(int index, RECT bounds, BOOL isPrimary, void* userData);
void Capture_EnumMonitors(MonitorEnumProc callback, void* userData);

// Helper: Get monitor bounds by index
BOOL Capture_GetMonitorBoundsByIndex(int monitorIndex, RECT* bounds);

#endif // CAPTURE_H
