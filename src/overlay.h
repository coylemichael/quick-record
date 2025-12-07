/*
 * Overlay Window System
 * Handles selection UI and recording controls
 */

#ifndef OVERLAY_H
#define OVERLAY_H

#include <windows.h>
#include "config.h"

// Create the overlay window system
BOOL Overlay_Create(HINSTANCE hInstance);

// Destroy overlay windows
void Overlay_Destroy(void);

// Show/hide the selection overlay
void Overlay_ShowSelection(BOOL show);

// Show/hide the control panel
void Overlay_ShowControls(BOOL show);

// Set the current capture mode
void Overlay_SetMode(CaptureMode mode);

// Get the selected region
BOOL Overlay_GetSelectedRegion(RECT* region);

// Update recording state display
void Overlay_SetRecordingState(BOOL isRecording);

// Get overlay window handle
HWND Overlay_GetWindow(void);

// Start recording with current selection
void Recording_Start(void);

// Stop current recording
void Recording_Stop(void);

#endif // OVERLAY_H
