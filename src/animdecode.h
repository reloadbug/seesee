// seesee / 看看 — 动图（GIF / 动画 WebP）解码
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>

#include <cstdint>
#include <string>
#include <vector>

// 一帧：像素为 BGRA + 预乘 alpha，尺寸统一为导出尺寸
struct AnimFrame {
    std::vector<uint8_t> pixels;
    int delayMs = 100;
};

struct AnimationData {
    uint32_t index = 0;
    uint32_t srcW = 0, srcH = 0;  // 逻辑画布（原图）尺寸
    uint32_t w = 0, h = 0;        // 每帧像素尺寸（可能已按窗口缩放）
    std::vector<AnimFrame> frames;
    bool ok = false;
    std::wstring error;
    double decodeMs = 0;
};

// 该文件是否可能是多帧动画（只看扩展名，真正的判断交给解码器）
bool MightBeAnimated(const std::wstring& path);

// 解码动画。fitW/fitH 为 0 时按原始尺寸；返回 false 表示"不是动图/解不出来"，
// 调用方应回落到普通静态图路径。
bool DecodeAnimation(IWICImagingFactory* factory, const std::wstring& path, uint32_t fitW,
                     uint32_t fitH, AnimationData& out, int maxFrames = 400,
                     size_t maxBytes = 160ull * 1024 * 1024);
