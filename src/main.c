/*
 * Ultra Lightweight Screen Recorder
 * Pure Win32 + DXGI Desktop Duplication + Media Foundation
 * Zero external dependencies, maximum performance
 */

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <initguid.h>
#include <windows.h>
#include <windowsx.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comdlg32.lib")

// Forward declarations
#include "config.h"
#include "capture.h"
#include "encoder.h"
#include "overlay.h"
#include "replay_buffer.h"
#include "logger.h"
#include "crash_handler.h"

// Global state
AppConfig g_config;
CaptureState g_capture;
ReplayBufferState g_replayBuffer;
BOOL g_isRecording = FALSE;
BOOL g_isSelecting = FALSE;
HWND g_overlayWnd = NULL;
HWND g_controlWnd = NULL;

// Hotkey ID for replay save
#define HOTKEY_REPLAY_SAVE 1

// Debug mode flag (enabled via --debug CLI argument)
static BOOL g_debugMode = FALSE;

// Mutex for single instance detection
HANDLE g_mutex = NULL;
const char* MUTEX_NAME = "LightweightScreenRecorderMutex";
const char* WINDOW_CLASS = "LWSROverlay";

// Parse command line for --debug flag
static void ParseCommandLine(LPSTR lpCmdLine) {
    if (lpCmdLine && (strstr(lpCmdLine, "--debug") || strstr(lpCmdLine, "-d"))) {
        g_debugMode = TRUE;
    }
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, 
                   LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)nCmdShow;
    
    // Initialize crash handler first
    CrashHandler_Init();
    
    // Parse command line arguments
    ParseCommandLine(lpCmdLine);
    
    // Check for existing instance - toggle recording if running
    g_mutex = OpenMutexA(MUTEX_ALL_ACCESS, FALSE, MUTEX_NAME);
    if (g_mutex) {
        // Another instance exists - signal it to stop recording
        HWND existingWnd = FindWindowA(WINDOW_CLASS, NULL);
        if (existingWnd) {
            PostMessage(existingWnd, WM_USER + 1, 0, 0); // Custom stop message
        }
        CloseHandle(g_mutex);
        return 0;
    }
    
    // Create mutex for this instance
    g_mutex = CreateMutexA(NULL, TRUE, MUTEX_NAME);
    
    // Initialize COM
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        MessageBoxA(NULL, "Failed to initialize COM", "Error", MB_OK | MB_ICONERROR);
        return 1;
    }
    
    // Set AppUserModelID for taskbar pinning
    SetCurrentProcessExplicitAppUserModelID(L"CarnmorCyber.LightWeightScreenRecorder");
    
    // Initialize Media Foundation
    hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    if (FAILED(hr)) {
        MessageBoxA(NULL, "Failed to initialize Media Foundation", "Error", MB_OK | MB_ICONERROR);
        CoUninitialize();
        return 1;
    }
    
    // Load configuration
    Config_Load(&g_config);
    
    // Initialize capture system
    if (!Capture_Init(&g_capture)) {
        MessageBoxA(NULL, "Failed to initialize screen capture", "Error", MB_OK | MB_ICONERROR);
        MFShutdown();
        CoUninitialize();
        return 1;
    }
    
    // Create and show overlay
    if (!Overlay_Create(hInstance)) {
        MessageBoxA(NULL, "Failed to create overlay", "Error", MB_OK | MB_ICONERROR);
        Capture_Shutdown(&g_capture);
        MFShutdown();
        CoUninitialize();
        return 1;
    }
    
    // Initialize replay buffer
    ReplayBuffer_Init(&g_replayBuffer);
    
    // Initialize logger for replay debugging (only if --debug flag is set)
    if (g_debugMode) {
        Logger_Init("replay_debug.txt", "w");
    }
    
    // Start replay buffer if enabled in config
    if (g_config.replayEnabled) {
        ReplayBuffer_Start(&g_replayBuffer, &g_config);
        // Register global hotkey for saving replay
        RegisterHotKey(g_controlWnd, HOTKEY_REPLAY_SAVE, 0, g_config.replaySaveKey);
    }
    
    // Message loop
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    
    // Cleanup
    UnregisterHotKey(g_controlWnd, HOTKEY_REPLAY_SAVE);
    ReplayBuffer_Shutdown(&g_replayBuffer);
    Logger_Shutdown();
    Config_Save(&g_config);
    Capture_Shutdown(&g_capture);
    MFShutdown();
    CoUninitialize();
    
    if (g_mutex) {
        ReleaseMutex(g_mutex);
        CloseHandle(g_mutex);
    }
    
    return (int)msg.wParam;
}
