// MCP Integration - Screenshot capture and status reporting for OpenXR Simulator
#pragma once

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <cstdio>
#include <cstring>
#include <cstdarg>

namespace mcp {

using Microsoft::WRL::ComPtr;

// MCP-specific logging
inline void McpLog(const char* msg) {
    OutputDebugStringA("[OxrWXR-MCP] ");
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");
}
inline void McpLogf(const char* fmt, ...) {
    char buf[2048];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    McpLog(buf);
}

inline std::string GetSimulatorDataPath() {
    char base[MAX_PATH]{};
    DWORD len = GetEnvironmentVariableA("LOCALAPPDATA", base, (DWORD)sizeof(base));
    if (len > 0 && len < sizeof(base)) {
        return std::string(base) + "\\OxrWXR-Simulator";
    }
    return ".";
}

inline bool g_screenshotRequested = false;
inline std::string g_screenshotEye = "both";
inline std::string g_screenshotLayer = "projection";  // "projection", "quad", or "all"

// Storage for quad layer pixels (set by renderQuadLayer)
inline std::vector<uint8_t> g_quadLayerPixels;
inline uint32_t g_quadLayerWidth = 0;
inline uint32_t g_quadLayerHeight = 0;
inline bool g_quadLayerCaptured = false;

// Check if MCP has requested a screenshot
inline void CheckScreenshotRequest() {
    std::string reqPath = GetSimulatorDataPath() + "\\screenshot_request.json";
    FILE* f = nullptr;
    if (fopen_s(&f, reqPath.c_str(), "r") == 0 && f) {
        char buf[512];
        size_t n = fread(buf, 1, sizeof(buf)-1, f);
        buf[n] = 0;
        fclose(f);

        g_screenshotRequested = true;
        g_screenshotEye = "both";
        g_screenshotLayer = "projection";  // default

        const char* eyePos = strstr(buf, "\"eye\"");
        if (eyePos) {
            if (strstr(eyePos, "\"left\"")) g_screenshotEye = "left";
            else if (strstr(eyePos, "\"right\"")) g_screenshotEye = "right";
        }

        // Check for layer type specification
        const char* layerPos = strstr(buf, "\"layer\"");
        if (layerPos) {
            if (strstr(layerPos, "\"quad\"")) g_screenshotLayer = "quad";
            else if (strstr(layerPos, "\"all\"")) g_screenshotLayer = "all";
        }

        DeleteFileA(reqPath.c_str());
        McpLogf("Screenshot request detected: layer=%s, eye=%s", g_screenshotLayer.c_str(), g_screenshotEye.c_str());
    }
}

// Store quad layer pixels for screenshot capture
inline void StoreQuadLayerPixels(const uint8_t* pixels, uint32_t width, uint32_t height) {
    if (!pixels || width == 0 || height == 0) return;
    g_quadLayerWidth = width;
    g_quadLayerHeight = height;
    g_quadLayerPixels.resize(width * height * 4);
    memcpy(g_quadLayerPixels.data(), pixels, width * height * 4);
    g_quadLayerCaptured = true;
}

// Forward declaration - CaptureQuadScreenshot is defined after SavePixelsToBMP
inline void CaptureQuadScreenshot();

// Save a D3D11 texture to BMP file
inline bool SaveTextureToBMP(ID3D11Device* device, ID3D11DeviceContext* ctx,
                              ID3D11Texture2D* texture, const char* filename) {
    if (!device || !ctx || !texture) return false;

    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);

    // Create staging texture for CPU read
    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> staging;
    HRESULT hr = device->CreateTexture2D(&stagingDesc, nullptr, staging.GetAddressOf());
    if (FAILED(hr)) {
        McpLogf("Failed to create staging texture: 0x%08X", hr);
        return false;
    }

    // Copy to staging
    ctx->CopyResource(staging.Get(), texture);

    // Map and read pixels
    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        McpLogf("Failed to map staging texture: 0x%08X", hr);
        return false;
    }

    // Write BMP file
    FILE* file = nullptr;
    if (fopen_s(&file, filename, "wb") != 0 || !file) {
        ctx->Unmap(staging.Get(), 0);
        return false;
    }

    uint32_t w = desc.Width;
    uint32_t h = desc.Height;
    uint32_t rowSize = w * 3;
    uint32_t rowPadding = (4 - (rowSize % 4)) % 4;
    uint32_t rowStride = rowSize + rowPadding;
    uint32_t imageSize = rowStride * h;

    // BMP Header (54 bytes)
    uint8_t bmpHeader[54] = {
        'B', 'M',           // Signature
        0, 0, 0, 0,         // File size
        0, 0, 0, 0,         // Reserved
        54, 0, 0, 0,        // Data offset
        40, 0, 0, 0,        // DIB header size
        0, 0, 0, 0,         // Width
        0, 0, 0, 0,         // Height
        1, 0,               // Planes
        24, 0,              // Bits per pixel
        0, 0, 0, 0,         // Compression
        0, 0, 0, 0,         // Image size
        0, 0, 0, 0,         // X pixels/meter
        0, 0, 0, 0,         // Y pixels/meter
        0, 0, 0, 0,         // Colors in table
        0, 0, 0, 0          // Important colors
    };

    uint32_t fileSize = 54 + imageSize;
    memcpy(bmpHeader + 2, &fileSize, 4);
    memcpy(bmpHeader + 18, &w, 4);
    memcpy(bmpHeader + 22, &h, 4);
    memcpy(bmpHeader + 34, &imageSize, 4);

    fwrite(bmpHeader, 1, 54, file);

    // Write pixel data (BMP is bottom-up, BGR)
    std::vector<uint8_t> row(rowStride, 0);
    uint8_t* src = (uint8_t*)mapped.pData;

    for (int y = h - 1; y >= 0; y--) {
        uint8_t* srcRow = src + y * mapped.RowPitch;
        for (uint32_t x = 0; x < w; x++) {
            // Handle different DXGI formats
            if (desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
                row[x*3 + 0] = srcRow[x*4 + 2]; // B
                row[x*3 + 1] = srcRow[x*4 + 1]; // G
                row[x*3 + 2] = srcRow[x*4 + 0]; // R
            } else if (desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                       desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
                row[x*3 + 0] = srcRow[x*4 + 0]; // B
                row[x*3 + 1] = srcRow[x*4 + 1]; // G
                row[x*3 + 2] = srcRow[x*4 + 2]; // R
            } else {
                // Fallback - assume RGBA
                row[x*3 + 0] = srcRow[x*4 + 2];
                row[x*3 + 1] = srcRow[x*4 + 1];
                row[x*3 + 2] = srcRow[x*4 + 0];
            }
        }
        fwrite(row.data(), 1, rowStride, file);
    }

    fclose(file);
    ctx->Unmap(staging.Get(), 0);

    McpLogf("Screenshot saved: %s (%ux%u)", filename, w, h);
    return true;
}

// Capture screenshot from preview swapchain (D3D11 path)
inline void CaptureScreenshot(ID3D11Device* device, ID3D11DeviceContext* ctx,
                               IDXGISwapChain1* swapchain) {
    if (!swapchain) return;

    ComPtr<ID3D11Texture2D> backbuffer;
    if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(backbuffer.GetAddressOf())))) {
        McpLog("Failed to get backbuffer for screenshot");
        return;
    }

    std::string outPath = GetSimulatorDataPath() + "\\screenshot.bmp";
    SaveTextureToBMP(device, ctx, backbuffer.Get(), outPath.c_str());

    g_screenshotRequested = false;
}

// Save raw RGBA pixel data to BMP (for OpenGL path)
inline bool SavePixelsToBMP(const uint8_t* pixels, uint32_t width, uint32_t height,
                             const char* filename) {
    if (!pixels || width == 0 || height == 0) return false;

    FILE* file = nullptr;
    if (fopen_s(&file, filename, "wb") != 0 || !file) {
        McpLogf("Failed to open file for writing: %s", filename);
        return false;
    }

    uint32_t rowSize = width * 3;
    uint32_t rowPadding = (4 - (rowSize % 4)) % 4;
    uint32_t rowStride = rowSize + rowPadding;
    uint32_t imageSize = rowStride * height;

    // BMP Header (54 bytes)
    uint8_t bmpHeader[54] = {
        'B', 'M',           // Signature
        0, 0, 0, 0,         // File size
        0, 0, 0, 0,         // Reserved
        54, 0, 0, 0,        // Data offset
        40, 0, 0, 0,        // DIB header size
        0, 0, 0, 0,         // Width
        0, 0, 0, 0,         // Height
        1, 0,               // Planes
        24, 0,              // Bits per pixel
        0, 0, 0, 0,         // Compression
        0, 0, 0, 0,         // Image size
        0, 0, 0, 0,         // X pixels/meter
        0, 0, 0, 0,         // Y pixels/meter
        0, 0, 0, 0,         // Colors in table
        0, 0, 0, 0          // Important colors
    };

    uint32_t fileSize = 54 + imageSize;
    memcpy(bmpHeader + 2, &fileSize, 4);
    memcpy(bmpHeader + 18, &width, 4);
    memcpy(bmpHeader + 22, &height, 4);
    memcpy(bmpHeader + 34, &imageSize, 4);

    fwrite(bmpHeader, 1, 54, file);

    // Write pixel data (BMP is bottom-up, BGR)
    std::vector<uint8_t> row(rowStride, 0);

    for (int y = height - 1; y >= 0; y--) {
        const uint8_t* srcRow = pixels + y * width * 4;
        for (uint32_t x = 0; x < width; x++) {
            // RGBA to BGR
            row[x*3 + 0] = srcRow[x*4 + 2]; // B
            row[x*3 + 1] = srcRow[x*4 + 1]; // G
            row[x*3 + 2] = srcRow[x*4 + 0]; // R
        }
        fwrite(row.data(), 1, rowStride, file);
    }

    fclose(file);
    McpLogf("Screenshot saved (pixels): %s (%ux%u)", filename, width, height);
    return true;
}

// Capture quad layer screenshot (implementation - forward declared above)
inline void CaptureQuadScreenshot() {
    if (!g_quadLayerCaptured || g_quadLayerPixels.empty()) {
        McpLog("No quad layer pixels available for screenshot");
        g_screenshotRequested = false;
        return;
    }

    std::string outPath = GetSimulatorDataPath() + "\\screenshot_quad.bmp";
    SavePixelsToBMP(g_quadLayerPixels.data(), g_quadLayerWidth, g_quadLayerHeight, outPath.c_str());
    McpLogf("Quad layer screenshot saved: %s (%ux%u)", outPath.c_str(), g_quadLayerWidth, g_quadLayerHeight);
    g_screenshotRequested = false;
}

// Capture screenshot from OpenGL pixel data (side-by-side left+right eyes)
inline void CaptureScreenshotGL(const uint8_t* leftPixels, const uint8_t* rightPixels,
                                 uint32_t width, uint32_t height) {
    if (!leftPixels && !rightPixels) {
        McpLog("No pixel data for GL screenshot");
        g_screenshotRequested = false;
        return;
    }

    // Create side-by-side image
    uint32_t totalWidth = (leftPixels && rightPixels) ? width * 2 : width;
    std::vector<uint8_t> combined(totalWidth * height * 4, 0);

    if (leftPixels && rightPixels) {
        // Side by side: left eye on left, right eye on right
        for (uint32_t y = 0; y < height; y++) {
            // Copy left eye
            memcpy(combined.data() + y * totalWidth * 4,
                   leftPixels + y * width * 4,
                   width * 4);
            // Copy right eye
            memcpy(combined.data() + y * totalWidth * 4 + width * 4,
                   rightPixels + y * width * 4,
                   width * 4);
        }
    } else if (leftPixels) {
        memcpy(combined.data(), leftPixels, width * height * 4);
    } else {
        memcpy(combined.data(), rightPixels, width * height * 4);
    }

    std::string outPath = GetSimulatorDataPath() + "\\screenshot.bmp";
    SavePixelsToBMP(combined.data(), totalWidth, height, outPath.c_str());

    g_screenshotRequested = false;
}

// Write frame status JSON for MCP
inline void WriteFrameStatus(uint32_t frameCount, uint32_t width, uint32_t height,
                              const char* format, const char* sessionState,
                              float headYaw = 0, float headPitch = 0,
                              float headX = 0, float headY = 1.7f, float headZ = 0) {
    static uint32_t lastWrite = 0;
    // Only write every 30 frames to reduce I/O
    if (frameCount - lastWrite < 30) return;
    lastWrite = frameCount;

    std::string path = GetSimulatorDataPath() + "\\runtime_status.json";
    FILE* file = nullptr;
    if (fopen_s(&file, path.c_str(), "w") != 0 || !file) return;

    // Get current timestamp
    SYSTEMTIME st;
    GetLocalTime(&st);

    fprintf(file, "{\n");
    fprintf(file, "  \"timestamp\": \"%04d-%02d-%02dT%02d:%02d:%02d\",\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    fprintf(file, "  \"frame_count\": %u,\n", frameCount);
    fprintf(file, "  \"preview_width\": %u,\n", width);
    fprintf(file, "  \"preview_height\": %u,\n", height);
    fprintf(file, "  \"format\": \"%s\",\n", format ? format : "unknown");
    fprintf(file, "  \"session_state\": \"%s\",\n", sessionState ? sessionState : "unknown");
    fprintf(file, "  \"target_fps\": 90,\n");
    fprintf(file, "  \"frame_time_ms\": 11.1,\n");
    fprintf(file, "  \"head_tracking\": {\n");
    fprintf(file, "    \"position\": {\"x\": %.3f, \"y\": %.3f, \"z\": %.3f},\n", headX, headY, headZ);
    fprintf(file, "    \"yaw\": %.3f,\n", headYaw);
    fprintf(file, "    \"pitch\": %.3f\n", headPitch);
    fprintf(file, "  }\n");
    fprintf(file, "}\n");
    fclose(file);
}

// Get session state name
inline const char* GetSessionStateName(int state) {
    switch (state) {
        case 0: return "UNKNOWN";
        case 1: return "IDLE";
        case 2: return "READY";
        case 3: return "SYNCHRONIZED";
        case 4: return "VISIBLE";
        case 5: return "FOCUSED";
        case 6: return "STOPPING";
        case 7: return "LOSS_PENDING";
        case 8: return "EXITING";
        default: return "UNKNOWN";
    }
}

// Head pose control structure for MCP
struct HeadPoseCommand {
    bool valid = false;
    float x = 0.0f;
    float y = 1.7f;
    float z = 0.0f;
    float yaw = 0.0f;
    float pitch = 0.0f;
};

// Simple JSON float parser
inline float ParseJsonFloat(const char* json, const char* key, float defaultVal) {
    char searchKey[64];
    snprintf(searchKey, sizeof(searchKey), "\"%s\"", key);
    const char* pos = strstr(json, searchKey);
    if (!pos) return defaultVal;
    pos = strchr(pos, ':');
    if (!pos) return defaultVal;
    pos++;
    while (*pos == ' ' || *pos == '\t') pos++;
    return (float)atof(pos);
}

// Check for head pose command from MCP
// File format: {"x": 0, "y": 1.7, "z": 0, "yaw": 0, "pitch": 0}
inline HeadPoseCommand CheckHeadPoseCommand() {
    HeadPoseCommand cmd;
    std::string cmdPath = GetSimulatorDataPath() + "\\head_pose_command.json";
    FILE* f = nullptr;
    if (fopen_s(&f, cmdPath.c_str(), "r") == 0 && f) {
        char buf[512];
        size_t n = fread(buf, 1, sizeof(buf)-1, f);
        buf[n] = 0;
        fclose(f);

        cmd.valid = true;
        cmd.x = ParseJsonFloat(buf, "x", 0.0f);
        cmd.y = ParseJsonFloat(buf, "y", 1.7f);
        cmd.z = ParseJsonFloat(buf, "z", 0.0f);
        cmd.yaw = ParseJsonFloat(buf, "yaw", 0.0f);
        cmd.pitch = ParseJsonFloat(buf, "pitch", 0.0f);

        // Delete the file after reading (one-shot command)
        DeleteFileA(cmdPath.c_str());
        McpLogf("Head pose command: pos(%.2f, %.2f, %.2f) yaw=%.1f pitch=%.1f",
                cmd.x, cmd.y, cmd.z, cmd.yaw, cmd.pitch);
    }
    return cmd;
}

// Write acknowledgment that command was processed
inline void WriteCommandAck(const char* cmdType, bool success) {
    std::string ackPath = GetSimulatorDataPath() + "\\command_ack.json";
    FILE* f = nullptr;
    if (fopen_s(&f, ackPath.c_str(), "w") == 0 && f) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "{\n");
        fprintf(f, "  \"timestamp\": \"%04d-%02d-%02dT%02d:%02d:%02d.%03d\",\n",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        fprintf(f, "  \"command\": \"%s\",\n", cmdType);
        fprintf(f, "  \"success\": %s\n", success ? "true" : "false");
        fprintf(f, "}\n");
        fclose(f);
    }
}

} // namespace mcp
