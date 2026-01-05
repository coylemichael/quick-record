/*
 * GPU Color Converter Implementation
 * Uses D3D11 Video Processor for hardware BGRA→NV12 conversion
 */

#include "gpu_converter.h"
#include "logger.h"
#include <dxgi.h>

#define GPULog Logger_Log

BOOL GPUConverter_Init(GPUConverter* conv, ID3D11Device* device, int width, int height) {
    if (!conv || !device) return FALSE;
    
    ZeroMemory(conv, sizeof(GPUConverter));
    conv->device = device;
    conv->width = width;
    conv->height = height;
    
    HRESULT hr;
    
    // Get device context
    device->lpVtbl->GetImmediateContext(device, &conv->context);
    if (!conv->context) {
        GPULog("GPUConverter: Failed to get device context\n");
        return FALSE;
    }
    
    // Get video device interface
    hr = device->lpVtbl->QueryInterface(device, &IID_ID3D11VideoDevice, (void**)&conv->videoDevice);
    if (FAILED(hr)) {
        GPULog("GPUConverter: QueryInterface ID3D11VideoDevice failed: 0x%08X\n", hr);
        goto fail;
    }
    
    // Get video context interface
    hr = conv->context->lpVtbl->QueryInterface(conv->context, &IID_ID3D11VideoContext, (void**)&conv->videoContext);
    if (FAILED(hr)) {
        GPULog("GPUConverter: QueryInterface ID3D11VideoContext failed: 0x%08X\n", hr);
        goto fail;
    }
    
    // Create video processor enumerator
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC contentDesc = {0};
    contentDesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    contentDesc.InputWidth = width;
    contentDesc.InputHeight = height;
    contentDesc.OutputWidth = width;
    contentDesc.OutputHeight = height;
    contentDesc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    
    hr = conv->videoDevice->lpVtbl->CreateVideoProcessorEnumerator(
        conv->videoDevice, &contentDesc, &conv->processorEnum);
    if (FAILED(hr)) {
        GPULog("GPUConverter: CreateVideoProcessorEnumerator failed: 0x%08X\n", hr);
        goto fail;
    }
    
    // Create video processor
    hr = conv->videoDevice->lpVtbl->CreateVideoProcessor(
        conv->videoDevice, conv->processorEnum, 0, &conv->videoProcessor);
    if (FAILED(hr)) {
        GPULog("GPUConverter: CreateVideoProcessor failed: 0x%08X\n", hr);
        goto fail;
    }
    
    // Create NV12 output texture
    D3D11_TEXTURE2D_DESC texDesc = {0};
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_NV12;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_VIDEO_ENCODER;
    texDesc.CPUAccessFlags = 0;
    texDesc.MiscFlags = 0;
    
    hr = device->lpVtbl->CreateTexture2D(device, &texDesc, NULL, &conv->outputTexture);
    if (FAILED(hr)) {
        GPULog("GPUConverter: CreateTexture2D (NV12) failed: 0x%08X\n", hr);
        goto fail;
    }
    
    // Create output view for video processor
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputViewDesc = {0};
    outputViewDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    outputViewDesc.Texture2D.MipSlice = 0;
    
    hr = conv->videoDevice->lpVtbl->CreateVideoProcessorOutputView(
        conv->videoDevice, (ID3D11Resource*)conv->outputTexture,
        conv->processorEnum, &outputViewDesc, &conv->outputView);
    if (FAILED(hr)) {
        GPULog("GPUConverter: CreateVideoProcessorOutputView failed: 0x%08X\n", hr);
        goto fail;
    }
    
    conv->initialized = TRUE;
    GPULog("GPUConverter: Initialized %dx%d BGRA→NV12 (D3D11 Video Processor)\n", width, height);
    return TRUE;
    
fail:
    GPUConverter_Shutdown(conv);
    return FALSE;
}

ID3D11Texture2D* GPUConverter_Convert(GPUConverter* conv, ID3D11Texture2D* bgraTexture) {
    if (!conv->initialized || !bgraTexture) return NULL;
    
    HRESULT hr;
    
    // Create input view for this texture (recreate each time for different textures)
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputViewDesc = {0};
    inputViewDesc.FourCC = 0;  // Use texture format
    inputViewDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    inputViewDesc.Texture2D.MipSlice = 0;
    inputViewDesc.Texture2D.ArraySlice = 0;
    
    ID3D11VideoProcessorInputView* inputView = NULL;
    hr = conv->videoDevice->lpVtbl->CreateVideoProcessorInputView(
        conv->videoDevice, (ID3D11Resource*)bgraTexture,
        conv->processorEnum, &inputViewDesc, &inputView);
    
    if (FAILED(hr)) {
        return NULL;
    }
    
    // Configure video processor stream
    D3D11_VIDEO_PROCESSOR_STREAM stream = {0};
    stream.Enable = TRUE;
    stream.OutputIndex = 0;
    stream.InputFrameOrField = 0;
    stream.PastFrames = 0;
    stream.FutureFrames = 0;
    stream.ppPastSurfaces = NULL;
    stream.pInputSurface = inputView;
    stream.ppFutureSurfaces = NULL;
    stream.ppPastSurfacesRight = NULL;
    stream.pInputSurfaceRight = NULL;
    stream.ppFutureSurfacesRight = NULL;
    
    // Run the video processor (BGRA → NV12 conversion on GPU)
    hr = conv->videoContext->lpVtbl->VideoProcessorBlt(
        conv->videoContext, conv->videoProcessor,
        conv->outputView, 0, 1, &stream);
    
    inputView->lpVtbl->Release(inputView);
    
    if (FAILED(hr)) {
        return NULL;
    }
    
    return conv->outputTexture;
}

void GPUConverter_Shutdown(GPUConverter* conv) {
    if (!conv) return;
    
    if (conv->outputView) {
        conv->outputView->lpVtbl->Release(conv->outputView);
        conv->outputView = NULL;
    }
    if (conv->outputTexture) {
        conv->outputTexture->lpVtbl->Release(conv->outputTexture);
        conv->outputTexture = NULL;
    }
    if (conv->videoProcessor) {
        conv->videoProcessor->lpVtbl->Release(conv->videoProcessor);
        conv->videoProcessor = NULL;
    }
    if (conv->processorEnum) {
        conv->processorEnum->lpVtbl->Release(conv->processorEnum);
        conv->processorEnum = NULL;
    }
    if (conv->videoContext) {
        conv->videoContext->lpVtbl->Release(conv->videoContext);
        conv->videoContext = NULL;
    }
    if (conv->videoDevice) {
        conv->videoDevice->lpVtbl->Release(conv->videoDevice);
        conv->videoDevice = NULL;
    }
    if (conv->context) {
        conv->context->lpVtbl->Release(conv->context);
        conv->context = NULL;
    }
    
    conv->initialized = FALSE;
}
