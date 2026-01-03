/*
 * Overlay Implementation
 * Selection UI, recording controls, and main logic
 */

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <dwmapi.h>
#include <mmsystem.h>  // For timeBeginPeriod/timeEndPeriod
#include <stdio.h>

#include "action_toolbar.h"
#include "border.h"
#include "capture.h"

// GDI+ Flat API for anti-aliased drawing
#include <objbase.h>

// GDI+ types and functions (flat API)
typedef struct GdiplusStartupInput {
    UINT32 GdiplusVersion;
    void* DebugEventCallback;
    BOOL SuppressBackgroundThread;
    BOOL SuppressExternalCodecs;
} GdiplusStartupInput;

typedef void* GpGraphics;
typedef void* GpBrush;
typedef void* GpSolidFill;
typedef void* GpPen;
typedef int GpStatus;

// GDI+ function pointers
typedef GpStatus (WINAPI *GdiplusStartupFunc)(ULONG_PTR*, const GdiplusStartupInput*, void*);
typedef void (WINAPI *GdiplusShutdownFunc)(ULONG_PTR);
typedef GpStatus (WINAPI *GdipCreateFromHDCFunc)(HDC, GpGraphics**);
typedef GpStatus (WINAPI *GdipDeleteGraphicsFunc)(GpGraphics*);
typedef GpStatus (WINAPI *GdipSetSmoothingModeFunc)(GpGraphics*, int);
typedef GpStatus (WINAPI *GdipCreateSolidFillFunc)(DWORD, GpSolidFill**);
typedef GpStatus (WINAPI *GdipDeleteBrushFunc)(GpBrush*);
typedef GpStatus (WINAPI *GdipCreatePenFunc)(DWORD, float, int, GpPen**);
typedef GpStatus (WINAPI *GdipDeletePenFunc)(GpPen*);
typedef GpStatus (WINAPI *GdipFillRectangleFunc)(GpGraphics*, GpBrush*, float, float, float, float);
typedef GpStatus (WINAPI *GdipFillEllipseFunc)(GpGraphics*, GpBrush*, float, float, float, float);
typedef GpStatus (WINAPI *GdipDrawRectangleFunc)(GpGraphics*, GpPen*, float, float, float, float);
typedef GpStatus (WINAPI *GdipFillPathFunc)(GpGraphics*, GpBrush*, void*);
typedef GpStatus (WINAPI *GdipDrawPathFunc)(GpGraphics*, GpPen*, void*);
typedef GpStatus (WINAPI *GdipCreatePathFunc)(int, void**);
typedef GpStatus (WINAPI *GdipDeletePathFunc)(void*);
typedef GpStatus (WINAPI *GdipAddPathArcFunc)(void*, float, float, float, float, float, float);
typedef GpStatus (WINAPI *GdipAddPathLineFunc)(void*, float, float, float, float);
typedef GpStatus (WINAPI *GdipClosePathFigureFunc)(void*);
typedef GpStatus (WINAPI *GdipStartPathFigureFunc)(void*);

static HMODULE g_gdiplus = NULL;
static ULONG_PTR g_gdiplusToken = 0;
static GdipCreateFromHDCFunc GdipCreateFromHDC = NULL;
static GdipDeleteGraphicsFunc GdipDeleteGraphics = NULL;
static GdipSetSmoothingModeFunc GdipSetSmoothingMode = NULL;
static GdipCreateSolidFillFunc GdipCreateSolidFill = NULL;
static GdipDeleteBrushFunc GdipDeleteBrush = NULL;
static GdipCreatePenFunc GdipCreatePen1 = NULL;
static GdipDeletePenFunc GdipDeletePen = NULL;
static GdipFillRectangleFunc GdipFillRectangle = NULL;
static GdipFillEllipseFunc GdipFillEllipse = NULL;
static GdipFillPathFunc GdipFillPath = NULL;
static GdipDrawPathFunc GdipDrawPath = NULL;
static GdipCreatePathFunc GdipCreatePath = NULL;
static GdipDeletePathFunc GdipDeletePath = NULL;
static GdipAddPathArcFunc GdipAddPathArc = NULL;
static GdipAddPathLineFunc GdipAddPathLine = NULL;
static GdipClosePathFigureFunc GdipClosePathFigure = NULL;
static GdipStartPathFigureFunc GdipStartPathFigure = NULL;

// Smoothing mode constants
#define SmoothingModeAntiAlias 4
#define UnitPixel 2
#define FillModeAlternate 0

// DWM window corner preference (Windows 11+)
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
typedef enum {
    DWMWCP_DEFAULT = 0,
    DWMWCP_DONOTROUND = 1,
    DWMWCP_ROUND = 2,
    DWMWCP_ROUNDSMALL = 3
} DWM_WINDOW_CORNER_PREFERENCE;

// OCR_NORMAL not defined in some Windows headers
#ifndef OCR_NORMAL
#define OCR_NORMAL 32512
#endif

#include "overlay.h"
#include "capture.h"
#include "encoder.h"
#include "config.h"
#include "replay_buffer.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "dwmapi.lib")

// Hotkey ID for replay save (must match main.c)
#define HOTKEY_REPLAY_SAVE 1

// External globals from main.c
extern AppConfig g_config;
extern CaptureState g_capture;
extern ReplayBufferState g_replayBuffer;
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
#define ID_RECORDING_PANEL 1019  // Inline timer + stop button
#define ID_TIMER_RECORD    2001
#define ID_TIMER_LIMIT     2002
#define ID_TIMER_DISPLAY   2003

// Replay buffer settings control IDs
#define ID_CHK_REPLAY_ENABLED   4001
#define ID_CMB_REPLAY_SOURCE    4002
#define ID_CMB_REPLAY_ASPECT    4003
#define ID_CMB_REPLAY_STORAGE   4004
#define ID_STATIC_REPLAY_INFO   4005
#define ID_BTN_REPLAY_HOTKEY    4006
#define ID_CMB_REPLAY_HOURS     4007
#define ID_CMB_REPLAY_MINS      4008
#define ID_CMB_REPLAY_SECS      4009
#define ID_CMB_REPLAY_FPS       4010
#define ID_STATIC_REPLAY_RAM    4011
#define ID_STATIC_REPLAY_CALC   4012

// Action toolbar button IDs
#define ID_ACTION_RECORD   3001
#define ID_ACTION_COPY     3002
#define ID_ACTION_SAVE     3003
#define ID_ACTION_MARKUP   3004

// Selection states
typedef enum {
    SEL_NONE = 0,
    SEL_DRAWING,      // Currently drawing selection
    SEL_COMPLETE,     // Selection complete, showing handles
    SEL_MOVING,       // Moving the entire selection
    SEL_RESIZING      // Resizing via handle
} SelectionState;

// Resize handle positions
typedef enum {
    HANDLE_NONE = 0,
    HANDLE_TL, HANDLE_T, HANDLE_TR,
    HANDLE_L,           HANDLE_R,
    HANDLE_BL, HANDLE_B, HANDLE_BR
} HandlePosition;

// Window state
static HINSTANCE g_hInstance;
static CaptureMode g_currentMode = MODE_NONE;
static CaptureMode g_recordingMode = MODE_NONE;  // Mode that started recording
static SelectionState g_selState = SEL_NONE;
static HandlePosition g_activeHandle = HANDLE_NONE;
static BOOL g_isDragging = FALSE;
static POINT g_dragStart;
static POINT g_dragEnd;
static POINT g_moveStart;      // For moving selection
static RECT g_selectedRect;
static RECT g_originalRect;    // Original rect before resize/move
static HWND g_settingsWnd = NULL;
static HWND g_crosshairWnd = NULL;
static HWND g_recordingPanel = NULL;  // Inline timer + stop in control bar
static EncoderState g_encoder;
static HANDLE g_recordThread = NULL;
static volatile BOOL g_stopRecording = FALSE;
static DWORD g_recordStartTime = 0;
static BOOL g_recordingPanelHovered = FALSE;  // Hover state for recording panel
static BOOL g_waitingForHotkey = FALSE;       // Waiting for user to press hotkey

// Handle size
#define HANDLE_SIZE 10

// Recording thread
static DWORD WINAPI RecordingThread(LPVOID param);

// Window procedures
static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static LRESULT CALLBACK ControlWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static LRESULT CALLBACK CrosshairWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Helper functions
static HandlePosition HitTestHandle(POINT pt);
static void UpdateActionToolbar(void);
static void UpdateTimerDisplay(void);
static void ShowActionToolbar(BOOL show);
static void CaptureToClipboard(void);
static void CaptureToFile(void);

// Initialize GDI+ for anti-aliased drawing
static BOOL InitGdiPlus(void) {
    g_gdiplus = LoadLibraryW(L"gdiplus.dll");
    if (!g_gdiplus) return FALSE;
    
    GdiplusStartupFunc GdiplusStartup = (GdiplusStartupFunc)GetProcAddress(g_gdiplus, "GdiplusStartup");
    if (!GdiplusStartup) return FALSE;
    
    GdiplusStartupInput input = { 1, NULL, FALSE, FALSE };
    if (GdiplusStartup(&g_gdiplusToken, &input, NULL) != 0) return FALSE;
    
    // Load all needed functions
    GdipCreateFromHDC = (GdipCreateFromHDCFunc)GetProcAddress(g_gdiplus, "GdipCreateFromHDC");
    GdipDeleteGraphics = (GdipDeleteGraphicsFunc)GetProcAddress(g_gdiplus, "GdipDeleteGraphics");
    GdipSetSmoothingMode = (GdipSetSmoothingModeFunc)GetProcAddress(g_gdiplus, "GdipSetSmoothingMode");
    GdipCreateSolidFill = (GdipCreateSolidFillFunc)GetProcAddress(g_gdiplus, "GdipCreateSolidFill");
    GdipDeleteBrush = (GdipDeleteBrushFunc)GetProcAddress(g_gdiplus, "GdipDeleteBrush");
    GdipCreatePen1 = (GdipCreatePenFunc)GetProcAddress(g_gdiplus, "GdipCreatePen1");
    GdipDeletePen = (GdipDeletePenFunc)GetProcAddress(g_gdiplus, "GdipDeletePen");
    GdipFillRectangle = (GdipFillRectangleFunc)GetProcAddress(g_gdiplus, "GdipFillRectangle");
    GdipFillEllipse = (GdipFillEllipseFunc)GetProcAddress(g_gdiplus, "GdipFillEllipse");
    GdipFillPath = (GdipFillPathFunc)GetProcAddress(g_gdiplus, "GdipFillPath");
    GdipDrawPath = (GdipDrawPathFunc)GetProcAddress(g_gdiplus, "GdipDrawPath");
    GdipCreatePath = (GdipCreatePathFunc)GetProcAddress(g_gdiplus, "GdipCreatePath");
    GdipDeletePath = (GdipDeletePathFunc)GetProcAddress(g_gdiplus, "GdipDeletePath");
    GdipAddPathArc = (GdipAddPathArcFunc)GetProcAddress(g_gdiplus, "GdipAddPathArc");
    GdipAddPathLine = (GdipAddPathLineFunc)GetProcAddress(g_gdiplus, "GdipAddPathLine");
    GdipClosePathFigure = (GdipClosePathFigureFunc)GetProcAddress(g_gdiplus, "GdipClosePathFigure");
    GdipStartPathFigure = (GdipStartPathFigureFunc)GetProcAddress(g_gdiplus, "GdipStartPathFigure");
    
    return TRUE;
}

static void ShutdownGdiPlus(void) {
    if (g_gdiplusToken && g_gdiplus) {
        GdiplusShutdownFunc GdiplusShutdown = (GdiplusShutdownFunc)GetProcAddress(g_gdiplus, "GdiplusShutdown");
        if (GdiplusShutdown) GdiplusShutdown(g_gdiplusToken);
    }
    if (g_gdiplus) FreeLibrary(g_gdiplus);
    g_gdiplus = NULL;
    g_gdiplusToken = 0;
}

// Convert COLORREF to ARGB for GDI+
static DWORD ColorRefToARGB(COLORREF cr, BYTE alpha) {
    return ((DWORD)alpha << 24) | 
           ((DWORD)GetRValue(cr) << 16) | 
           ((DWORD)GetGValue(cr) << 8) | 
           (DWORD)GetBValue(cr);
}

// Draw anti-aliased filled rounded rectangle
static void DrawRoundedRectAA(HDC hdc, RECT* rect, int radius, COLORREF fillColor, COLORREF borderColor) {
    if (!GdipCreateFromHDC) return;
    
    GpGraphics* graphics = NULL;
    if (GdipCreateFromHDC(hdc, &graphics) != 0) return;
    
    GdipSetSmoothingMode(graphics, SmoothingModeAntiAlias);
    
    // Inset by 0.5 to ensure border is fully visible (GDI+ draws strokes centered on path)
    float x = (float)rect->left + 0.5f;
    float y = (float)rect->top + 0.5f;
    float w = (float)(rect->right - rect->left) - 1.0f;
    float h = (float)(rect->bottom - rect->top) - 1.0f;
    float r = (float)radius;
    float d = r * 2.0f;
    
    // Create rounded rectangle path
    void* path = NULL;
    GdipCreatePath(FillModeAlternate, &path);
    
    // Top-left arc
    GdipAddPathArc(path, x, y, d, d, 180.0f, 90.0f);
    // Top-right arc
    GdipAddPathArc(path, x + w - d, y, d, d, 270.0f, 90.0f);
    // Bottom-right arc
    GdipAddPathArc(path, x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    // Bottom-left arc
    GdipAddPathArc(path, x, y + h - d, d, d, 90.0f, 90.0f);
    GdipClosePathFigure(path);
    
    // Fill
    GpSolidFill* brush = NULL;
    GdipCreateSolidFill(ColorRefToARGB(fillColor, 255), &brush);
    GdipFillPath(graphics, brush, path);
    GdipDeleteBrush(brush);
    
    // Border
    GpPen* pen = NULL;
    GdipCreatePen1(ColorRefToARGB(borderColor, 255), 1.0f, UnitPixel, &pen);
    GdipDrawPath(graphics, pen, path);
    GdipDeletePen(pen);
    
    GdipDeletePath(path);
    GdipDeleteGraphics(graphics);
}

// Draw anti-aliased filled circle
static void DrawCircleAA(HDC hdc, int cx, int cy, int radius, COLORREF color) {
    if (!GdipCreateFromHDC) return;
    
    GpGraphics* graphics = NULL;
    if (GdipCreateFromHDC(hdc, &graphics) != 0) return;
    
    GdipSetSmoothingMode(graphics, SmoothingModeAntiAlias);
    
    GpSolidFill* brush = NULL;
    GdipCreateSolidFill(ColorRefToARGB(color, 255), &brush);
    GdipFillEllipse(graphics, brush, 
                    (float)(cx - radius), (float)(cy - radius), 
                    (float)(radius * 2), (float)(radius * 2));
    GdipDeleteBrush(brush);
    GdipDeleteGraphics(graphics);
}

// Apply smooth rounded corners using DWM (Windows 11+)
static void ApplyRoundedCorners(HWND hwnd) {
    DWM_WINDOW_CORNER_PREFERENCE pref = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));
}

// Helper to get primary monitor center position
static void GetPrimaryMonitorCenter(POINT* pt) {
    HMONITOR hMon = MonitorFromPoint((POINT){0,0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfo(hMon, &mi);
    pt->x = (mi.rcMonitor.left + mi.rcMonitor.right) / 2;
    pt->y = mi.rcMonitor.top + 80; // Near top
}

// Draw dotted selection rectangle on a DC
static void DrawSelectionBorder(HDC hdc, RECT* rect) {
    // White dotted line
    HPEN whitePen = CreatePen(PS_DOT, 1, RGB(255, 255, 255));
    HPEN oldPen = (HPEN)SelectObject(hdc, whitePen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    
    Rectangle(hdc, rect->left, rect->top, rect->right, rect->bottom);
    
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(whitePen);
}

// Update layered window with dark overlay and clear selection hole
static void UpdateOverlayBitmap(void) {
    if (!g_overlayWnd) return;
    
    RECT wndRect;
    GetWindowRect(g_overlayWnd, &wndRect);
    int width = wndRect.right - wndRect.left;
    int height = wndRect.bottom - wndRect.top;
    
    // Create 32-bit DIB for per-pixel alpha
    HDC screenDC = GetDC(NULL);
    HDC memDC = CreateCompatibleDC(screenDC);
    
    BITMAPINFO bmi = {0};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height; // Top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    
    BYTE* pBits = NULL;
    HBITMAP hBitmap = CreateDIBSection(screenDC, &bmi, DIB_RGB_COLORS, (void**)&pBits, NULL, 0);
    HBITMAP oldBitmap = (HBITMAP)SelectObject(memDC, hBitmap);
    
    // Fill entire overlay with semi-transparent dark (alpha ~100 out of 255)
    int overlayAlpha = 100;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            BYTE* pixel = pBits + (y * width + x) * 4;
            pixel[0] = 0;   // B
            pixel[1] = 0;   // G  
            pixel[2] = 0;   // R
            pixel[3] = overlayAlpha; // A
        }
    }
    
    // If we have a selection (drawing or complete), punch a clear hole
    BOOL hasSelection = !IsRectEmpty(&g_selectedRect) && 
                        (g_selState == SEL_DRAWING || g_selState == SEL_COMPLETE || 
                         g_selState == SEL_MOVING || g_selState == SEL_RESIZING);
    
    if (hasSelection) {
        // Convert screen coords to window coords
        int selLeft = g_selectedRect.left - wndRect.left;
        int selTop = g_selectedRect.top - wndRect.top;
        int selRight = g_selectedRect.right - wndRect.left;
        int selBottom = g_selectedRect.bottom - wndRect.top;
        
        // Clamp to window bounds
        if (selLeft < 0) selLeft = 0;
        if (selTop < 0) selTop = 0;
        if (selRight > width) selRight = width;
        if (selBottom > height) selBottom = height;
        
        // Clear the selection area (fully transparent)
        for (int y = selTop; y < selBottom; y++) {
            for (int x = selLeft; x < selRight; x++) {
                BYTE* pixel = pBits + (y * width + x) * 4;
                pixel[0] = 0;
                pixel[1] = 0;
                pixel[2] = 0;
                pixel[3] = 0; // Fully transparent
            }
        }
        
        // Draw white dotted border around selection
        RECT borderRect = { selLeft, selTop, selRight, selBottom };
        DrawSelectionBorder(memDC, &borderRect);
        
        // Draw resize handles when selection is complete
        if (g_selState == SEL_COMPLETE || g_selState == SEL_MOVING || g_selState == SEL_RESIZING) {
            HBRUSH whiteBrush = CreateSolidBrush(RGB(255, 255, 255));
            HBRUSH oldBrush = (HBRUSH)SelectObject(memDC, whiteBrush);
            HPEN whitePen = CreatePen(PS_SOLID, 1, RGB(200, 200, 200));
            HPEN oldPen = (HPEN)SelectObject(memDC, whitePen);
            
            int cx = (selLeft + selRight) / 2;
            int cy = (selTop + selBottom) / 2;
            int hs = HANDLE_SIZE / 2;
            
            // Corner handles
            Ellipse(memDC, selLeft - hs, selTop - hs, selLeft + hs, selTop + hs);           // TL
            Ellipse(memDC, selRight - hs, selTop - hs, selRight + hs, selTop + hs);         // TR
            Ellipse(memDC, selLeft - hs, selBottom - hs, selLeft + hs, selBottom + hs);     // BL
            Ellipse(memDC, selRight - hs, selBottom - hs, selRight + hs, selBottom + hs);   // BR
            
            // Edge handles
            Ellipse(memDC, cx - hs, selTop - hs, cx + hs, selTop + hs);                     // T
            Ellipse(memDC, cx - hs, selBottom - hs, cx + hs, selBottom + hs);               // B
            Ellipse(memDC, selLeft - hs, cy - hs, selLeft + hs, cy + hs);                   // L
            Ellipse(memDC, selRight - hs, cy - hs, selRight + hs, cy + hs);                 // R
            
            SelectObject(memDC, oldBrush);
            SelectObject(memDC, oldPen);
            DeleteObject(whiteBrush);
            DeleteObject(whitePen);
        }
    }
    
    // Apply to layered window
    POINT ptSrc = {0, 0};
    POINT ptDst = { wndRect.left, wndRect.top };
    SIZE sizeWnd = { width, height };
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    
    UpdateLayeredWindow(g_overlayWnd, screenDC, &ptDst, &sizeWnd, memDC, &ptSrc, 0, &blend, ULW_ALPHA);
    
    SelectObject(memDC, oldBitmap);
    DeleteObject(hBitmap);
    DeleteDC(memDC);
    ReleaseDC(NULL, screenDC);
}

// Update crosshair position - positions the size indicator near cursor
static void UpdateCrosshair(int x, int y) {
    if (!g_crosshairWnd) return;
    if (!IsWindowVisible(g_crosshairWnd)) return;
    
    // Get screen bounds to determine corner placement
    RECT screenRect;
    Capture_GetAllMonitorsBounds(&screenRect);
    
    int crossSize = 80;
    int offset = 20;
    int posX, posY;
    
    // Position near cursor but offset so it doesn't obscure selection
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
                 crossSize, crossSize, SWP_NOACTIVATE);
    InvalidateRect(g_crosshairWnd, NULL, FALSE);
}

// Hit test for resize handles - returns which handle is under the point
static HandlePosition HitTestHandle(POINT pt) {
    if (IsRectEmpty(&g_selectedRect)) return HANDLE_NONE;
    
    int hs = HANDLE_SIZE;
    int cx = (g_selectedRect.left + g_selectedRect.right) / 2;
    int cy = (g_selectedRect.top + g_selectedRect.bottom) / 2;
    
    // Check corner handles first (higher priority)
    RECT handleRect;
    
    // Top-left
    SetRect(&handleRect, g_selectedRect.left - hs, g_selectedRect.top - hs, 
            g_selectedRect.left + hs, g_selectedRect.top + hs);
    if (PtInRect(&handleRect, pt)) return HANDLE_TL;
    
    // Top-right
    SetRect(&handleRect, g_selectedRect.right - hs, g_selectedRect.top - hs,
            g_selectedRect.right + hs, g_selectedRect.top + hs);
    if (PtInRect(&handleRect, pt)) return HANDLE_TR;
    
    // Bottom-left
    SetRect(&handleRect, g_selectedRect.left - hs, g_selectedRect.bottom - hs,
            g_selectedRect.left + hs, g_selectedRect.bottom + hs);
    if (PtInRect(&handleRect, pt)) return HANDLE_BL;
    
    // Bottom-right
    SetRect(&handleRect, g_selectedRect.right - hs, g_selectedRect.bottom - hs,
            g_selectedRect.right + hs, g_selectedRect.bottom + hs);
    if (PtInRect(&handleRect, pt)) return HANDLE_BR;
    
    // Edge handles
    // Top
    SetRect(&handleRect, cx - hs, g_selectedRect.top - hs, cx + hs, g_selectedRect.top + hs);
    if (PtInRect(&handleRect, pt)) return HANDLE_T;
    
    // Bottom
    SetRect(&handleRect, cx - hs, g_selectedRect.bottom - hs, cx + hs, g_selectedRect.bottom + hs);
    if (PtInRect(&handleRect, pt)) return HANDLE_B;
    
    // Left
    SetRect(&handleRect, g_selectedRect.left - hs, cy - hs, g_selectedRect.left + hs, cy + hs);
    if (PtInRect(&handleRect, pt)) return HANDLE_L;
    
    // Right
    SetRect(&handleRect, g_selectedRect.right - hs, cy - hs, g_selectedRect.right + hs, cy + hs);
    if (PtInRect(&handleRect, pt)) return HANDLE_R;
    
    return HANDLE_NONE;
}

// Check if point is inside the selection (for moving)
static BOOL PtInSelection(POINT pt) {
    return PtInRect(&g_selectedRect, pt);
}

// Check if point is on the selection border (for moving with border hover)
static BOOL PtOnSelectionBorder(POINT pt) {
    if (IsRectEmpty(&g_selectedRect)) return FALSE;
    
    int borderWidth = 8; // Width of the border hit zone
    
    // Create outer and inner rects
    RECT outer = g_selectedRect;
    InflateRect(&outer, borderWidth, borderWidth);
    
    RECT inner = g_selectedRect;
    InflateRect(&inner, -borderWidth, -borderWidth);
    
    // Point must be in outer but not in inner
    return PtInRect(&outer, pt) && !PtInRect(&inner, pt);
}

// Get cursor for current handle
static HCURSOR GetHandleCursor(HandlePosition handle) {
    switch (handle) {
        case HANDLE_TL: case HANDLE_BR: return LoadCursor(NULL, IDC_SIZENWSE);
        case HANDLE_TR: case HANDLE_BL: return LoadCursor(NULL, IDC_SIZENESW);
        case HANDLE_T: case HANDLE_B: return LoadCursor(NULL, IDC_SIZENS);
        case HANDLE_L: case HANDLE_R: return LoadCursor(NULL, IDC_SIZEWE);
        default: return LoadCursor(NULL, IDC_ARROW);
    }
}

// Show/hide the action toolbar (uses new action_toolbar module)
static void ShowActionToolbar(BOOL show) {
    if (show && !IsRectEmpty(&g_selectedRect)) {
        int cx = (g_selectedRect.left + g_selectedRect.right) / 2;
        int posY = g_selectedRect.bottom + 10;
        
        // Check if it would go off screen
        RECT screenRect;
        Capture_GetAllMonitorsBounds(&screenRect);
        if (posY + 40 > screenRect.bottom - 20) {
            posY = g_selectedRect.top - 40 - 10;
        }
        
        ActionToolbar_Show(cx, posY);
    } else {
        ActionToolbar_Hide();
    }
}

// Update the action toolbar position
static void UpdateActionToolbar(void) {
    ShowActionToolbar(g_selState == SEL_COMPLETE);
}

// Capture screen region to clipboard
static void CaptureToClipboard(void) {
    if (IsRectEmpty(&g_selectedRect)) return;
    
    int w = g_selectedRect.right - g_selectedRect.left;
    int h = g_selectedRect.bottom - g_selectedRect.top;
    
    // Hide overlay temporarily
    ShowWindow(g_overlayWnd, SW_HIDE);
    ActionToolbar_Hide();
    Sleep(50); // Let windows redraw
    
    HDC screenDC = GetDC(NULL);
    HDC memDC = CreateCompatibleDC(screenDC);
    HBITMAP hBitmap = CreateCompatibleBitmap(screenDC, w, h);
    HBITMAP oldBitmap = (HBITMAP)SelectObject(memDC, hBitmap);
    
    BitBlt(memDC, 0, 0, w, h, screenDC, g_selectedRect.left, g_selectedRect.top, SRCCOPY);
    
    SelectObject(memDC, oldBitmap);
    DeleteDC(memDC);
    ReleaseDC(NULL, screenDC);
    
    // Copy to clipboard
    if (OpenClipboard(NULL)) {
        EmptyClipboard();
        SetClipboardData(CF_BITMAP, hBitmap);
        CloseClipboard();
    } else {
        DeleteObject(hBitmap);
    }
    
    // Clear selection state - overlay stays hidden, show control panel
    g_selState = SEL_NONE;
    SetRectEmpty(&g_selectedRect);
    g_isSelecting = FALSE;
    ShowWindow(g_controlWnd, SW_SHOW);
}

// Capture screen region to file (Save As dialog)
static void CaptureToFile(void) {
    if (IsRectEmpty(&g_selectedRect)) return;
    
    int w = g_selectedRect.right - g_selectedRect.left;
    int h = g_selectedRect.bottom - g_selectedRect.top;
    
    // Hide overlay temporarily
    ShowWindow(g_overlayWnd, SW_HIDE);
    ActionToolbar_Hide();
    Sleep(50);
    
    HDC screenDC = GetDC(NULL);
    HDC memDC = CreateCompatibleDC(screenDC);
    HBITMAP hBitmap = CreateCompatibleBitmap(screenDC, w, h);
    HBITMAP oldBitmap = (HBITMAP)SelectObject(memDC, hBitmap);
    
    BitBlt(memDC, 0, 0, w, h, screenDC, g_selectedRect.left, g_selectedRect.top, SRCCOPY);
    
    SelectObject(memDC, oldBitmap);
    DeleteDC(memDC);
    ReleaseDC(NULL, screenDC);
    
    // Show Save As dialog
    char filename[MAX_PATH] = "capture.png";
    OPENFILENAMEA ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    ofn.lpstrFilter = "PNG Image\0*.png\0All Files\0*.*\0";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_OVERWRITEPROMPT;
    ofn.lpstrDefExt = "png";
    
    if (GetSaveFileNameA(&ofn)) {
        // TODO: Save as PNG (requires GDI+ or other library)
        // For now, just show success message
        MessageBoxA(NULL, "Save functionality requires PNG encoder.\nBitmap captured to clipboard instead.", 
                    "Save", MB_OK | MB_ICONINFORMATION);
        
        // Copy to clipboard as fallback
        if (OpenClipboard(NULL)) {
            EmptyClipboard();
            SetClipboardData(CF_BITMAP, hBitmap);
            CloseClipboard();
        }
    }
    
    DeleteObject(hBitmap);
    
    // Clear selection state - overlay stays hidden, show control panel
    g_selState = SEL_NONE;
    SetRectEmpty(&g_selectedRect);
    g_isSelecting = FALSE;
    ShowWindow(g_controlWnd, SW_SHOW);
}

// Timer text for display
static char g_timerText[32] = "00:00";

// Update timer display text
static void UpdateTimerDisplay(void) {
    if (!g_recordingPanel || !g_isRecording) return;
    
    // Update timer text
    DWORD elapsed = GetTickCount() - g_recordStartTime;
    int secs = (elapsed / 1000) % 60;
    int mins = (elapsed / 60000) % 60;
    int hours = elapsed / 3600000;
    
    if (hours > 0) {
        sprintf(g_timerText, "%d:%02d:%02d", hours, mins, secs);
    } else {
        sprintf(g_timerText, "%02d:%02d", mins, secs);  // MM:SS with leading zero
    }
    
    // Trigger repaint of recording panel
    InvalidateRect(g_recordingPanel, NULL, FALSE);
}

BOOL Overlay_Create(HINSTANCE hInstance) {
    g_hInstance = hInstance;
    
    // Initialize GDI+ for anti-aliased drawing
    InitGdiPlus();
    
    // Initialize common controls (including trackbar)
    INITCOMMONCONTROLSEX icex = { sizeof(icex), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES };
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
    
    // Initialize new action toolbar module
    ActionToolbar_Init(hInstance);
    ActionToolbar_SetCallbacks(Recording_Start, CaptureToClipboard, CaptureToFile, NULL);
    
    // Initialize border module
    Border_Init(hInstance);
    
    // Get virtual screen bounds (all monitors)
    int vsX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vsY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vsW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vsH = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    
    // Create overlay window (fullscreen, layered for per-pixel alpha)
    g_overlayWnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW,
        "LWSROverlay",
        NULL,
        WS_POPUP,
        vsX, vsY, vsW, vsH,
        NULL, NULL, hInstance, NULL
    );
    
    if (!g_overlayWnd) return FALSE;
    
    // Initial overlay bitmap will be set by UpdateOverlayBitmap when mode is selected
    
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
    
    // Apply smooth rounded corners using DWM (Windows 11+)
    ApplyRoundedCorners(g_controlWnd);
    
    // Create crosshair indicator window
    g_crosshairWnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW,
        "LWSRCrosshair",
        NULL,
        WS_POPUP,
        -9999, -9999, 80, 80,  // Start off-screen
        NULL, NULL, hInstance, NULL
    );
    
    SetLayeredWindowAttributes(g_crosshairWnd, RGB(0, 0, 0), 200, LWA_ALPHA);
    
    // Action toolbar is now managed by action_toolbar module
    
    // Don't show overlay or crosshair initially - only when mode is selected
    g_isSelecting = FALSE;
    g_selState = SEL_NONE;
    
    // Only show the control panel at startup
    UpdateWindow(g_controlWnd);
    
    return TRUE;
}

void Overlay_Destroy(void) {
    if (g_isRecording) {
        Recording_Stop();
    }
    
    // Shutdown GDI+
    ShutdownGdiPlus();
    
    if (g_crosshairWnd) {
        DestroyWindow(g_crosshairWnd);
        g_crosshairWnd = NULL;
    }
    
    if (g_settingsWnd) {
        DestroyWindow(g_settingsWnd);
        g_settingsWnd = NULL;
    }
    
    // Shutdown action toolbar module
    ActionToolbar_Shutdown();
    
    if (g_recordingPanel) {
        DestroyWindow(g_recordingPanel);
        g_recordingPanel = NULL;
    }
    
    // Shutdown border module
    Border_Shutdown();
    
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
    g_selState = SEL_NONE;
    SetRectEmpty(&g_selectedRect);
    ShowActionToolbar(FALSE);
    
    // Update overlay based on mode
    if (mode == MODE_AREA || mode == MODE_WINDOW || mode == MODE_MONITOR || mode == MODE_ALL_MONITORS) {
        // Show overlay with dark tint
        UpdateOverlayBitmap();
        ShowWindow(g_overlayWnd, SW_SHOW);
        
        // Make sure control panel stays on top of overlay
        SetWindowPos(g_overlayWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        SetWindowPos(g_controlWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        SetForegroundWindow(g_overlayWnd);
    } else if (mode == MODE_NONE) {
        // Hide overlay
        ShowWindow(g_overlayWnd, SW_HIDE);
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
    
    // Hide selection UI (but keep control bar visible)
    ShowWindow(g_overlayWnd, SW_HIDE);
    ShowWindow(g_crosshairWnd, SW_HIDE);
    ActionToolbar_Hide();
    
    // Start recording
    g_isRecording = TRUE;
    g_isSelecting = FALSE;
    g_stopRecording = FALSE;
    g_recordStartTime = GetTickCount();
    g_recordingMode = g_currentMode;  // Remember which mode started recording
    strcpy(g_timerText, "00:00");
    
    // Show recording border if enabled
    if (g_config.showRecordingBorder) {
        Border_Show(g_selectedRect);
    }
    
    // Update control panel to show inline timer/stop
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
    
    // Hide recording border
    Border_Hide();
    
    // Stop timers
    KillTimer(g_controlWnd, ID_TIMER_DISPLAY);
    KillTimer(g_controlWnd, ID_TIMER_LIMIT);
    
    // Restore control bar to normal state
    Overlay_SetRecordingState(FALSE);
    
    // Save config with last capture rect
    g_config.lastCaptureRect = g_selectedRect;
    g_config.lastMode = g_currentMode;
    Config_Save(&g_config);
    
    // Show control bar
    ShowWindow(g_controlWnd, SW_SHOW);
}

static DWORD WINAPI RecordingThread(LPVOID param) {
    (void)param;
    
    // Request high-resolution timer (1ms instead of 15.6ms)
    timeBeginPeriod(1);
    
    LARGE_INTEGER freq, start, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    
    int fps = Capture_GetRefreshRate(&g_capture);
    if (fps > 60) fps = 60;
    
    // Use frame count for consistent timing
    UINT64 frameCount = 0;
    UINT64 frameDuration100ns = 10000000ULL / fps; // 100-nanosecond units for MF
    double frameIntervalSec = 1.0 / fps;
    
    while (!g_stopRecording) {
        QueryPerformanceCounter(&now);
        double elapsed = (double)(now.QuadPart - start.QuadPart) / freq.QuadPart;
        double targetTime = frameCount * frameIntervalSec;
        
        if (elapsed >= targetTime) {
            // Use frame-based timestamp for smooth playback
            UINT64 timestamp = frameCount * frameDuration100ns;
            BYTE* frame = Capture_GetFrame(&g_capture, NULL); // Ignore DXGI timestamp
            
            if (frame) {
                Encoder_WriteFrame(&g_encoder, frame, timestamp);
            }
            
            frameCount++;
            
            // Skip frames if we're falling behind (drop frames rather than stutter)
            double newElapsed = (double)(now.QuadPart - start.QuadPart) / freq.QuadPart;
            while ((frameCount * frameIntervalSec) < newElapsed - frameIntervalSec) {
                frameCount++; // Skip this frame
            }
        } else {
            // Sleep until next frame time - use high precision sleep
            double sleepTime = (targetTime - elapsed) * 1000.0;
            if (sleepTime > 2.0) {
                Sleep((DWORD)(sleepTime - 1.5));
            } else if (sleepTime > 0.5) {
                Sleep(1);
            }
            // Busy-wait for sub-millisecond precision
        }
    }
    
    timeEndPeriod(1);
    return 0;
}

void Overlay_SetRecordingState(BOOL isRecording) {
    if (isRecording) {
        // Get button positions based on recording mode
        HWND modeBtn = NULL;
        RECT btnRect = {0};
        
        switch (g_recordingMode) {
            case MODE_AREA:
                modeBtn = GetDlgItem(g_controlWnd, ID_MODE_AREA);
                break;
            case MODE_WINDOW:
                modeBtn = GetDlgItem(g_controlWnd, ID_MODE_WINDOW);
                break;
            case MODE_MONITOR:
                modeBtn = GetDlgItem(g_controlWnd, ID_MODE_MONITOR);
                break;
            case MODE_ALL_MONITORS:
                modeBtn = GetDlgItem(g_controlWnd, ID_MODE_ALL);
                break;
            default:
                modeBtn = GetDlgItem(g_controlWnd, ID_MODE_AREA);
                break;
        }
        
        if (modeBtn) {
            GetWindowRect(modeBtn, &btnRect);
            MapWindowPoints(HWND_DESKTOP, g_controlWnd, (LPPOINT)&btnRect, 2);
        }
        
        // Hide the active mode button
        if (modeBtn) ShowWindow(modeBtn, SW_HIDE);
        
        // Disable (gray out) other mode buttons
        HWND btnArea = GetDlgItem(g_controlWnd, ID_MODE_AREA);
        HWND btnWindow = GetDlgItem(g_controlWnd, ID_MODE_WINDOW);
        HWND btnMonitor = GetDlgItem(g_controlWnd, ID_MODE_MONITOR);
        HWND btnAll = GetDlgItem(g_controlWnd, ID_MODE_ALL);
        
        if (btnArea != modeBtn) EnableWindow(btnArea, FALSE);
        if (btnWindow != modeBtn) EnableWindow(btnWindow, FALSE);
        if (btnMonitor != modeBtn) EnableWindow(btnMonitor, FALSE);
        if (btnAll != modeBtn) EnableWindow(btnAll, FALSE);
        
        // Invalidate disabled buttons to show grayed state
        InvalidateRect(btnArea, NULL, TRUE);
        InvalidateRect(btnWindow, NULL, TRUE);
        InvalidateRect(btnMonitor, NULL, TRUE);
        InvalidateRect(btnAll, NULL, TRUE);
        
        // Create recording panel in place of the mode button
        if (!g_recordingPanel) {
            g_recordingPanel = CreateWindowW(L"BUTTON", L"",
                WS_CHILD | BS_OWNERDRAW,
                btnRect.left, btnRect.top, 
                btnRect.right - btnRect.left, btnRect.bottom - btnRect.top,
                g_controlWnd, (HMENU)ID_RECORDING_PANEL, g_hInstance, NULL);
        } else {
            SetWindowPos(g_recordingPanel, NULL,
                btnRect.left, btnRect.top, 
                btnRect.right - btnRect.left, btnRect.bottom - btnRect.top,
                SWP_NOZORDER);
        }
        ShowWindow(g_recordingPanel, SW_SHOW);
        
        // Start timer for display updates
        SetTimer(g_controlWnd, ID_TIMER_DISPLAY, 1000, NULL);
        
        // Keep control bar visible but ensure it's on top
        ShowWindow(g_controlWnd, SW_SHOW);
        SetWindowPos(g_controlWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    } else {
        // Stop timer
        KillTimer(g_controlWnd, ID_TIMER_DISPLAY);
        
        // Hide recording panel
        if (g_recordingPanel) {
            ShowWindow(g_recordingPanel, SW_HIDE);
        }
        
        // Re-enable and show all mode buttons
        HWND btnArea = GetDlgItem(g_controlWnd, ID_MODE_AREA);
        HWND btnWindow = GetDlgItem(g_controlWnd, ID_MODE_WINDOW);
        HWND btnMonitor = GetDlgItem(g_controlWnd, ID_MODE_MONITOR);
        HWND btnAll = GetDlgItem(g_controlWnd, ID_MODE_ALL);
        
        EnableWindow(btnArea, TRUE);
        EnableWindow(btnWindow, TRUE);
        EnableWindow(btnMonitor, TRUE);
        EnableWindow(btnAll, TRUE);
        
        ShowWindow(btnArea, SW_SHOW);
        ShowWindow(btnWindow, SW_SHOW);
        ShowWindow(btnMonitor, SW_SHOW);
        ShowWindow(btnAll, SW_SHOW);
        
        InvalidateRect(btnArea, NULL, TRUE);
        InvalidateRect(btnWindow, NULL, TRUE);
        InvalidateRect(btnMonitor, NULL, TRUE);
        InvalidateRect(btnAll, NULL, TRUE);
        
        g_recordingMode = MODE_NONE;
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
        
        case WM_SETCURSOR: {
            // Cursor depends on state and what's under the mouse
            POINT pt;
            GetCursorPos(&pt);
            
            if (g_selState == SEL_COMPLETE) {
                HandlePosition handle = HitTestHandle(pt);
                if (handle != HANDLE_NONE) {
                    SetCursor(GetHandleCursor(handle));
                    return TRUE;
                }
                // Show move cursor on border OR inside selection
                if (PtOnSelectionBorder(pt) || PtInSelection(pt)) {
                    SetCursor(LoadCursor(NULL, IDC_SIZEALL));
                    return TRUE;
                }
            }
            
            // Default cursor based on mode
            if (g_currentMode == MODE_AREA) {
                SetCursor(LoadCursor(NULL, IDC_CROSS));
            } else if (g_currentMode == MODE_WINDOW || g_currentMode == MODE_MONITOR || g_currentMode == MODE_ALL_MONITORS) {
                SetCursor(LoadCursor(NULL, IDC_HAND));
            } else {
                SetCursor(LoadCursor(NULL, IDC_ARROW));
            }
            return TRUE;
        }
            
        case WM_LBUTTONDOWN: {
            // Convert client coordinates to screen coordinates
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ClientToScreen(hwnd, &pt);
            
            if (g_currentMode == MODE_AREA) {
                if (g_selState == SEL_COMPLETE) {
                    // Check if clicking on a handle
                    HandlePosition handle = HitTestHandle(pt);
                    if (handle != HANDLE_NONE) {
                        g_selState = SEL_RESIZING;
                        g_activeHandle = handle;
                        g_originalRect = g_selectedRect;
                        g_moveStart = pt;
                        SetCapture(hwnd);
                        ShowActionToolbar(FALSE);
                        return 0;
                    }
                    
                    // Check if clicking inside selection OR on border (move)
                    if (PtOnSelectionBorder(pt) || PtInSelection(pt)) {
                        g_selState = SEL_MOVING;
                        g_originalRect = g_selectedRect;
                        g_moveStart = pt;
                        SetCapture(hwnd);
                        ShowActionToolbar(FALSE);
                        return 0;
                    }
                    
                    // Clicking outside - start new selection
                    g_selState = SEL_NONE;
                    SetRectEmpty(&g_selectedRect);
                    ShowActionToolbar(FALSE);
                }
                
                // Start drawing new selection
                g_selState = SEL_DRAWING;
                g_dragStart = pt;
                g_dragEnd = pt;
                SetCapture(hwnd);
            }
            return 0;
        }
            
        case WM_MOUSEMOVE: {
            // Convert client coordinates to screen coordinates
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ClientToScreen(hwnd, &pt);
            
            if (g_selState == SEL_DRAWING) {
                g_dragEnd = pt;
                
                // Update selection rect
                g_selectedRect.left = min(g_dragStart.x, g_dragEnd.x);
                g_selectedRect.top = min(g_dragStart.y, g_dragEnd.y);
                g_selectedRect.right = max(g_dragStart.x, g_dragEnd.x);
                g_selectedRect.bottom = max(g_dragStart.y, g_dragEnd.y);
                
                UpdateOverlayBitmap();
            } else if (g_selState == SEL_MOVING) {
                int dx = pt.x - g_moveStart.x;
                int dy = pt.y - g_moveStart.y;
                
                g_selectedRect.left = g_originalRect.left + dx;
                g_selectedRect.top = g_originalRect.top + dy;
                g_selectedRect.right = g_originalRect.right + dx;
                g_selectedRect.bottom = g_originalRect.bottom + dy;
                
                UpdateOverlayBitmap();
            } else if (g_selState == SEL_RESIZING) {
                int dx = pt.x - g_moveStart.x;
                int dy = pt.y - g_moveStart.y;
                
                g_selectedRect = g_originalRect;
                
                // Apply resize based on active handle
                switch (g_activeHandle) {
                    case HANDLE_TL:
                        g_selectedRect.left += dx;
                        g_selectedRect.top += dy;
                        break;
                    case HANDLE_T:
                        g_selectedRect.top += dy;
                        break;
                    case HANDLE_TR:
                        g_selectedRect.right += dx;
                        g_selectedRect.top += dy;
                        break;
                    case HANDLE_L:
                        g_selectedRect.left += dx;
                        break;
                    case HANDLE_R:
                        g_selectedRect.right += dx;
                        break;
                    case HANDLE_BL:
                        g_selectedRect.left += dx;
                        g_selectedRect.bottom += dy;
                        break;
                    case HANDLE_B:
                        g_selectedRect.bottom += dy;
                        break;
                    case HANDLE_BR:
                        g_selectedRect.right += dx;
                        g_selectedRect.bottom += dy;
                        break;
                    default:
                        break;
                }
                
                // Normalize rect (ensure left < right, top < bottom)
                if (g_selectedRect.left > g_selectedRect.right) {
                    int tmp = g_selectedRect.left;
                    g_selectedRect.left = g_selectedRect.right;
                    g_selectedRect.right = tmp;
                }
                if (g_selectedRect.top > g_selectedRect.bottom) {
                    int tmp = g_selectedRect.top;
                    g_selectedRect.top = g_selectedRect.bottom;
                    g_selectedRect.bottom = tmp;
                }
                
                UpdateOverlayBitmap();
            } else if (g_isSelecting && g_selState == SEL_NONE) {
                // Just moving mouse over overlay, cursor handles itself
            }
            return 0;
        }
            
        case WM_LBUTTONUP: {
            // Convert client coordinates to screen coordinates
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ClientToScreen(hwnd, &pt);
            
            if (g_selState == SEL_DRAWING) {
                ReleaseCapture();
                
                int width = g_selectedRect.right - g_selectedRect.left;
                int height = g_selectedRect.bottom - g_selectedRect.top;
                
                if (width >= 10 && height >= 10) {
                    // Selection complete - show handles and action toolbar
                    g_selState = SEL_COMPLETE;
                    UpdateOverlayBitmap();
                    ShowActionToolbar(TRUE);
                } else {
                    // Too small - reset
                    g_selState = SEL_NONE;
                    SetRectEmpty(&g_selectedRect);
                    UpdateOverlayBitmap();
                }
            } else if (g_selState == SEL_MOVING || g_selState == SEL_RESIZING) {
                ReleaseCapture();
                g_selState = SEL_COMPLETE;
                g_activeHandle = HANDLE_NONE;
                UpdateOverlayBitmap();
                ShowActionToolbar(TRUE);
            } else if (g_isSelecting && g_currentMode != MODE_AREA) {
                // Window/Monitor click mode
                if (g_currentMode == MODE_WINDOW) {
                    HWND targetWnd = WindowFromPoint(pt);
                    if (targetWnd) {
                        HWND topLevel = GetAncestor(targetWnd, GA_ROOT);
                        if (topLevel) {
                            Capture_GetWindowRect(topLevel, &g_selectedRect);
                            g_selState = SEL_COMPLETE;
                            UpdateOverlayBitmap();
                            ShowActionToolbar(TRUE);
                        }
                    }
                } else if (g_currentMode == MODE_MONITOR) {
                    RECT monRect;
                    int monIndex;
                    if (Capture_GetMonitorFromPoint(pt, &monRect, &monIndex)) {
                        g_selectedRect = monRect;
                        g_selState = SEL_COMPLETE;
                        UpdateOverlayBitmap();
                        ShowActionToolbar(TRUE);
                    }
                } else if (g_currentMode == MODE_ALL_MONITORS) {
                    Capture_GetAllMonitorsBounds(&g_selectedRect);
                    g_selState = SEL_COMPLETE;
                    UpdateOverlayBitmap();
                    ShowActionToolbar(TRUE);
                }
            }
            return 0;
        }
            
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            return 0;
        }
        
        case WM_KEYDOWN:
            // Check for configurable cancel key
            if (wParam == (WPARAM)g_config.cancelKey) {
                if (g_isRecording) {
                    Recording_Stop();
                } else if (g_selState == SEL_DRAWING || g_selState == SEL_MOVING || g_selState == SEL_RESIZING) {
                    ReleaseCapture();
                    g_selState = SEL_NONE;
                    SetRectEmpty(&g_selectedRect);
                    UpdateOverlayBitmap();
                    ShowActionToolbar(FALSE);
                } else if (g_selState == SEL_COMPLETE) {
                    // Cancel selection
                    g_selState = SEL_NONE;
                    SetRectEmpty(&g_selectedRect);
                    UpdateOverlayBitmap();
                    ShowActionToolbar(FALSE);
                } else {
                    PostQuitMessage(0);
                }
            } else if (wParam == VK_RETURN && g_selState == SEL_COMPLETE) {
                // Enter key starts recording
                Recording_Start();
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
            // All buttons are 130px wide for consistency
            CreateWindowW(L"BUTTON", L"Capture Area",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                8, 7, 130, 30, hwnd, (HMENU)ID_MODE_AREA, g_hInstance, NULL);
            
            CreateWindowW(L"BUTTON", L"Capture Window",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                142, 7, 130, 30, hwnd, (HMENU)ID_MODE_WINDOW, g_hInstance, NULL);
            
            CreateWindowW(L"BUTTON", L"Capture Monitor",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                276, 7, 130, 30, hwnd, (HMENU)ID_MODE_MONITOR, g_hInstance, NULL);
            
            CreateWindowW(L"BUTTON", L"Capture All Monitors",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                410, 7, 130, 30, hwnd, (HMENU)ID_MODE_ALL, g_hInstance, NULL);
            
            // Small buttons on right side (square 28x28, vertically centered)
            int btnSize = 28;
            int btnY = (44 - btnSize) / 2;  // Center in 44px tall window
            
            // Close button (right side)
            CreateWindowW(L"BUTTON", L"\\u2715",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                644, btnY, btnSize, btnSize, hwnd, (HMENU)ID_BTN_CLOSE, g_hInstance, NULL);
            
            // Settings button (gear icon area)
            CreateWindowW(L"BUTTON", L"...",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                574, btnY, btnSize, btnSize, hwnd, (HMENU)ID_BTN_SETTINGS, g_hInstance, NULL);
            
            // Record button
            CreateWindowW(L"BUTTON", L"",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                609, btnY, btnSize, btnSize, hwnd, (HMENU)ID_BTN_RECORD, g_hInstance, NULL);
            
            // No mode selected by default
            g_currentMode = MODE_NONE;
            
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
                        // Close settings if already open (use WM_CLOSE to trigger cleanup)
                        SendMessage(g_settingsWnd, WM_CLOSE, 0, 0);
                    } else {
                        // Open settings below control panel, centered
                        RECT ctrlRect;
                        GetWindowRect(hwnd, &ctrlRect);
                        int settingsW = 620;
                        int settingsH = 555;  // Height to fit all controls with RAM explanation
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
                    // Stop recording if in progress, then exit
                    if (g_isRecording) {
                        Recording_Stop();
                    }
                    PostQuitMessage(0);
                    break;
                case ID_BTN_STOP:
                    Recording_Stop();
                    break;
                case ID_RECORDING_PANEL:
                    // Click on recording panel stops recording
                    if (g_isRecording) {
                        Recording_Stop();
                    }
                    break;
            }
            return 0;
            
        case WM_TIMER:
            if (wParam == ID_TIMER_LIMIT) {
                // Time limit reached
                Recording_Stop();
            } else if (wParam == ID_TIMER_DISPLAY) {
                // Update timer display
                UpdateTimerDisplay();
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
            COLORREF borderColor;
            if (isSelected) {
                bgColor = RGB(0, 95, 184); // Windows blue for selected
                borderColor = RGB(0, 120, 215);
            } else if (isHovered || (dis->itemState & ODS_SELECTED)) {
                bgColor = RGB(55, 55, 55); // Hover color
                borderColor = RGB(80, 80, 80);
            } else {
                bgColor = RGB(32, 32, 32); // Normal background
                borderColor = RGB(80, 80, 80);
            }
            
            // Draw anti-aliased rounded button background
            DrawRoundedRectAA(dis->hDC, &dis->rcItem, 6, bgColor, borderColor);
            
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
                    // Red filled circle (record icon) - use anti-aliased circle
                    DrawCircleAA(dis->hDC, cx, cy, 6, RGB(220, 50, 50));
                }
                return TRUE;
            }
            
            // Settings button (three horizontal dots)
            if (ctlId == ID_BTN_SETTINGS) {
                SelectObject(dis->hDC, g_uiFont);
                SetBkMode(dis->hDC, TRANSPARENT);
                SetTextColor(dis->hDC, RGB(200, 200, 200));
                RECT textRect = dis->rcItem;
                textRect.left += 1;  // Shift 1 pixel right
                textRect.right += 1;
                // Use horizontal ellipsis or three dots
                DrawTextW(dis->hDC, L"\u22EF", -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                return TRUE;
            }
            
            // Recording panel (inline timer + stop button)
            if (ctlId == ID_RECORDING_PANEL) {
                RECT rect = dis->rcItem;
                int width = rect.right - rect.left;
                int centerX = width / 2;  // Divider at center (50/50 split)
                
                // Check hover state
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(dis->hwndItem, &pt);
                BOOL isHover = PtInRect(&rect, pt);
                
                // Background - slightly lighter on hover
                COLORREF bgColor = isHover ? RGB(48, 48, 48) : RGB(32, 32, 32);
                HBRUSH bgBrush = CreateSolidBrush(bgColor);
                FillRect(dis->hDC, &rect, bgBrush);
                DeleteObject(bgBrush);
                
                // Draw rounded border
                HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(80, 80, 80));
                HPEN oldPen = (HPEN)SelectObject(dis->hDC, borderPen);
                HBRUSH oldBrush = (HBRUSH)SelectObject(dis->hDC, GetStockObject(NULL_BRUSH));
                RoundRect(dis->hDC, rect.left, rect.top, rect.right, rect.bottom, 6, 6);
                SelectObject(dis->hDC, oldPen);
                SelectObject(dis->hDC, oldBrush);
                DeleteObject(borderPen);
                
                // Left half: red dot + timer (centered)
                // Measure timer text width
                SelectObject(dis->hDC, g_uiFont);
                SIZE timerSize;
                GetTextExtentPoint32A(dis->hDC, g_timerText, (int)strlen(g_timerText), &timerSize);
                int dotSize = 8;
                int dotGap = 6;  // Gap between dot and text
                int leftContentWidth = dotSize + dotGap + timerSize.cx;
                int leftStartX = rect.left + (centerX - leftContentWidth) / 2;
                
                // Draw anti-aliased red recording dot using GDI+
                if (GdipCreateFromHDC && GdipSetSmoothingMode && GdipCreateSolidFill && 
                    GdipFillEllipse && GdipDeleteBrush && GdipDeleteGraphics) {
                    GpGraphics* graphics = NULL;
                    GdipCreateFromHDC(dis->hDC, &graphics);
                    if (graphics) {
                        GdipSetSmoothingMode(graphics, 4); // SmoothingModeAntiAlias
                        GpBrush* redBrush = NULL;
                        GdipCreateSolidFill(0xFFEA4335, &redBrush);
                        if (redBrush) {
                            int dotY = (rect.top + rect.bottom - dotSize) / 2;
                            GdipFillEllipse(graphics, redBrush, (float)leftStartX, (float)dotY, (float)dotSize, (float)dotSize);
                            GdipDeleteBrush(redBrush);
                        }
                        GdipDeleteGraphics(graphics);
                    }
                }
                
                // Draw timer text
                SetBkMode(dis->hDC, TRANSPARENT);
                COLORREF textColor = isHover ? RGB(230, 230, 230) : RGB(200, 200, 200);
                SetTextColor(dis->hDC, textColor);
                
                RECT timerRect = rect;
                timerRect.left = leftStartX + dotSize + dotGap;
                timerRect.right = rect.left + centerX;
                DrawTextA(dis->hDC, g_timerText, -1, &timerRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                
                // Draw vertical divider at center
                HPEN dividerPen = CreatePen(PS_SOLID, 1, RGB(80, 80, 80));
                SelectObject(dis->hDC, dividerPen);
                MoveToEx(dis->hDC, rect.left + centerX, rect.top + 6, NULL);
                LineTo(dis->hDC, rect.left + centerX, rect.bottom - 6);
                DeleteObject(dividerPen);
                
                // Right half: red stop square + "Stop" (centered)
                SIZE stopSize;
                GetTextExtentPoint32A(dis->hDC, "Stop", 4, &stopSize);
                int stopSquareSize = 8;
                int stopGap = 6;  // Gap between square and text
                int rightContentWidth = stopSquareSize + stopGap + stopSize.cx;
                int rightStartX = rect.left + centerX + (centerX - rightContentWidth) / 2;
                
                // Draw red stop square
                int stopSquareY = (rect.top + rect.bottom - stopSquareSize) / 2;
                HBRUSH stopBrush = CreateSolidBrush(RGB(234, 67, 53));
                RECT stopSquareRect = { rightStartX, stopSquareY, rightStartX + stopSquareSize, stopSquareY + stopSquareSize };
                FillRect(dis->hDC, &stopSquareRect, stopBrush);
                DeleteObject(stopBrush);
                
                // Draw "Stop" text
                RECT stopTextRect = rect;
                stopTextRect.left = rightStartX + stopSquareSize + stopGap;
                stopTextRect.right = rect.right - 4;
                DrawTextA(dis->hDC, "Stop", -1, &stopTextRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                
                return TRUE;
            }
            
            // Close button
            if (ctlId == ID_BTN_CLOSE) {
                SelectObject(dis->hDC, g_iconFont);
                SetBkMode(dis->hDC, TRANSPARENT);
                SetTextColor(dis->hDC, RGB(200, 200, 200));
                RECT textRect = dis->rcItem;
                textRect.left += 1;  // Shift 1 pixel right
                textRect.right += 1;
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
        
        case WM_HOTKEY: {
            if (wParam == HOTKEY_REPLAY_SAVE) {
                // Check if replay buffer is actually running with frames
                if (!g_replayBuffer.isBuffering) {
                    MessageBeep(MB_ICONWARNING);
                    return 0;
                }
                
                // Generate filename with timestamp
                char filename[MAX_PATH];
                SYSTEMTIME st;
                GetLocalTime(&st);
                sprintf(filename, "%s\\Replay_%04d%02d%02d_%02d%02d%02d.mp4",
                    g_config.savePath, st.wYear, st.wMonth, st.wDay,
                    st.wHour, st.wMinute, st.wSecond);
                
                // Save the replay
                BOOL success = ReplayBuffer_Save(&g_replayBuffer, filename);
                
                // Show notification
                if (success) {
                    MessageBeep(MB_OK);  // Success audio feedback
                } else {
                    // Not enough frames yet or save failed
                    MessageBeep(MB_ICONERROR);
                }
            }
            return 0;
        }
        
        case WM_DESTROY:
            if (g_uiFont) DeleteObject(g_uiFont);
            if (g_iconFont) DeleteObject(g_iconFont);
            return 0;
    }
    
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// Helper function to get key name from virtual key code
static void GetKeyNameFromVK(int vk, char* buffer, int bufferSize) {
    // Handle special keys
    switch (vk) {
        case VK_F1: strcpy(buffer, "F1"); return;
        case VK_F2: strcpy(buffer, "F2"); return;
        case VK_F3: strcpy(buffer, "F3"); return;
        case VK_F4: strcpy(buffer, "F4"); return;
        case VK_F5: strcpy(buffer, "F5"); return;
        case VK_F6: strcpy(buffer, "F6"); return;
        case VK_F7: strcpy(buffer, "F7"); return;
        case VK_F8: strcpy(buffer, "F8"); return;
        case VK_F9: strcpy(buffer, "F9"); return;
        case VK_F10: strcpy(buffer, "F10"); return;
        case VK_F11: strcpy(buffer, "F11"); return;
        case VK_F12: strcpy(buffer, "F12"); return;
        case VK_ESCAPE: strcpy(buffer, "Escape"); return;
        case VK_TAB: strcpy(buffer, "Tab"); return;
        case VK_RETURN: strcpy(buffer, "Enter"); return;
        case VK_SPACE: strcpy(buffer, "Space"); return;
        case VK_BACK: strcpy(buffer, "Backspace"); return;
        case VK_DELETE: strcpy(buffer, "Delete"); return;
        case VK_INSERT: strcpy(buffer, "Insert"); return;
        case VK_HOME: strcpy(buffer, "Home"); return;
        case VK_END: strcpy(buffer, "End"); return;
        case VK_PRIOR: strcpy(buffer, "Page Up"); return;
        case VK_NEXT: strcpy(buffer, "Page Down"); return;
        case VK_LEFT: strcpy(buffer, "Left"); return;
        case VK_RIGHT: strcpy(buffer, "Right"); return;
        case VK_UP: strcpy(buffer, "Up"); return;
        case VK_DOWN: strcpy(buffer, "Down"); return;
        case VK_PAUSE: strcpy(buffer, "Pause"); return;
        case VK_SCROLL: strcpy(buffer, "Scroll Lock"); return;
        case VK_SNAPSHOT: strcpy(buffer, "Print Screen"); return;
        case VK_NUMLOCK: strcpy(buffer, "Num Lock"); return;
        default:
            // For letters and numbers, just use the character
            if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
                buffer[0] = (char)vk;
                buffer[1] = '\0';
                return;
            }
            // Numpad keys
            if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
                sprintf(buffer, "Numpad %d", vk - VK_NUMPAD0);
                return;
            }
            // Default: use scan code
            UINT scanCode = MapVirtualKey(vk, MAPVK_VK_TO_VSC);
            if (GetKeyNameTextA(scanCode << 16, buffer, bufferSize) == 0) {
                sprintf(buffer, "Key 0x%02X", vk);
            }
            break;
    }
}

// Settings window procedure
static HFONT g_settingsFont = NULL;
static HFONT g_settingsSmallFont = NULL;
static HBRUSH g_settingsBgBrush = NULL;

// Helper: Calculate aspect ratio dimensions (returns width:height ratio multiplied by 1000)
static void GetAspectRatioDimensions(int aspectIndex, int* ratioW, int* ratioH) {
    // 0=Native, 1=16:9, 2=9:16, 3=1:1, 4=4:5, 5=16:10, 6=4:3, 7=21:9, 8=32:9
    switch (aspectIndex) {
        case 1: *ratioW = 16; *ratioH = 9; break;   // 16:9 (YouTube, Standard)
        case 2: *ratioW = 9;  *ratioH = 16; break;  // 9:16 (TikTok, Shorts, Reels)
        case 3: *ratioW = 1;  *ratioH = 1; break;   // 1:1 (Square - Instagram)
        case 4: *ratioW = 4;  *ratioH = 5; break;   // 4:5 (Instagram Portrait)
        case 5: *ratioW = 16; *ratioH = 10; break;  // 16:10
        case 6: *ratioW = 4;  *ratioH = 3; break;   // 4:3
        case 7: *ratioW = 21; *ratioH = 9; break;   // 21:9 (Ultrawide)
        case 8: *ratioW = 32; *ratioH = 9; break;   // 32:9 (Super Ultrawide)
        default: *ratioW = 0; *ratioH = 0; break;   // Native
    }
}

// Helper: Calculate aspect ratio rect centered on monitor bounds
static RECT CalculateAspectRect(RECT monBounds, int aspectW, int aspectH) {
    int monW = monBounds.right - monBounds.left;
    int monH = monBounds.bottom - monBounds.top;
    
    int rectW, rectH;
    
    // Fit to monitor while maintaining aspect ratio
    if (monW * aspectH > monH * aspectW) {
        // Monitor is wider than aspect ratio - fit to height
        rectH = monH;
        rectW = (rectH * aspectW) / aspectH;
    } else {
        // Monitor is taller than aspect ratio - fit to width
        rectW = monW;
        rectH = (rectW * aspectH) / aspectW;
    }
    
    // Center on monitor
    RECT result;
    result.left = monBounds.left + (monW - rectW) / 2;
    result.top = monBounds.top + (monH - rectH) / 2;
    result.right = result.left + rectW;
    result.bottom = result.top + rectH;
    
    return result;
}

// Update RAM usage estimate label in settings
static void UpdateReplayRAMEstimate(HWND hwndSettings) {
    HWND lblRam = GetDlgItem(hwndSettings, ID_STATIC_REPLAY_RAM);
    HWND lblCalc = GetDlgItem(hwndSettings, ID_STATIC_REPLAY_CALC);
    if (!lblRam || !lblCalc) return;
    
    int durationSecs = g_config.replayDuration;
    int fps = g_config.replayFPS;
    
    // Get monitor resolution for estimate
    int estWidth = GetSystemMetrics(SM_CXSCREEN);
    int estHeight = GetSystemMetrics(SM_CYSCREEN);
    
    // Adjust for aspect ratio if set
    if (g_config.replayAspectRatio > 0) {
        int ratioW, ratioH;
        GetAspectRatioDimensions(g_config.replayAspectRatio, &ratioW, &ratioH);
        if (ratioW > 0 && ratioH > 0) {
            // Calculate effective resolution with aspect ratio applied
            if (estWidth * ratioH > estHeight * ratioW) {
                // Monitor is wider - fit to height
                estWidth = (estHeight * ratioW) / ratioH;
            } else {
                // Monitor is taller - fit to width
                estHeight = (estWidth * ratioH) / ratioW;
            }
        }
    }
    
    int ramMB = ReplayBuffer_EstimateRAMUsage(durationSecs, estWidth, estHeight, fps);
    
    // Update explanation text
    char explainText[256];
    sprintf(explainText, "When enabled, ~%d MB of RAM is reserved for the video buffer. See the calculation below:", ramMB);
    SetWindowTextA(lblRam, explainText);
    
    // Update calculation text
    char calcText[128];
    if (durationSecs >= 60) {
        int mins = durationSecs / 60;
        int secs = durationSecs % 60;
        if (secs > 0) {
            sprintf(calcText, "%dm %ds @ %d FPS, %dx%d = ~%d MB", mins, secs, fps, estWidth, estHeight, ramMB);
        } else {
            sprintf(calcText, "%dm @ %d FPS, %dx%d = ~%d MB", mins, fps, estWidth, estHeight, ramMB);
        }
    } else {
        sprintf(calcText, "%ds @ %d FPS, %dx%d = ~%d MB", durationSecs, fps, estWidth, estHeight, ramMB);
    }
    SetWindowTextA(lblCalc, calcText);
}

// Update preview border based on current replay capture source
static void UpdateReplayPreview(void) {
    // Hide any existing overlays first
    PreviewBorder_Hide();
    AreaSelector_Hide();
    
    // Show appropriate preview based on capture source
    switch (g_config.replayCaptureSource) {
        case MODE_MONITOR: {
            RECT monBounds;
            if (Capture_GetMonitorBoundsByIndex(g_config.replayMonitorIndex, &monBounds)) {
                // Check if aspect ratio is set (not Native)
                if (g_config.replayAspectRatio > 0) {
                    int ratioW, ratioH;
                    GetAspectRatioDimensions(g_config.replayAspectRatio, &ratioW, &ratioH);
                    
                    // Use saved area rect if valid, otherwise calculate default
                    RECT aspectRect = g_config.replayAreaRect;
                    int areaW = aspectRect.right - aspectRect.left;
                    int areaH = aspectRect.bottom - aspectRect.top;
                    
                    // Validate saved area is within monitor and has correct aspect ratio
                    // If not, calculate a new default centered rect
                    BOOL needsRecalc = FALSE;
                    if (areaW <= 0 || areaH <= 0) needsRecalc = TRUE;
                    if (aspectRect.left < monBounds.left || aspectRect.right > monBounds.right) needsRecalc = TRUE;
                    if (aspectRect.top < monBounds.top || aspectRect.bottom > monBounds.bottom) needsRecalc = TRUE;
                    
                    if (needsRecalc) {
                        aspectRect = CalculateAspectRect(monBounds, ratioW, ratioH);
                        g_config.replayAreaRect = aspectRect;
                    }
                    
                    AreaSelector_Show(aspectRect, TRUE);  // Allow moving aspect ratio regions
                } else {
                    // Native - show overlay covering full monitor (locked, no moving)
                    AreaSelector_Show(monBounds, FALSE);
                }
            }
            break;
        }
        case MODE_ALL_MONITORS: {
            RECT allBounds;
            if (Capture_GetAllMonitorsBounds(&allBounds)) {
                // Check if aspect ratio is set (not Native)
                if (g_config.replayAspectRatio > 0) {
                    int ratioW, ratioH;
                    GetAspectRatioDimensions(g_config.replayAspectRatio, &ratioW, &ratioH);
                    
                    RECT aspectRect = g_config.replayAreaRect;
                    int areaW = aspectRect.right - aspectRect.left;
                    int areaH = aspectRect.bottom - aspectRect.top;
                    
                    BOOL needsRecalc = FALSE;
                    if (areaW <= 0 || areaH <= 0) needsRecalc = TRUE;
                    if (aspectRect.left < allBounds.left || aspectRect.right > allBounds.right) needsRecalc = TRUE;
                    if (aspectRect.top < allBounds.top || aspectRect.bottom > allBounds.bottom) needsRecalc = TRUE;
                    
                    if (needsRecalc) {
                        aspectRect = CalculateAspectRect(allBounds, ratioW, ratioH);
                        g_config.replayAreaRect = aspectRect;
                    }
                    
                    AreaSelector_Show(aspectRect, TRUE);  // Allow moving aspect ratio regions
                } else {
                    // Native - show overlay covering all monitors (locked, no moving)
                    AreaSelector_Show(allBounds, FALSE);
                }
            }
            break;
        }
        case MODE_AREA: {
            // Show draggable area selector
            // Check if saved area is valid, otherwise create a centered default
            RECT areaRect = g_config.replayAreaRect;
            int areaW = areaRect.right - areaRect.left;
            int areaH = areaRect.bottom - areaRect.top;
            
            if (areaW < 100 || areaH < 100) {
                // Invalid or first-time - create a 640x480 box centered on primary monitor
                HMONITOR hMon = MonitorFromPoint((POINT){0, 0}, MONITOR_DEFAULTTOPRIMARY);
                MONITORINFO mi = { sizeof(mi) };
                GetMonitorInfo(hMon, &mi);
                int monW = mi.rcMonitor.right - mi.rcMonitor.left;
                int monH = mi.rcMonitor.bottom - mi.rcMonitor.top;
                int defaultW = 640;
                int defaultH = 480;
                areaRect.left = mi.rcMonitor.left + (monW - defaultW) / 2;
                areaRect.top = mi.rcMonitor.top + (monH - defaultH) / 2;
                areaRect.right = areaRect.left + defaultW;
                areaRect.bottom = areaRect.top + defaultH;
                g_config.replayAreaRect = areaRect;
            }
            
            AreaSelector_Show(areaRect, TRUE);
            break;
        }
        case MODE_WINDOW:
            // No preview for window mode - user selects window separately
            break;
        default:
            break;
    }
    
    // Ensure settings window and control panel stay on top of the preview overlays
    if (g_settingsWnd) {
        SetWindowPos(g_settingsWnd, HWND_TOPMOST, 0, 0, 0, 0, 
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    if (g_controlWnd) {
        SetWindowPos(g_controlWnd, HWND_TOPMOST, 0, 0, 0, 0, 
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

// Save area selector position to config
static void SaveAreaSelectorPosition(void) {
    if (AreaSelector_IsVisible()) {
        AreaSelector_GetRect(&g_config.replayAreaRect);
    }
}

static LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static HFONT g_settingsTitleFont = NULL;
    
    switch (msg) {
        case WM_CREATE: {
            // Create fonts
            g_settingsFont = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            g_settingsSmallFont = CreateFontW(11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            g_settingsTitleFont = CreateFontW(16, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
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
            SendMessage(cmbHours, CB_SETITEMHEIGHT, (WPARAM)-1, 18);  // Center text vertically
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
            SendMessage(cmbMins, CB_SETITEMHEIGHT, (WPARAM)-1, 18);  // Center text vertically
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
            SendMessage(cmbSecs, CB_SETITEMHEIGHT, (WPARAM)-1, 18);  // Center text vertically
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
            
            y += rowH + 12;
            
            // ===== REPLAY BUFFER SECTION =====
            // Separator line
            CreateWindowW(L"STATIC", L"",
                WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
                labelX, y, contentW, 2, hwnd, NULL, g_hInstance, NULL);
            y += 14;
            
            // Enable replay checkbox
            HWND chkReplayEnabled = CreateWindowW(L"BUTTON", L"Enable Instant Replay",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                labelX, y, 200, 24, hwnd, (HMENU)ID_CHK_REPLAY_ENABLED, g_hInstance, NULL);
            SendMessage(chkReplayEnabled, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            CheckDlgButton(hwnd, ID_CHK_REPLAY_ENABLED, g_config.replayEnabled ? BST_CHECKED : BST_UNCHECKED);
            y += 38;
            
            // Capture source dropdown
            HWND lblReplaySource = CreateWindowW(L"STATIC", L"Capture source",
                WS_CHILD | WS_VISIBLE,
                labelX, y + 5, labelW, 20, hwnd, NULL, g_hInstance, NULL);
            SendMessage(lblReplaySource, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            
            HWND cmbReplaySource = CreateWindowW(L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                controlX, y, controlW, 200, hwnd, (HMENU)ID_CMB_REPLAY_SOURCE, g_hInstance, NULL);
            SendMessage(cmbReplaySource, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            
            // Enumerate and add monitors dynamically
            int monitorCount = GetSystemMetrics(SM_CMONITORS);
            WCHAR monitorName[64];
            for (int i = 0; i < monitorCount; i++) {
                if (i == 0) {
                    wsprintfW(monitorName, L"Monitor %d (Primary)", i + 1);
                } else {
                    wsprintfW(monitorName, L"Monitor %d", i + 1);
                }
                SendMessageW(cmbReplaySource, CB_ADDSTRING, 0, (LPARAM)monitorName);
            }
            SendMessageW(cmbReplaySource, CB_ADDSTRING, 0, (LPARAM)L"All Monitors");
            SendMessageW(cmbReplaySource, CB_ADDSTRING, 0, (LPARAM)L"Specific Window");
            SendMessageW(cmbReplaySource, CB_ADDSTRING, 0, (LPARAM)L"Custom Area");
            
            // Set current selection based on config
            int sourceIndex = 0;
            if (g_config.replayCaptureSource == MODE_MONITOR) {
                // Specific monitor selected
                sourceIndex = g_config.replayMonitorIndex;
                if (sourceIndex >= monitorCount) sourceIndex = 0;
            } else if (g_config.replayCaptureSource == MODE_ALL_MONITORS) {
                sourceIndex = monitorCount;  // All Monitors is after individual monitors
            } else if (g_config.replayCaptureSource == MODE_WINDOW) {
                sourceIndex = monitorCount + 1;
            } else if (g_config.replayCaptureSource == MODE_AREA) {
                sourceIndex = monitorCount + 2;
            }
            SendMessage(cmbReplaySource, CB_SETCURSEL, sourceIndex, 0);
            y += rowH;
            
            // Aspect ratio dropdown (only enabled for monitor capture)
            HWND lblAspect = CreateWindowW(L"STATIC", L"Aspect ratio",
                WS_CHILD | WS_VISIBLE,
                labelX, y + 5, labelW, 20, hwnd, NULL, g_hInstance, NULL);
            SendMessage(lblAspect, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            
            HWND cmbAspect = CreateWindowW(L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                controlX, y, controlW, 250, hwnd, (HMENU)ID_CMB_REPLAY_ASPECT, g_hInstance, NULL);
            SendMessage(cmbAspect, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            SendMessageW(cmbAspect, CB_ADDSTRING, 0, (LPARAM)L"Native (No change)");
            SendMessageW(cmbAspect, CB_ADDSTRING, 0, (LPARAM)L"16:9 (YouTube, Standard)");
            SendMessageW(cmbAspect, CB_ADDSTRING, 0, (LPARAM)L"9:16 (TikTok, Shorts, Reels)");
            SendMessageW(cmbAspect, CB_ADDSTRING, 0, (LPARAM)L"1:1 (Square - Instagram)");
            SendMessageW(cmbAspect, CB_ADDSTRING, 0, (LPARAM)L"4:5 (Instagram Portrait)");
            SendMessageW(cmbAspect, CB_ADDSTRING, 0, (LPARAM)L"16:10");
            SendMessageW(cmbAspect, CB_ADDSTRING, 0, (LPARAM)L"4:3");
            SendMessageW(cmbAspect, CB_ADDSTRING, 0, (LPARAM)L"21:9 (Ultrawide)");
            SendMessageW(cmbAspect, CB_ADDSTRING, 0, (LPARAM)L"32:9 (Super Ultrawide)");
            SendMessage(cmbAspect, CB_SETCURSEL, g_config.replayAspectRatio, 0);
            
            // Enable/disable aspect ratio based on source
            BOOL enableAspect = (g_config.replayCaptureSource == MODE_MONITOR || 
                                 g_config.replayCaptureSource == MODE_ALL_MONITORS);
            EnableWindow(cmbAspect, enableAspect);
            y += rowH;
            
            // Frame rate dropdown
            HWND lblFPS = CreateWindowW(L"STATIC", L"Frame rate",
                WS_CHILD | WS_VISIBLE,
                labelX, y + 5, labelW, 20, hwnd, NULL, g_hInstance, NULL);
            SendMessage(lblFPS, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            
            HWND cmbFPS = CreateWindowW(L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                controlX, y, controlW, 150, hwnd, (HMENU)ID_CMB_REPLAY_FPS, g_hInstance, NULL);
            SendMessage(cmbFPS, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            SendMessageW(cmbFPS, CB_ADDSTRING, 0, (LPARAM)L"30 FPS");
            SendMessageW(cmbFPS, CB_ADDSTRING, 0, (LPARAM)L"60 FPS");
            SendMessageW(cmbFPS, CB_ADDSTRING, 0, (LPARAM)L"120 FPS");
            // Set selection based on config
            int fpsIdx = (g_config.replayFPS >= 120) ? 2 : (g_config.replayFPS >= 60) ? 1 : 0;
            SendMessage(cmbFPS, CB_SETCURSEL, fpsIdx, 0);
            y += rowH;
            
            // Buffer duration - using dropdowns for proper centering
            HWND lblReplayDuration = CreateWindowW(L"STATIC", L"Duration",
                WS_CHILD | WS_VISIBLE,
                labelX, y + 5, labelW, 20, hwnd, NULL, g_hInstance, NULL);
            SendMessage(lblReplayDuration, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            
            // Calculate hours, minutes, seconds from config
            int replayTotalSecs = g_config.replayDuration;
            int replayHours = replayTotalSecs / 3600;
            int replayMins = (replayTotalSecs % 3600) / 60;
            int replaySecs = replayTotalSecs % 60;
            
            // Hours dropdown
            HWND cmbReplayHours = CreateWindowW(L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                controlX, y, 55, 300, hwnd, (HMENU)ID_CMB_REPLAY_HOURS, g_hInstance, NULL);
            SendMessage(cmbReplayHours, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            SendMessage(cmbReplayHours, CB_SETITEMHEIGHT, (WPARAM)-1, 18);  // Center text vertically
            for (int i = 0; i <= 24; i++) {
                WCHAR buf[8]; wsprintfW(buf, L"%d", i);
                SendMessageW(cmbReplayHours, CB_ADDSTRING, 0, (LPARAM)buf);
            }
            SendMessage(cmbReplayHours, CB_SETCURSEL, replayHours, 0);
            
            HWND lblReplayH = CreateWindowW(L"STATIC", L"h",
                WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                controlX + 58, y, 15, 26, hwnd, NULL, g_hInstance, NULL);
            SendMessage(lblReplayH, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            
            // Minutes dropdown
            HWND cmbReplayMinutes = CreateWindowW(L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                controlX + 78, y, 55, 300, hwnd, (HMENU)ID_CMB_REPLAY_MINS, g_hInstance, NULL);
            SendMessage(cmbReplayMinutes, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            SendMessage(cmbReplayMinutes, CB_SETITEMHEIGHT, (WPARAM)-1, 18);  // Center text vertically
            for (int i = 0; i <= 59; i++) {
                WCHAR buf[8]; wsprintfW(buf, L"%d", i);
                SendMessageW(cmbReplayMinutes, CB_ADDSTRING, 0, (LPARAM)buf);
            }
            SendMessage(cmbReplayMinutes, CB_SETCURSEL, replayMins, 0);
            
            HWND lblReplayM = CreateWindowW(L"STATIC", L"m",
                WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                controlX + 136, y, 18, 26, hwnd, NULL, g_hInstance, NULL);
            SendMessage(lblReplayM, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            
            // Seconds dropdown
            HWND cmbReplaySecs = CreateWindowW(L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                controlX + 158, y, 55, 300, hwnd, (HMENU)ID_CMB_REPLAY_SECS, g_hInstance, NULL);
            SendMessage(cmbReplaySecs, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            SendMessage(cmbReplaySecs, CB_SETITEMHEIGHT, (WPARAM)-1, 18);  // Center text vertically
            for (int i = 0; i <= 59; i++) {
                WCHAR buf[8]; wsprintfW(buf, L"%d", i);
                SendMessageW(cmbReplaySecs, CB_ADDSTRING, 0, (LPARAM)buf);
            }
            SendMessage(cmbReplaySecs, CB_SETCURSEL, replaySecs, 0);
            
            HWND lblReplayS = CreateWindowW(L"STATIC", L"s",
                WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                controlX + 216, y, 15, 26, hwnd, NULL, g_hInstance, NULL);
            SendMessage(lblReplayS, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            y += rowH;
            
            // Save hotkey button
            HWND lblHotkey = CreateWindowW(L"STATIC", L"Save hotkey",
                WS_CHILD | WS_VISIBLE,
                labelX, y + 6, labelW, 20, hwnd, NULL, g_hInstance, NULL);
            SendMessage(lblHotkey, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            
            // Get current hotkey name
            char hotkeyName[64];
            GetKeyNameFromVK(g_config.replaySaveKey, hotkeyName, sizeof(hotkeyName));
            
            // Create button showing current hotkey - click to change
            HWND btnHotkey = CreateWindowExA(0, "BUTTON", hotkeyName,
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                controlX, y + 1, 120, 26, hwnd, (HMENU)ID_BTN_REPLAY_HOTKEY, g_hInstance, NULL);
            SendMessage(btnHotkey, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            
            // Hint text - larger and more visible
            HWND lblHotkeyHint = CreateWindowW(L"STATIC", L"(Click to change)",
                WS_CHILD | WS_VISIBLE,
                controlX + 130, y + 7, 140, 24, hwnd, NULL, g_hInstance, NULL);
            SendMessage(lblHotkeyHint, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            
            y += rowH;
            
            // RAM explanation and estimate
            {
                int durationSecs = g_config.replayDuration;
                int fps = g_config.replayFPS;
                int estWidth = GetSystemMetrics(SM_CXSCREEN);
                int estHeight = GetSystemMetrics(SM_CYSCREEN);
                
                // Adjust for aspect ratio if set
                if (g_config.replayAspectRatio > 0) {
                    int ratioW, ratioH;
                    GetAspectRatioDimensions(g_config.replayAspectRatio, &ratioW, &ratioH);
                    if (ratioW > 0 && ratioH > 0) {
                        if (estWidth * ratioH > estHeight * ratioW) {
                            estWidth = (estHeight * ratioW) / ratioH;
                        } else {
                            estHeight = (estWidth * ratioH) / ratioW;
                        }
                    }
                }
                
                int ramMB = ReplayBuffer_EstimateRAMUsage(durationSecs, estWidth, estHeight, fps);
                
                // Explanation text
                char explainText[256];
                sprintf(explainText, "When enabled, ~%d MB of RAM is reserved for the video buffer. See the calculation below:", ramMB);
                HWND lblExplain = CreateWindowExA(0, "STATIC", explainText,
                    WS_CHILD | WS_VISIBLE,
                    labelX, y + 4, contentW, 20, hwnd, (HMENU)ID_STATIC_REPLAY_RAM, g_hInstance, NULL);
                SendMessage(lblExplain, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
                y += 32;
                
                // Calculation breakdown
                char calcText[128];
                if (durationSecs >= 60) {
                    int mins = durationSecs / 60;
                    int secs = durationSecs % 60;
                    if (secs > 0) {
                        sprintf(calcText, "%dm %ds @ %d FPS, %dx%d = ~%d MB", mins, secs, fps, estWidth, estHeight, ramMB);
                    } else {
                        sprintf(calcText, "%dm @ %d FPS, %dx%d = ~%d MB", mins, fps, estWidth, estHeight, ramMB);
                    }
                } else {
                    sprintf(calcText, "%ds @ %d FPS, %dx%d = ~%d MB", durationSecs, fps, estWidth, estHeight, ramMB);
                }
                
                HWND lblCalc = CreateWindowExA(0, "STATIC", calcText,
                    WS_CHILD | WS_VISIBLE,
                    labelX + 20, y, contentW - 20, 20, hwnd, (HMENU)ID_STATIC_REPLAY_CALC, g_hInstance, NULL);
                SendMessage(lblCalc, WM_SETFONT, (WPARAM)g_settingsFont, TRUE);
            }
            
            // Initialize preview border and area selector
            PreviewBorder_Init(g_hInstance);
            AreaSelector_Init(g_hInstance);
            
            // Show preview for current capture source
            UpdateReplayPreview();
            
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
                
                // Replay buffer settings handlers
                case ID_CHK_REPLAY_ENABLED: {
                    BOOL wasEnabled = g_config.replayEnabled;
                    g_config.replayEnabled = IsDlgButtonChecked(hwnd, ID_CHK_REPLAY_ENABLED) == BST_CHECKED;
                    
                    // Start or stop replay buffer based on new state
                    if (g_config.replayEnabled && !wasEnabled) {
                        // Starting replay buffer
                        ReplayBuffer_Start(&g_replayBuffer, &g_config);
                        RegisterHotKey(g_controlWnd, HOTKEY_REPLAY_SAVE, 0, g_config.replaySaveKey);
                    } else if (!g_config.replayEnabled && wasEnabled) {
                        // Stopping replay buffer
                        UnregisterHotKey(g_controlWnd, HOTKEY_REPLAY_SAVE);
                        ReplayBuffer_Stop(&g_replayBuffer);
                    }
                    break;
                }
                    
                case ID_CMB_REPLAY_SOURCE:
                    if (HIWORD(wParam) == CBN_SELCHANGE) {
                        int sel = (int)SendMessage(GetDlgItem(hwnd, ID_CMB_REPLAY_SOURCE), CB_GETCURSEL, 0, 0);
                        int monCount = GetSystemMetrics(SM_CMONITORS);
                        
                        if (sel < monCount) {
                            // Individual monitor selected
                            g_config.replayCaptureSource = MODE_MONITOR;
                            g_config.replayMonitorIndex = sel;
                        } else if (sel == monCount) {
                            g_config.replayCaptureSource = MODE_ALL_MONITORS;
                        } else if (sel == monCount + 1) {
                            g_config.replayCaptureSource = MODE_WINDOW;
                        } else {
                            g_config.replayCaptureSource = MODE_AREA;
                        }
                        
                        // Enable/disable aspect ratio dropdown
                        BOOL enableAspect = (g_config.replayCaptureSource == MODE_MONITOR || 
                                             g_config.replayCaptureSource == MODE_ALL_MONITORS);
                        EnableWindow(GetDlgItem(hwnd, ID_CMB_REPLAY_ASPECT), enableAspect);
                        
                        // Update preview border/area selector
                        UpdateReplayPreview();
                    }
                    break;
                    
                case ID_CMB_REPLAY_HOURS:
                case ID_CMB_REPLAY_MINS:
                case ID_CMB_REPLAY_SECS:
                    // Duration is read on settings close, but update RAM estimate live
                    if (HIWORD(wParam) == CBN_SELCHANGE) {
                        int h = (int)SendMessage(GetDlgItem(hwnd, ID_CMB_REPLAY_HOURS), CB_GETCURSEL, 0, 0);
                        int m = (int)SendMessage(GetDlgItem(hwnd, ID_CMB_REPLAY_MINS), CB_GETCURSEL, 0, 0);
                        int s = (int)SendMessage(GetDlgItem(hwnd, ID_CMB_REPLAY_SECS), CB_GETCURSEL, 0, 0);
                        int total = h * 3600 + m * 60 + s;
                        if (total < 1) total = 1;
                        // Update config immediately so RAM estimate is accurate
                        g_config.replayDuration = total;
                        UpdateReplayRAMEstimate(hwnd);
                    }
                    break;
                    
                case ID_CMB_REPLAY_ASPECT:
                    if (HIWORD(wParam) == CBN_SELCHANGE) {
                        g_config.replayAspectRatio = (int)SendMessage(
                            GetDlgItem(hwnd, ID_CMB_REPLAY_ASPECT), CB_GETCURSEL, 0, 0);
                        
                        // Force recalculation of area for new aspect ratio
                        if (g_config.replayAspectRatio > 0) {
                            // Invalidate saved area to force recalc
                            g_config.replayAreaRect.left = 0;
                            g_config.replayAreaRect.top = 0;
                            g_config.replayAreaRect.right = 0;
                            g_config.replayAreaRect.bottom = 0;
                        }
                        
                        // Update preview
                        UpdateReplayPreview();
                        
                        // Update RAM estimate (resolution changes with aspect ratio)
                        UpdateReplayRAMEstimate(hwnd);
                    }
                    break;
                
                case ID_CMB_REPLAY_FPS:
                    if (HIWORD(wParam) == CBN_SELCHANGE) {
                        int idx = (int)SendMessage(GetDlgItem(hwnd, ID_CMB_REPLAY_FPS), CB_GETCURSEL, 0, 0);
                        g_config.replayFPS = (idx == 2) ? 120 : (idx == 1) ? 60 : 30;
                        
                        // Update RAM estimate
                        UpdateReplayRAMEstimate(hwnd);
                    }
                    break;
                    
                case ID_BTN_REPLAY_HOTKEY:
                    // Enter hotkey capture mode
                    g_waitingForHotkey = TRUE;
                    SetWindowTextA(GetDlgItem(hwnd, ID_BTN_REPLAY_HOTKEY), "Press a key...");
                    SetFocus(hwnd);  // Focus the window to receive key events
                    break;
            }
            return 0;
        
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:  // For Alt combinations
            if (g_waitingForHotkey) {
                // Get the virtual key code
                int vk = (int)wParam;
                
                // Ignore modifier keys alone
                if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || 
                    vk == VK_LSHIFT || vk == VK_RSHIFT || 
                    vk == VK_LCONTROL || vk == VK_RCONTROL ||
                    vk == VK_LMENU || vk == VK_RMENU) {
                    return 0;
                }
                
                // Unregister old hotkey if replay is enabled
                if (g_config.replayEnabled) {
                    UnregisterHotKey(g_controlWnd, HOTKEY_REPLAY_SAVE);
                }
                
                // Save the new hotkey
                g_config.replaySaveKey = vk;
                
                // Re-register with new hotkey if replay is enabled
                if (g_config.replayEnabled) {
                    RegisterHotKey(g_controlWnd, HOTKEY_REPLAY_SAVE, 0, g_config.replaySaveKey);
                }
                
                // Update button text with key name
                char keyName[64];
                GetKeyNameFromVK(vk, keyName, sizeof(keyName));
                SetWindowTextA(GetDlgItem(hwnd, ID_BTN_REPLAY_HOTKEY), keyName);
                
                g_waitingForHotkey = FALSE;
                return 0;
            }
            break;
            
        case WM_CLOSE: {
            // Save area selector position if visible
            SaveAreaSelectorPosition();
            
            // Hide preview overlays
            PreviewBorder_Hide();
            AreaSelector_Hide();
            
            // Save time limit from dropdowns
            int hours = (int)SendMessage(GetDlgItem(hwnd, ID_CMB_HOURS), CB_GETCURSEL, 0, 0);
            int mins = (int)SendMessage(GetDlgItem(hwnd, ID_CMB_MINUTES), CB_GETCURSEL, 0, 0);
            int secs = (int)SendMessage(GetDlgItem(hwnd, ID_CMB_SECONDS), CB_GETCURSEL, 0, 0);
            int total = hours * 3600 + mins * 60 + secs;
            if (total < 1) total = 1;
            g_config.maxRecordingSeconds = total;
            
            // Save replay duration from dropdowns (simple: 3 boxes -> 1 number)
            int rh = (int)SendMessage(GetDlgItem(hwnd, ID_CMB_REPLAY_HOURS), CB_GETCURSEL, 0, 0);
            int rm = (int)SendMessage(GetDlgItem(hwnd, ID_CMB_REPLAY_MINS), CB_GETCURSEL, 0, 0);
            int rs = (int)SendMessage(GetDlgItem(hwnd, ID_CMB_REPLAY_SECS), CB_GETCURSEL, 0, 0);
            int replayTotal = rh * 3600 + rm * 60 + rs;
            if (replayTotal < 1) replayTotal = 1;
            g_config.replayDuration = replayTotal;
            
            // Save path
            GetWindowTextA(GetDlgItem(hwnd, ID_EDT_PATH), g_config.savePath, MAX_PATH);
            Config_Save(&g_config);
            
            // Clean up preview overlays
            PreviewBorder_Shutdown();
            AreaSelector_Shutdown();
            
            // Clean up fonts and brushes
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

// Timer display font

