/*
 * Configuration Implementation
 */

#include "config.h"
#include <shlobj.h>
#include <stdio.h>

static const char* FORMAT_EXTENSIONS[] = { ".mp4", ".avi", ".wmv" };
static const char* FORMAT_NAMES[] = { "MP4 (H.264)", "AVI", "WMV" };

void Config_GetPath(char* buffer, size_t size) {
    // Store config next to executable
    GetModuleFileNameA(NULL, buffer, (DWORD)size);
    char* lastSlash = strrchr(buffer, '\\');
    if (lastSlash) {
        strcpy(lastSlash + 1, "lwsr_config.ini");
    }
}

void Config_Load(AppConfig* config) {
    char configPath[MAX_PATH];
    Config_GetPath(configPath, MAX_PATH);
    
    // Set defaults
    config->outputFormat = FORMAT_MP4;
    config->quality = QUALITY_HIGH;
    config->captureMouse = TRUE;
    config->showRecordingBorder = TRUE;
    config->maxRecordingSeconds = 0;
    
    // Default save path to Videos folder
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_MYVIDEO, NULL, 0, config->savePath))) {
        strcat(config->savePath, "\\Recordings");
    } else {
        strcpy(config->savePath, "C:\\Recordings");
    }
    
    SetRectEmpty(&config->lastCaptureRect);
    config->lastMode = MODE_AREA;
    
    // Load from INI if exists
    if (GetFileAttributesA(configPath) != INVALID_FILE_ATTRIBUTES) {
        config->outputFormat = (OutputFormat)GetPrivateProfileIntA(
            "Recording", "Format", FORMAT_MP4, configPath);
        config->quality = (QualityPreset)GetPrivateProfileIntA(
            "Recording", "Quality", QUALITY_HIGH, configPath);
        config->captureMouse = GetPrivateProfileIntA(
            "Recording", "CaptureMouse", TRUE, configPath);
        config->showRecordingBorder = GetPrivateProfileIntA(
            "Recording", "ShowBorder", TRUE, configPath);
        config->maxRecordingSeconds = GetPrivateProfileIntA(
            "Recording", "MaxSeconds", 0, configPath);
        
        GetPrivateProfileStringA("Recording", "SavePath", config->savePath,
            config->savePath, MAX_PATH, configPath);
        
        config->lastCaptureRect.left = GetPrivateProfileIntA(
            "LastCapture", "Left", 0, configPath);
        config->lastCaptureRect.top = GetPrivateProfileIntA(
            "LastCapture", "Top", 0, configPath);
        config->lastCaptureRect.right = GetPrivateProfileIntA(
            "LastCapture", "Right", 0, configPath);
        config->lastCaptureRect.bottom = GetPrivateProfileIntA(
            "LastCapture", "Bottom", 0, configPath);
        config->lastMode = (CaptureMode)GetPrivateProfileIntA(
            "LastCapture", "Mode", MODE_AREA, configPath);
    }
    
    // Ensure save directory exists
    CreateDirectoryA(config->savePath, NULL);
}

void Config_Save(const AppConfig* config) {
    char configPath[MAX_PATH];
    Config_GetPath(configPath, MAX_PATH);
    
    char buffer[32];
    
    sprintf(buffer, "%d", config->outputFormat);
    WritePrivateProfileStringA("Recording", "Format", buffer, configPath);
    
    sprintf(buffer, "%d", config->quality);
    WritePrivateProfileStringA("Recording", "Quality", buffer, configPath);
    
    sprintf(buffer, "%d", config->captureMouse);
    WritePrivateProfileStringA("Recording", "CaptureMouse", buffer, configPath);
    
    sprintf(buffer, "%d", config->showRecordingBorder);
    WritePrivateProfileStringA("Recording", "ShowBorder", buffer, configPath);
    
    sprintf(buffer, "%d", config->maxRecordingSeconds);
    WritePrivateProfileStringA("Recording", "MaxSeconds", buffer, configPath);
    
    WritePrivateProfileStringA("Recording", "SavePath", config->savePath, configPath);
    
    sprintf(buffer, "%ld", config->lastCaptureRect.left);
    WritePrivateProfileStringA("LastCapture", "Left", buffer, configPath);
    
    sprintf(buffer, "%ld", config->lastCaptureRect.top);
    WritePrivateProfileStringA("LastCapture", "Top", buffer, configPath);
    
    sprintf(buffer, "%ld", config->lastCaptureRect.right);
    WritePrivateProfileStringA("LastCapture", "Right", buffer, configPath);
    
    sprintf(buffer, "%ld", config->lastCaptureRect.bottom);
    WritePrivateProfileStringA("LastCapture", "Bottom", buffer, configPath);
    
    sprintf(buffer, "%d", config->lastMode);
    WritePrivateProfileStringA("LastCapture", "Mode", buffer, configPath);
}

const char* Config_GetFormatExtension(OutputFormat format) {
    if (format >= 0 && format < FORMAT_COUNT) {
        return FORMAT_EXTENSIONS[format];
    }
    return ".mp4";
}

const char* Config_GetFormatName(OutputFormat format) {
    if (format >= 0 && format < FORMAT_COUNT) {
        return FORMAT_NAMES[format];
    }
    return "MP4";
}
