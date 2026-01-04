/*
 * GPU Color Converter
 * Uses D3D11 Video Processor for hardware BGRA→NV12 conversion
 * Zero-copy path for NVENC encoding
 */

#ifndef GPU_CONVERTER_H
#define GPU_CONVERTER_H

#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>

typedef struct {
    ID3D11Device* device;
    ID3D11DeviceContext* context;
    ID3D11VideoDevice* videoDevice;
    ID3D11VideoContext* videoContext;
    ID3D11VideoProcessor* videoProcessor;
    ID3D11VideoProcessorEnumerator* processorEnum;
    ID3D11VideoProcessorInputView* inputView;
    ID3D11VideoProcessorOutputView* outputView;
    ID3D11Texture2D* outputTexture;  // NV12 output
    int width;
    int height;
    BOOL initialized;
} GPUConverter;

// Initialize GPU converter
BOOL GPUConverter_Init(GPUConverter* conv, ID3D11Device* device, int width, int height);

// Convert BGRA texture to NV12 texture (GPU-only, no CPU copy)
// Returns the NV12 output texture (owned by converter, do not release)
ID3D11Texture2D* GPUConverter_Convert(GPUConverter* conv, ID3D11Texture2D* bgraTexture);

// Shutdown and release resources  
void GPUConverter_Shutdown(GPUConverter* conv);

#endif // GPU_CONVERTER_H
