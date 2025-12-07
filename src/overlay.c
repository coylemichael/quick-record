/*
 * Overlay Implementation
 * Selection UI, recording controls, and main logic
 */

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shlobj.h>
#include <stdio.h>

#include "overlay.h"
#include "capture.h"
#include "encoder.h"
#include "config.h"

#pragma comment(lib, "comctl32.lib")

// External globals from main.c
extern AppConfig g_config;
extern CaptureState g_capture;
extern BOOL g_isRecording;
extern BOOL g_isSelecting;
extern HWND g_overlayWnd;
extern HWND g_controlWnd;

// Control IDs
#define ID_MODE_AREA       1001
#define ID_MODE_WINDOW     1002
#define ID_MODE_MONITOR    1003
#define ID_MODE_ALL        1004
#define ID_BTN_CLOSE       1005
#define ID_BTN_STOP        1006
#define ID_CHK_MOUSE       1007
#define ID_CHK_BORDER      1008
#define ID_CMB_FORMAT      1009
#define ID_CMB_QUALITY     1010
#define ID_EDT_PATH        1011
#define ID_BTN_BROWSE      1012
#define ID_BTN_SETTINGS    1013
#define ID_EDT_TIMELIMIT   1014
#define ID_BTN_RECORD      1015
#define ID_CMB_HOURS       1016
#define ID_CMB_MINUTES     1017
#define ID_CMB_SECONDS     1018
#define ID_TIMER_RECORD    2001
#define ID_TIMER_LIMIT     2002

// Window state
static HINSTANCE g_hInstance;
static CaptureMode g_currentMode = MODE_AREA;
static BOOL g_isDragging = FALSE;
static POINT g_dragStart;
static POINT g_dragEnd;
static RECT g_selectedRect;
static HWND g_settingsWnd = NULL;
static HWND g_crosshairWnd = NULL;
static EncoderState g_encoder;
static HANDLE g_recordThread = NULL;
static volatile BOOL g_stopRecording = FALSE;

// Recording thread
static DWORD WINAPI RecordingThread(LPVOID param);

// Window procedures
static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static LRESULT CALLBACK ControlWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static LRESULT CALLBACK CrosshairWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Helper to get primary monitor center position
static void GetPrimaryMonitorCenter(POINT* pt) {
    HMONITOR hMon = MonitorFromPoint((POINT){0,0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfo(hMon, &mi);
    pt->x = (mi.rcMonitor.left + mi.rcMonitor.right) / 2;
    pt->y = mi.rcMonitor.top + 80; // Near top
}

// Draw dotted selection rectangle
static void DrawSelectionRect(HDC hdc, RECT* rect) {
    HPEN dotPen = CreatePen(PS_DOT, 1, RGB(255, 255, 255));
    HPEN oldPen = (HPEN)SelectObject(hdc, dotPen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    
    // Draw white dotted outline
    Rectangle(hdc, rect->left, rect->top, rect->right, rect->bottom);
    
    // Draw shadow for visibility
    HPEN shadowPen = CreatePen(PS_DOT, 1, RGB(0, 0, 0));
    SelectObject(hdc, shadowPen);
    OffsetRect(rect, 1, 1);
    Rectangle(hdc, rect->left, rect->top, rect->right, rect->bottom);
    OffsetRect(rect, -1, -1);
    
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(dotPen);
    DeleteObject(shadowPen);
}

// Update crosshair position
static void UpdateCrosshair(int x, int y) {
    if (!g_crosshairWnd) return;
    
    // Get screen bounds to determine corner placement
    RECT screenRect;
    Capture_GetAllMonitorsBounds(&screenRect);
    
    int crossSize = 80;
    int offset = 20;
    int posX, posY;
    
    // Position in opposite quadrant from cursor
    if (x > (screenRect.right - 150)) {
        posX = x - crossSize - offset;
    } else {
        posX = x + offset;
    }
    
    if (y > (screenRect.bottom - 150)) {
        posY = y - crossSize - offset;
    } else {
        posY = y + offset;
    }
    
    SetWindowPos(g_crosshairWnd, HWND_TOPMOST, posX, posY, 
                 crossSize, crossSize, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_crosshairWnd, NULL, FALSE);
}

BOOL Overlay_Create(HINSTANCE hInstance) {
    g_hInstance = hInstance;
    
    // Initialize common controls
    INITCOMMONCONTROLSEX icex = { sizeof(icex), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icex);
    
    // Register overlay window class
    WNDCLASSEXA wcOverlay = {0};
    wcOverlay.cbSize = sizeof(wcOverlay);
    wcOverlay.style = CS_HREDRAW | CS_VREDRAW;
    wcOverlay.lpfnWndProc = OverlayWndProc;
    wcOverlay.hInstance = hInstance;
    wcOverlay.hCursor = LoadCursor(NULL, IDC_CROSS);
    wcOverlay.hbrBackground = NULL; // Transparent
    wcOverlay.lpszClassName = "LWSROverlay";
    RegisterClassExA(&wcOverlay);
    
    // Register control panel class
    WNDCLASSEXA wcControl = {0};
    wcControl.cbSize = sizeof(wcControl);
    wcControl.style = CS_HREDRAW | CS_VREDRAW;
    wcControl.lpfnWndProc = ControlWndProc;
    wcControl.hInstance = hInstance;
    wcControl.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcControl.hbrBackground = (HBRUSH)(COLOR_3DFACE + 1);
    wcControl.lpszClassName = "LWSRControl";
    RegisterClassExA(&wcControl);
    
    // Register settings window class
    WNDCLASSEXA wcSettings = {0};
    wcSettings.cbSize = sizeof(wcSettings);
    wcSettings.style = CS_HREDRAW | CS_VREDRAW;
    wcSettings.lpfnWndProc = SettingsWndProc;
    wcSettings.hInstance = hInstance;
    wcSettings.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcSettings.hbrBackground = (HBRUSH)(COLOR_3DFACE + 1);
    wcSettings.lpszClassName = "LWSRSettings";
    RegisterClassExA(&wcSettings);
    
    // Register crosshair window class
    WNDCLASSEXA wcCross = {0};
    wcCross.cbSize = sizeof(wcCross);
    wcCross.style = CS_HREDRAW | CS_VREDRAW;
    wcCross.lpfnWndProc = CrosshairWndProc;
    wcCross.hInstance = hInstance;
    wcCross.hCursor = LoadCursor(NULL, IDC_CROSS);
    wcCross.hbrBackground = NULL;
    wcCross.lpszClassName = "LWSRCrosshair";
    RegisterClassExA(&wcCross);
    
    // Get virtual screen bounds (all monitors)
    int vsX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vsY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vsW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vsH = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    
    // Create overlay window (fullscreen, transparent, layered)
    g_overlayWnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW,
        "LWSROverlay",
        NULL,
        WS_POPUP,
        vsX, vsY, vsW, vsH,
        NULL, NULL, hInstance, NULL
    );
    
    if (!g_overlayWnd) return FALSE;
    
    // Set layered window for transparency
    SetLayeredWindowAttributes(g_overlayWnd, RGB(0, 0, 0), 1, LWA_COLORKEY);
    
    // Create control panel (top center) - Windows 11 Snipping Tool style
    POINT center;
    GetPrimaryMonitorCenter(&center);
    
    int ctrlWidth = 680;
    int ctrlHeight = 44;
    
    g_controlWnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        "LWSRControl",
        NULL,
        WS_POPUP | WS_VISIBLE,
        center.x - ctrlWidth / 2, center.y - ctrlHeight / 2,
        ctrlWidth, ctrlHeight,
        NULL, NULL, hInstance, NULL
    );
    
    if (!g_controlWnd) {
        DestroyWindow(g_overlayWnd);
        return FALSE;
    }
    
    // Create crosshair indicator window
    g_crosshairWnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW,
        "LWSRCrosshair",
        NULL,
        WS_POPUP,
        0, 0, 80, 80,
        NULL, NULL, hInstance, NULL
    );
    
    SetLayeredWindowAttributes(g_crosshairWnd, RGB(0, 0, 0), 200, LWA_ALPHA);
    
    g_isSelecting = TRUE;
    ShowWindow(g_overlayWnd, SW_SHOW);
    UpdateWindow(g_overlayWnd);
    UpdateWindow(g_controlWnd);
    
    return TRUE;
}

void Overlay_Destroy(void) {
    if (g_isRecording) {
        Recording_Stop();
    }
    
    if (g_crosshairWnd) {
        DestroyWindow(g_crosshairWnd);
        g_crosshairWnd = NULL;
    }
    
    if (g_settingsWnd) {
        DestroyWindow(g_settingsWnd);
        g_settingsWnd = NULL;
    }
    
    if (g_controlWnd) {
        DestroyWindow(g_controlWnd);
        g_controlWnd = NULL;
    }
    
    if (g_overlayWnd) {
        DestroyWindow(g_overlayWnd);
        g_overlayWnd = NULL;
    }
}

void Overlay_SetMode(CaptureMode mode) {
    g_currentMode = mode;
    g_isSelecting = TRUE;
    SetRectEmpty(&g_selectedRect);
    
    // Update cursor and show/hide appropriate UI
    if (mode == MODE_AREA) {
        // Show overlay for area selection with crosshair cursor
        ShowWindow(g_overlayWnd, SW_SHOW);
        // Use SetSystemCursor for persistent crosshair
        HCURSOR crossCursor = LoadCursor(NULL, IDC_CROSS);
        SetCursor(crossCursor);
    } else {
        // For other modes, still show overlay but with hand cursor
        ShowWindow(g_overlayWnd, SW_SHOW);
        SetCursor(LoadCursor(NULL, IDC_HAND));
    }
    
    InvalidateRect(g_overlayWnd, NULL, TRUE);
}

BOOL Overlay_GetSelectedRegion(RECT* region) {
    if (IsRectEmpty(&g_selectedRect)) return FALSE;
    *region = g_selectedRect;
    return TRUE;
}

HWND Overlay_GetWindow(void) {
    return g_overlayWnd;
}

void Recording_Start(void) {
    if (g_isRecording) return;
    if (IsRectEmpty(&g_selectedRect)) return;
    
    // Set capture region
    if (!Capture_SetRegion(&g_capture, g_selectedRect)) {
        MessageBoxA(NULL, "Failed to set capture region", "Error", MB_OK | MB_ICONERROR);
        return;
    }
    
    // Validate dimensions
    if (g_capture.captureWidth < 16 || g_capture.captureHeight < 16) {
        MessageBoxA(NULL, "Capture area too small", "Error", MB_OK | MB_ICONERROR);
        return;
    }
    
    // Generate output filename
    char outputPath[MAX_PATH];
    Encoder_GenerateFilename(outputPath, MAX_PATH, 
                             g_config.savePath, g_config.outputFormat);
    
    // Initialize encoder
    int fps = Capture_GetRefreshRate(&g_capture);
    if (fps > 60) fps = 60; // Cap at 60 FPS for encoder compatibility
    if (!Encoder_Init(&g_encoder, outputPath, 
                      g_capture.captureWidth, g_capture.captureHeight,
                      fps, g_config.outputFormat, g_config.quality)) {
        char errMsg[512];
        snprintf(errMsg, sizeof(errMsg), 
            "Failed to initialize encoder.\nPath: %s\nSize: %dx%d\nFPS: %d",
            outputPath, g_capture.captureWidth, g_capture.captureHeight, fps);
        MessageBoxA(NULL, errMsg, "Error", MB_OK | MB_ICONERROR);
        return;
    }
    
    // Hide selection UI
    ShowWindow(g_overlayWnd, SW_HIDE);
    ShowWindow(g_crosshairWnd, SW_HIDE);
    
    // Start recording
    g_isRecording = TRUE;
    g_isSelecting = FALSE;
    g_stopRecording = FALSE;
    
    // Update control panel
    Overlay_SetRecordingState(TRUE);
    
    // Start recording thread
    g_recordThread = CreateThread(NULL, 0, RecordingThread, NULL, 0, NULL);
    
    // Start time limit timer if configured
    if (g_config.maxRecordingSeconds > 0) {
        SetTimer(g_controlWnd, ID_TIMER_LIMIT, 
                 g_config.maxRecordingSeconds * 1000, NULL);
    }
}

void Recording_Stop(void) {
    if (!g_isRecording) return;
    
    g_stopRecording = TRUE;
    
    // Wait for recording thread
    if (g_recordThread) {
        WaitForSingleObject(g_recordThread, 5000);
        CloseHandle(g_recordThread);
        g_recordThread = NULL;
    }
    
    // Finalize encoder
    Encoder_Finalize(&g_encoder);
    
    g_isRecording = FALSE;
    
    KillTimer(g_controlWnd, ID_TIMER_LIMIT);
    
    // Save config with last capture rect
    g_config.lastCaptureRect = g_selectedRect;
    g_config.lastMode = g_currentMode;
    Config_Save(&g_config);
    
    // Exit application
    PostQuitMessage(0);
}

static DWORD WINAPI RecordingThread(LPVOID param) {
    (void)param;
    
    LARGE_INTEGER freq, start, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    
    int fps = Capture_GetRefreshRate(&g_capture);
    double frameInterval = 1.0 / fps;
    double nextFrameTime = 0;
    
    while (!g_stopRecording) {
        QueryPerformanceCounter(&now);
        double elapsed = (double)(now.QuadPart - start.QuadPart) / freq.QuadPart;
        
        if (elapsed >= nextFrameTime) {
            UINT64 timestamp;
            BYTE* frame = Capture_GetFrame(&g_capture, &timestamp);
            
            if (frame) {
                Encoder_WriteFrame(&g_encoder, frame, timestamp);
            }
            
            nextFrameTime += frameInterval;
            
            // Don't let it fall too far behind
            if (nextFrameTime < elapsed - frameInterval) {
                nextFrameTime = elapsed;
            }
        } else {
            // Sleep until next frame time
            double sleepTime = (nextFrameTime - elapsed) * 1000;
            if (sleepTime > 1) {
                Sleep((DWORD)(sleepTime - 1));
            }
        }
    }
    
    return 0;
}

void Overlay_SetRecordingState(BOOL isRecording) {
    HWND btnStop = GetDlgItem(g_controlWnd, ID_BTN_STOP);
    
    if (isRecording) {
        // Hide mode buttons, show stop button
        ShowWindow(GetDlgItem(g_controlWnd, ID_MODE_AREA), SW_HIDE);
        ShowWindow(GetDlgItem(g_controlWnd, ID_MODE_WINDOW), SW_HIDE);
        ShowWindow(GetDlgItem(g_controlWnd, ID_MODE_MONITOR), SW_HIDE);
        ShowWindow(GetDlgItem(g_controlWnd, ID_MODE_ALL), SW_HIDE);
        ShowWindow(GetDlgItem(g_controlWnd, ID_BTN_SETTINGS), SW_HIDE);
        
        if (btnStop) {
            ShowWindow(btnStop, SW_SHOW);
            SetWindowTextA(btnStop, "Stop Recording");
        }
        
        // Show recording border if enabled
        if (g_config.showRecordingBorder) {
            // Position control near capture area
            SetWindowPos(g_controlWnd, HWND_TOPMOST,
                         g_selectedRect.left, g_selectedRect.top - 60,
                         200, 40, SWP_SHOWWINDOW);
        }
    } else {
        // Show mode buttons, hide stop button
        ShowWindow(GetDlgItem(g_controlWnd, ID_MODE_AREA), SW_SHOW);
        ShowWindow(GetDlgItem(g_controlWnd, ID_MODE_WINDOW), SW_SHOW);
        ShowWindow(GetDlgItem(g_controlWnd, ID_MODE_MONITOR), SW_SHOW);
        ShowWindow(GetDlgItem(g_controlWnd, ID_MODE_ALL), SW_SHOW);
        ShowWindow(GetDlgItem(g_controlWnd, ID_BTN_SETTINGS), SW_SHOW);
        
        if (btnStop) {
            ShowWindow(btnStop, SW_HIDE);
        }
    }
}

// Overlay window procedure - handles selection
static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_USER + 1: // Stop recording signal from second instance
            if (g_isRecording) {
                Recording_Stop();
            } else {
                PostQuitMessage(0);
            }
            return 0;
            
        case WM_LBUTTONDOWN:
            if (g_isSelecting && g_currentMode == MODE_AREA) {
                g_isDragging = TRUE;
                g_dragStart.x = GET_X_LPARAM(lParam);
                g_dragStart.y = GET_Y_LPARAM(lParam);
                g_dragEnd = g_dragStart;
                SetCapture(hwnd);
                
                // Remove transparent flag to receive mouse events
                LONG exStyle = GetWindowLong(hwnd, GWL_EXSTYLE);
                SetWindowLong(hwnd, GWL_EXSTYLE, exStyle & ~WS_EX_TRANSPARENT);
            }
            return 0;
            
        case WM_MOUSEMOVE:
            if (g_isDragging) {
                g_dragEnd.x = GET_X_LPARAM(lParam);
                g_dragEnd.y = GET_Y_LPARAM(lParam);
                
                // Update selection rect
                g_selectedRect.left = min(g_dragStart.x, g_dragEnd.x);
                g_selectedRect.top = min(g_dragStart.y, g_dragEnd.y);
                g_selectedRect.right = max(g_dragStart.x, g_dragEnd.x);
                g_selectedRect.bottom = max(g_dragStart.y, g_dragEnd.y);
                
                InvalidateRect(hwnd, NULL, TRUE);
                UpdateCrosshair(g_dragEnd.x, g_dragEnd.y);
            } else if (g_isSelecting) {
                POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                UpdateCrosshair(pt.x, pt.y);
            }
            return 0;
            
        case WM_LBUTTONUP:
            if (g_isDragging) {
                g_isDragging = FALSE;
                ReleaseCapture();
                
                // Restore transparent flag
                LONG exStyle = GetWindowLong(hwnd, GWL_EXSTYLE);
                SetWindowLong(hwnd, GWL_EXSTYLE, exStyle | WS_EX_TRANSPARENT);
                
                // If valid selection, start recording
                int width = g_selectedRect.right - g_selectedRect.left;
                int height = g_selectedRect.bottom - g_selectedRect.top;
                
                if (width >= 10 && height >= 10) {
                    Recording_Start();
                }
            } else if (g_isSelecting) {
                // Window/Monitor click mode
                POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                
                if (g_currentMode == MODE_WINDOW) {
                    HWND targetWnd = WindowFromPoint(pt);
                    if (targetWnd) {
                        // Get top-level window
                        HWND topLevel = GetAncestor(targetWnd, GA_ROOT);
                        if (topLevel) {
                            Capture_GetWindowRect(topLevel, &g_selectedRect);
                            Recording_Start();
                        }
                    }
                } else if (g_currentMode == MODE_MONITOR) {
                    RECT monRect;
                    int monIndex;
                    if (Capture_GetMonitorFromPoint(pt, &monRect, &monIndex)) {
                        g_selectedRect = monRect;
                        Recording_Start();
                    }
                } else if (g_currentMode == MODE_ALL_MONITORS) {
                    Capture_GetAllMonitorsBounds(&g_selectedRect);
                    Recording_Start();
                }
            }
            return 0;
            
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            
            // Fill with transparent color
            RECT clientRect;
            GetClientRect(hwnd, &clientRect);
            HBRUSH blackBrush = CreateSolidBrush(RGB(0, 0, 0));
            FillRect(hdc, &clientRect, blackBrush);
            DeleteObject(blackBrush);
            
            // Draw selection if dragging
            if (g_isDragging && !IsRectEmpty(&g_selectedRect)) {
                RECT drawRect = g_selectedRect;
                DrawSelectionRect(hdc, &drawRect);
                
                // Draw size indicator
                char sizeText[64];
                int w = g_selectedRect.right - g_selectedRect.left;
                int h = g_selectedRect.bottom - g_selectedRect.top;
                sprintf(sizeText, "%d x %d", w, h);
                
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, RGB(255, 255, 255));
                TextOutA(hdc, g_selectedRect.right + 5, g_selectedRect.bottom + 5, 
                         sizeText, (int)strlen(sizeText));
            }
            
            EndPaint(hwnd, &ps);
            return 0;
        }
        
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                if (g_isDragging) {
                    g_isDragging = FALSE;
                    ReleaseCapture();
                    SetRectEmpty(&g_selectedRect);
                    InvalidateRect(hwnd, NULL, TRUE);
                } else {
                    PostQuitMessage(0);
                }
            }
            return 0;
            
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// Track which mode button is hovered/selected
static int g_hoveredButton = 0;
static HFONT g_uiFont = NULL;
static HFONT g_iconFont = NULL;

// Control panel window procedure
static LRESULT CALLBACK ControlWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            // Create UI fonts
            g_uiFont = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            g_iconFont = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI Symbol");
            
            // Create mode buttons (owner-drawn for Snipping Tool style)
            CreateWindowW(L"BUTTON", L"Capture Area",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                8, 7, 120, 30, hwnd, (HMENU)ID_MODE_AREA, g_hInstance, NULL);
            
            CreateWindowW(L"BUTTON", L"Capture Window",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                132, 7, 130, 30, hwnd, (HMENU)ID_MODE_WINDOW, g_hInstance, NULL);
            
            CreateWindowW(L"BUTTON", L"Capture Monitor",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                266, 7, 135, 30, hwnd, (HMENU)ID_MODE_MONITOR, g_hInstance, NULL);
            
            CreateWindowW(L"BUTTON", L"Capture All Monitors",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                405, 7, 160, 30, hwnd, (HMENU)ID_MODE_ALL, g_hInstance, NULL);
            
            // Separator line (drawn in WM_PAINT)
            
            // Close button (right side)
            CreateWindowW(L"BUTTON", L"\\u2715",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                644, 10, 28, 24, hwnd, (HMENU)ID_BTN_CLOSE, g_hInstance, NULL);
            
            // Settings button (gear icon area)
            CreateWindowW(L"BUTTON", L"...",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                574, 10, 28, 24, hwnd, (HMENU)ID_BTN_SETTINGS, g_hInstance, NULL);
            
            // Record button
            CreateWindowW(L"BUTTON", L"",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                609, 10, 28, 24, hwnd, (HMENU)ID_BTN_RECORD, g_hInstance, NULL);
            
            // Default to Area mode
            g_currentMode = MODE_AREA;
            
            return 0;
        }
        
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case ID_MODE_AREA:
                    Overlay_SetMode(MODE_AREA);
                    // Invalidate all mode buttons
                    InvalidateRect(GetDlgItem(hwnd, ID_MODE_AREA), NULL, TRUE);
                    InvalidateRect(GetDlgItem(hwnd, ID_MODE_WINDOW), NULL, TRUE);
                    InvalidateRect(GetDlgItem(hwnd, ID_MODE_MONITOR), NULL, TRUE);
                    InvalidateRect(GetDlgItem(hwnd, ID_MODE_ALL), NULL, TRUE);
                    break;
                case ID_MODE_WINDOW:
                    Overlay_SetMode(MODE_WINDOW);
                    InvalidateRect(GetDlgItem(hwnd, ID_MODE_AREA), NULL, TRUE);
                    InvalidateRect(GetDlgItem(hwnd, ID_MODE_WINDOW), NULL, TRUE);
                    InvalidateRect(GetDlgItem(hwnd, ID_MODE_MONITOR), NULL, TRUE);
                    InvalidateRect(GetDlgItem(hwnd, ID_MODE_ALL), NULL, TRUE);
                    break;
                case ID_MODE_MONITOR:
                    Overlay_SetMode(MODE_MONITOR);
                    InvalidateRect(GetDlgItem(hwnd, ID_MODE_AREA), NULL, TRUE);
                    InvalidateRect(GetDlgItem(hwnd, ID_MODE_WINDOW), NULL, TRUE);
                    InvalidateRect(GetDlgItem(hwnd, ID_MODE_MONITOR), NULL, TRUE);
                    InvalidateRect(GetDlgItem(hwnd, ID_MODE_ALL), NULL, TRUE);
                    break;
                case ID_MODE_ALL:
                    Overlay_SetMode(MODE_ALL_MONITORS);
                    InvalidateRect(GetDlgItem(hwnd, ID_MODE_AREA), NULL, TRUE);
                    InvalidateRect(GetDlgItem(hwnd, ID_MODE_WINDOW), NULL, TRUE);
                    InvalidateRect(GetDlgItem(hwnd, ID_MODE_MONITOR), NULL, TRUE);
                    InvalidateRect(GetDlgItem(hwnd, ID_MODE_ALL), NULL, TRUE);
                    break;
                case ID_BTN_SETTINGS:
                    // Toggle settings window
                    if (g_settingsWnd) {
                        // Close settings if already open
                        DestroyWindow(g_settingsWnd);
                        g_settingsWnd = NULL;
                    } else {
                        // Open settings below control panel, centered
                        RECT ctrlRect;
                        GetWindowRect(hwnd, &ctrlRect);
                        int settingsW = 620;
                        int settingsH = 240;
                        int ctrlCenterX = (ctrlRect.left + ctrlRect.right) / 2;
                        
                        g_settingsWnd = CreateWindowExA(
                            WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                            "LWSRSettings",
                            NULL,
                            WS_POPUP | WS_VISIBLE | WS_BORDER,
                            ctrlCenterX - settingsW / 2, ctrlRect.bottom + 5,
                            settingsW, settingsH,
                            hwnd, NULL, g_hInstance, NULL
                        );
                    }
                    break;
                case ID_BTN_RECORD:
                    // Toggle recording
                    if (g_isRecording) {
                        Recording_Stop();
                    } else {
                        // If no selection, use full primary monitor
                        if (IsRectEmpty(&g_selectedRect)) {
                            HMONITOR hMon = MonitorFromPoint((POINT){0,0}, MONITOR_DEFAULTTOPRIMARY);
                            MONITORINFO mi = { sizeof(mi) };
                            GetMonitorInfo(hMon, &mi);
                            g_selectedRect = mi.rcMonitor;
                        }
                        Recording_Start();
                    }
                    // Redraw button to show state change
                    InvalidateRect(GetDlgItem(hwnd, ID_BTN_RECORD), NULL, TRUE);
                    break;
                case ID_BTN_CLOSE:
                    PostQuitMessage(0);
                    break;
                case ID_BTN_STOP:
                    Recording_Stop();
                    break;
            }
            return 0;
            
        case WM_TIMER:
            if (wParam == ID_TIMER_LIMIT) {
                // Time limit reached
                Recording_Stop();
            }
            return 0;
            
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            
            // Dark background (Windows 11 dark theme)
            RECT rect;
            GetClientRect(hwnd, &rect);
            HBRUSH bgBrush = CreateSolidBrush(RGB(32, 32, 32));
            FillRect(hdc, &rect, bgBrush);
            DeleteObject(bgBrush);
            
            // Draw subtle border
            HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(60, 60, 60));
            HPEN oldPen = SelectObject(hdc, borderPen);
            HBRUSH oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
            RoundRect(hdc, rect.left, rect.top, rect.right, rect.bottom, 8, 8);
            SelectObject(hdc, oldPen);
            SelectObject(hdc, oldBrush);
            DeleteObject(borderPen);
            
            // Draw separator before right buttons
            HPEN sepPen = CreatePen(PS_SOLID, 1, RGB(70, 70, 70));
            SelectObject(hdc, sepPen);
            MoveToEx(hdc, 570, 8, NULL);
            LineTo(hdc, 570, 36);
            DeleteObject(sepPen);
            
            EndPaint(hwnd, &ps);
            return 0;
        }
        
        case WM_DRAWITEM: {
            LPDRAWITEMSTRUCT dis = (LPDRAWITEMSTRUCT)lParam;
            UINT ctlId = dis->CtlID;
            BOOL isSelected = FALSE;
            BOOL isHovered = (dis->itemState & ODS_HOTLIGHT) || (dis->itemState & ODS_FOCUS);
            
            // Check if this is a mode button
            BOOL isModeButton = (ctlId >= ID_MODE_AREA && ctlId <= ID_MODE_ALL);
            
            if (isModeButton) {
                // Check if this mode is selected
                isSelected = (ctlId == ID_MODE_AREA && g_currentMode == MODE_AREA) ||
                            (ctlId == ID_MODE_WINDOW && g_currentMode == MODE_WINDOW) ||
                            (ctlId == ID_MODE_MONITOR && g_currentMode == MODE_MONITOR) ||
                            (ctlId == ID_MODE_ALL && g_currentMode == MODE_ALL_MONITORS);
            }
            
            // Background color
            COLORREF bgColor;
            if (isSelected) {
                bgColor = RGB(0, 95, 184); // Windows blue for selected
            } else if (isHovered || (dis->itemState & ODS_SELECTED)) {
                bgColor = RGB(55, 55, 55); // Hover color
            } else {
                bgColor = RGB(32, 32, 32); // Normal background
            }
            
            // Draw rounded button background
            HBRUSH bgBrush = CreateSolidBrush(bgColor);
            HPEN borderPen;
            if (isSelected) {
                borderPen = CreatePen(PS_SOLID, 1, RGB(0, 120, 215));
            } else {
                borderPen = CreatePen(PS_SOLID, 1, RGB(80, 80, 80));
            }
            
            SelectObject(dis->hDC, bgBrush);
            SelectObject(dis->hDC, borderPen);
            RoundRect(dis->hDC, dis->rcItem.left, dis->rcItem.top, 
                      dis->rcItem.right, dis->rcItem.bottom, 6, 6);
            DeleteObject(bgBrush);
            DeleteObject(borderPen);
            
            // Draw text for mode buttons (no icon, just centered text)
            if (isModeButton) {
                SelectObject(dis->hDC, g_uiFont);
                SetBkMode(dis->hDC, TRANSPARENT);
                SetTextColor(dis->hDC, RGB(255, 255, 255));
                
                // Get button text
                WCHAR text[64];
                GetWindowTextW(dis->hwndItem, text, 64);
                
                // Draw centered text
                RECT textRect = dis->rcItem;
                DrawTextW(dis->hDC, text, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                
                return TRUE;
            }
            
            // Record button
            if (ctlId == ID_BTN_RECORD) {
                int cx = (dis->rcItem.left + dis->rcItem.right) / 2;
                int cy = (dis->rcItem.top + dis->rcItem.bottom) / 2;
                
                if (g_isRecording) {
                    // White square (stop icon)
                    HBRUSH iconBrush = CreateSolidBrush(RGB(255, 255, 255));
                    RECT stopRect = { cx - 4, cy - 4, cx + 4, cy + 4 };
                    FillRect(dis->hDC, &stopRect, iconBrush);
                    DeleteObject(iconBrush);
                } else {
                    // Red filled circle (record icon) - use Unicode for smoother look
                    SelectObject(dis->hDC, g_iconFont);
                    SetBkMode(dis->hDC, TRANSPARENT);
                    SetTextColor(dis->hDC, RGB(220, 50, 50));
                    RECT textRect = dis->rcItem;
                    // Use a large filled circle character
                    DrawTextW(dis->hDC, L"\u2B24", -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                }
                return TRUE;
            }
            
            // Settings button (three horizontal dots)
            if (ctlId == ID_BTN_SETTINGS) {
                SelectObject(dis->hDC, g_uiFont);
                SetBkMode(dis->hDC, TRANSPARENT);
                SetTextColor(dis->hDC, RGB(200, 200, 200));
                RECT textRect = dis->rcItem;
                // Use horizontal ellipsis or three dots
                DrawTextW(dis->hDC, L"\u22EF", -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                return TRUE;
            }
            
            // Close button
            if (ctlId == ID_BTN_CLOSE) {
                SelectObject(dis->hDC, g_iconFont);
                SetBkMode(dis->hDC, TRANSPARENT);
                SetTextColor(dis->hDC, RGB(200, 200, 200));
                RECT textRect = dis->rcItem;
                DrawTextW(dis->hDC, L"\u2715", -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                return TRUE;
            }
            
            break;
        }
        
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            HDC hdcCtrl = (HDC)wParam;
            SetBkMode(hdcCtrl, TRANSPARENT);
            SetTextColor(hdcCtrl, RGB(255, 255, 255));
            static HBRUSH hBrush = NULL;
            if (!hBrush) hBrush = CreateSolidBrush(RGB(32, 32, 32));
            return (LRESULT)hBrush;
        }
        
        case WM_DESTROY:
            if (g_uiFont) DeleteObject(g_uiFont);
            if (g_iconFont) DeleteObject(g_iconFont);
            return 0;
    }
    
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// Settings window procedure
static HFONT g_settingsFont = NULL;
static HFONT g_settingsSmallFont = NULL;
static HBRUSH g_settingsBgBrush = NULL;

static LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            // Create fonts
            g_settingsFont = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            g_settingsSmallFont = CreateFontW(11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            g_settingsBgBrush = CreateSolidBrush(RGB(32, 32, 32));
            
            // Get window width for centering
            RECT clientRect;
            GetClientRect(hwnd, &clientRect);
            int windowW = clientRect.right;
            
            int contentW = 560; // Total content width
            int marginX = (windowW - contentW) / 2; // Center margin
            
            int y = 20;
            int labelX = marginX;
            int labelW = 110;
            int controlX = marginX + labelW + 10;
            int controlW = contentW - labelW - 10;
            int rowH = 38;
            
            // Format dropdown
            HWND lblFormat = CreateWindowW(L"STATIC", L"Output Format", 
                WS_CHILD | WS_VISIBLE,
                labelX, y + 5, labelW, 20, hwnd, NULL, g_hInstance, NULL);
            SendMessage(lblFormat, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            
            HWND cmbFormat = CreateWindowW(L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                controlX, y, controlW, 120, hwnd, (HMENU)ID_CMB_FORMAT, g_hInstance, NULL);
            SendMessage(cmbFormat, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            
            SendMessageW(cmbFormat, CB_ADDSTRING, 0, (LPARAM)L"MP4 (H.264) - Best compatibility");
            SendMessageW(cmbFormat, CB_ADDSTRING, 0, (LPARAM)L"AVI - Legacy format");
            SendMessageW(cmbFormat, CB_ADDSTRING, 0, (LPARAM)L"WMV - Windows Media");
            SendMessage(cmbFormat, CB_SETCURSEL, g_config.outputFormat, 0);
            y += rowH;
            
            // Quality dropdown with descriptions
            HWND lblQuality = CreateWindowW(L"STATIC", L"Quality",
                WS_CHILD | WS_VISIBLE,
                labelX, y + 5, labelW, 20, hwnd, NULL, g_hInstance, NULL);
            SendMessage(lblQuality, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            
            HWND cmbQuality = CreateWindowW(L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                controlX, y, controlW, 120, hwnd, (HMENU)ID_CMB_QUALITY, g_hInstance, NULL);
            SendMessage(cmbQuality, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            
            SendMessageW(cmbQuality, CB_ADDSTRING, 0, (LPARAM)L"Low - Small file, lower clarity");
            SendMessageW(cmbQuality, CB_ADDSTRING, 0, (LPARAM)L"Medium - Balanced quality/size");
            SendMessageW(cmbQuality, CB_ADDSTRING, 0, (LPARAM)L"High - Sharp video, larger file");
            SendMessageW(cmbQuality, CB_ADDSTRING, 0, (LPARAM)L"Lossless - Perfect quality, huge file");
            SendMessage(cmbQuality, CB_SETCURSEL, g_config.quality, 0);
            y += rowH + 8;
            
            // Separator line
            CreateWindowW(L"STATIC", L"",
                WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
                labelX, y, contentW, 2, hwnd, NULL, g_hInstance, NULL);
            y += 14;
            
            // Checkboxes side by side
            HWND chkMouse = CreateWindowW(L"BUTTON", L"Capture mouse cursor",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                labelX, y, 200, 24, hwnd, (HMENU)ID_CHK_MOUSE, g_hInstance, NULL);
            SendMessage(chkMouse, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            CheckDlgButton(hwnd, ID_CHK_MOUSE, g_config.captureMouse ? BST_CHECKED : BST_UNCHECKED);
            
            // Show border checkbox - on the right side
            HWND chkBorder = CreateWindowW(L"BUTTON", L"Show recording border",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                labelX + 280, y, 200, 24, hwnd, (HMENU)ID_CHK_BORDER, g_hInstance, NULL);
            SendMessage(chkBorder, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            CheckDlgButton(hwnd, ID_CHK_BORDER, g_config.showRecordingBorder ? BST_CHECKED : BST_UNCHECKED);
            y += 38;
            
            // Time limit - three dropdowns for hours, minutes, seconds
            HWND lblTime = CreateWindowW(L"STATIC", L"Time limit",
                WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                labelX, y, labelW, 26, hwnd, NULL, g_hInstance, NULL);
            SendMessage(lblTime, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            
            // Calculate time from seconds
            int totalSecs = g_config.maxRecordingSeconds;
            if (totalSecs < 1) totalSecs = 60; // Default to 1 minute minimum
            int hours = totalSecs / 3600;
            int mins = (totalSecs % 3600) / 60;
            int secs = totalSecs % 60;
            
            // Hours dropdown (CBS_DROPDOWNLIST for mouse wheel support)
            HWND cmbHours = CreateWindowW(L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                controlX, y, 55, 300, hwnd, (HMENU)ID_CMB_HOURS, g_hInstance, NULL);
            SendMessage(cmbHours, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            for (int i = 0; i <= 24; i++) {
                WCHAR buf[8]; wsprintfW(buf, L"%d", i);
                SendMessageW(cmbHours, CB_ADDSTRING, 0, (LPARAM)buf);
            }
            SendMessage(cmbHours, CB_SETCURSEL, hours, 0);
            
            HWND lblH = CreateWindowW(L"STATIC", L"h",
                WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                controlX + 58, y, 15, 26, hwnd, NULL, g_hInstance, NULL);
            SendMessage(lblH, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            
            // Minutes dropdown
            HWND cmbMins = CreateWindowW(L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                controlX + 78, y, 55, 300, hwnd, (HMENU)ID_CMB_MINUTES, g_hInstance, NULL);
            SendMessage(cmbMins, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            for (int i = 0; i <= 59; i++) {
                WCHAR buf[8]; wsprintfW(buf, L"%d", i);
                SendMessageW(cmbMins, CB_ADDSTRING, 0, (LPARAM)buf);
            }
            SendMessage(cmbMins, CB_SETCURSEL, mins, 0);
            
            HWND lblM = CreateWindowW(L"STATIC", L"m",
                WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                controlX + 136, y, 18, 26, hwnd, NULL, g_hInstance, NULL);
            SendMessage(lblM, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            
            // Seconds dropdown
            HWND cmbSecs = CreateWindowW(L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                controlX + 158, y, 55, 300, hwnd, (HMENU)ID_CMB_SECONDS, g_hInstance, NULL);
            SendMessage(cmbSecs, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            for (int i = 0; i <= 59; i++) {
                WCHAR buf[8]; wsprintfW(buf, L"%d", i);
                SendMessageW(cmbSecs, CB_ADDSTRING, 0, (LPARAM)buf);
            }
            SendMessage(cmbSecs, CB_SETCURSEL, secs, 0);
            
            HWND lblS = CreateWindowW(L"STATIC", L"s",
                WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                controlX + 216, y, 15, 26, hwnd, NULL, g_hInstance, NULL);
            SendMessage(lblS, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            
            y += rowH;
            
            // Save path - aligned with dropdowns
            HWND lblPath = CreateWindowW(L"STATIC", L"Save to",
                WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                labelX, y + 1, labelW, 22, hwnd, NULL, g_hInstance, NULL);
            SendMessage(lblPath, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            
            // Edit control - height 22 matches font better for vertical centering
            HWND edtPath = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                controlX, y, controlW - 80, 22, hwnd, (HMENU)ID_EDT_PATH, g_hInstance, NULL);
            SendMessage(edtPath, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            SetWindowTextA(edtPath, g_config.savePath);
            
            HWND btnBrowse = CreateWindowW(L"BUTTON", L"Browse",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                controlX + controlW - 72, y, 72, 22, hwnd, (HMENU)ID_BTN_BROWSE, g_hInstance, NULL);
            SendMessage(btnBrowse, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            
            return 0;
        }
        
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            
            RECT rect;
            GetClientRect(hwnd, &rect);
            FillRect(hdc, &rect, g_settingsBgBrush);
            
            EndPaint(hwnd, &ps);
            return 0;
        }
        
        case WM_CTLCOLORSTATIC: {
            HDC hdcStatic = (HDC)wParam;
            SetTextColor(hdcStatic, RGB(220, 220, 220));
            SetBkMode(hdcStatic, TRANSPARENT);
            return (LRESULT)g_settingsBgBrush;
        }
        
        case WM_CTLCOLORBTN: {
            HDC hdcBtn = (HDC)wParam;
            SetTextColor(hdcBtn, RGB(220, 220, 220));
            SetBkMode(hdcBtn, TRANSPARENT);
            return (LRESULT)g_settingsBgBrush;
        }
        
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case ID_CMB_FORMAT:
                    if (HIWORD(wParam) == CBN_SELCHANGE) {
                        g_config.outputFormat = (OutputFormat)SendMessage(
                            GetDlgItem(hwnd, ID_CMB_FORMAT), CB_GETCURSEL, 0, 0);
                    }
                    break;
                    
                case ID_CMB_QUALITY:
                    if (HIWORD(wParam) == CBN_SELCHANGE) {
                        g_config.quality = (QualityPreset)SendMessage(
                            GetDlgItem(hwnd, ID_CMB_QUALITY), CB_GETCURSEL, 0, 0);
                    }
                    break;
                    
                case ID_CHK_MOUSE:
                    g_config.captureMouse = IsDlgButtonChecked(hwnd, ID_CHK_MOUSE) == BST_CHECKED;
                    break;
                    
                case ID_CHK_BORDER:
                    g_config.showRecordingBorder = IsDlgButtonChecked(hwnd, ID_CHK_BORDER) == BST_CHECKED;
                    break;
                    
                case ID_CMB_HOURS:
                case ID_CMB_MINUTES:
                case ID_CMB_SECONDS:
                    if (HIWORD(wParam) == CBN_SELCHANGE) {
                        // Get values from all three dropdowns
                        int hours = (int)SendMessage(GetDlgItem(hwnd, ID_CMB_HOURS), CB_GETCURSEL, 0, 0);
                        int mins = (int)SendMessage(GetDlgItem(hwnd, ID_CMB_MINUTES), CB_GETCURSEL, 0, 0);
                        int secs = (int)SendMessage(GetDlgItem(hwnd, ID_CMB_SECONDS), CB_GETCURSEL, 0, 0);
                        
                        // Calculate total seconds (minimum 1 second)
                        int total = hours * 3600 + mins * 60 + secs;
                        if (total < 1) total = 1;
                        g_config.maxRecordingSeconds = total;
                    }
                    break;
                    
                case ID_BTN_BROWSE: {
                    BROWSEINFOA bi = {0};
                    bi.hwndOwner = hwnd;
                    bi.lpszTitle = "Select Save Folder";
                    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
                    
                    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderA(&bi);
                    if (pidl) {
                        char path[MAX_PATH];
                        if (SHGetPathFromIDListA(pidl, path)) {
                            strncpy(g_config.savePath, path, MAX_PATH - 1);
                            SetWindowTextA(GetDlgItem(hwnd, ID_EDT_PATH), path);
                            CreateDirectoryA(path, NULL);
                        }
                        CoTaskMemFree(pidl);
                    }
                    break;
                }
            }
            return 0;
            
        case WM_CLOSE: {
            // Save time limit from dropdowns
            int hours = (int)SendMessage(GetDlgItem(hwnd, ID_CMB_HOURS), CB_GETCURSEL, 0, 0);
            int mins = (int)SendMessage(GetDlgItem(hwnd, ID_CMB_MINUTES), CB_GETCURSEL, 0, 0);
            int secs = (int)SendMessage(GetDlgItem(hwnd, ID_CMB_SECONDS), CB_GETCURSEL, 0, 0);
            
            int total = hours * 3600 + mins * 60 + secs;
            if (total < 1) total = 1; // Minimum 1 second
            g_config.maxRecordingSeconds = total;
            
            // Save path
            GetWindowTextA(GetDlgItem(hwnd, ID_EDT_PATH), g_config.savePath, MAX_PATH);
            Config_Save(&g_config);
            
            // Clean up
            if (g_settingsFont) { DeleteObject(g_settingsFont); g_settingsFont = NULL; }
            if (g_settingsSmallFont) { DeleteObject(g_settingsSmallFont); g_settingsSmallFont = NULL; }
            if (g_settingsBgBrush) { DeleteObject(g_settingsBgBrush); g_settingsBgBrush = NULL; }
            
            DestroyWindow(hwnd);
            g_settingsWnd = NULL;
            return 0;
        }
        
        case WM_DESTROY:
            if (g_settingsFont) { DeleteObject(g_settingsFont); g_settingsFont = NULL; }
            if (g_settingsSmallFont) { DeleteObject(g_settingsSmallFont); g_settingsSmallFont = NULL; }
            if (g_settingsBgBrush) { DeleteObject(g_settingsBgBrush); g_settingsBgBrush = NULL; }
            return 0;
    }
    
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// Crosshair indicator window procedure
static LRESULT CALLBACK CrosshairWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            
            RECT rect;
            GetClientRect(hwnd, &rect);
            
            // Dark background
            HBRUSH bgBrush = CreateSolidBrush(RGB(30, 30, 30));
            FillRect(hdc, &rect, bgBrush);
            DeleteObject(bgBrush);
            
            // Draw crosshair
            HPEN bluePen = CreatePen(PS_SOLID, 2, RGB(0, 120, 215));
            HPEN oldPen = (HPEN)SelectObject(hdc, bluePen);
            
            int cx = (rect.right - rect.left) / 2;
            int cy = (rect.bottom - rect.top) / 2;
            
            MoveToEx(hdc, cx, 0, NULL);
            LineTo(hdc, cx, rect.bottom);
            MoveToEx(hdc, 0, cy, NULL);
            LineTo(hdc, rect.right, cy);
            
            SelectObject(hdc, oldPen);
            DeleteObject(bluePen);
            
            // Draw size text
            if (!IsRectEmpty(&g_selectedRect)) {
                char sizeText[64];
                int w = g_selectedRect.right - g_selectedRect.left;
                int h = g_selectedRect.bottom - g_selectedRect.top;
                sprintf(sizeText, "%d x %d", w, h);
                
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, RGB(255, 255, 255));
                
                RECT textRect = rect;
                textRect.top = rect.bottom - 20;
                DrawTextA(hdc, sizeText, -1, &textRect, DT_CENTER | DT_VCENTER);
            }
            
            EndPaint(hwnd, &ps);
            return 0;
        }
    }
    
    return DefWindowProc(hwnd, msg, wParam, lParam);
}
