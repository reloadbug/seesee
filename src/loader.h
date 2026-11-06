// seeess / 看看 — 图片装载器 (目录扫描 + WIC 解码 + 后台预加载)
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include "animdecode.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

struct ImageFile {
    std::wstring path;
    std::wstring name;
    uint64_t size = 0;
};

// 支持的扩展名判断（不含点）
bool IsSupportedImageFile(const std::wstring& name);
// 扫描目录，自然排序（IMG_2 < IMG_10），失败返回空
std::vector<ImageFile> ScanImageFolder(const std::wstring& dirPath);

// 解码档位：适应窗口 / 原始分辨率 / 胶片条缩略图
enum DecodeLevel : int { kLevelFit = 0, kLevelFull = 1, kLevelThumb = 2, kLevelCount = 3 };

// 缩略图解码的长边目标（像素）；实际会用 DCT 最接近的档位
constexpr int kThumbLongSide = 256;

struct DecodedImage {
    uint32_t index = 0;
    int level = kLevelFit;
    int orientation = 1;          // EXIF 原始方向值
    uint32_t srcW = 0, srcH = 0;  // 已按 EXIF 方向摆正后的原始尺寸
    uint32_t w = 0, h = 0;        // 解码后像素尺寸
    std::vector<uint8_t> pixels;  // BGRA (premultiplied)
    double readMs = 0, decodeMs = 0, orientMs = 0;
    bool ok = false;
    bool fromPreview = false;  // RAW：用的是文件内嵌的 JPEG 预览
    std::wstring error;
};

class Loader {
public:
    Loader() = default;
    ~Loader();

    void Start(int threadCount);
    void Stop();

    void SetFiles(std::vector<ImageFile> files);
    size_t FileCount() const;
    std::wstring PathAt(uint32_t index) const;
    uint64_t SizeAt(uint32_t index) const;

    // 适应窗口档位的解码目标尺寸（物理像素）
    void SetFitTarget(uint32_t w, uint32_t h);

    // 只保留这些索引的待办任务（快速翻页时丢弃过期请求）
    void SetWanted(const std::vector<uint32_t>& indices);

    void Request(uint32_t index, int level, int priority);
    void ClearQueue();

    // 动图：解码全部帧（GIF / 动画 WebP）；结果走 SetAnimSink
    void RequestAnimation(uint32_t index, uint32_t fitW, uint32_t fitH, int priority = 0);

    void SetSink(std::function<void(std::shared_ptr<DecodedImage>)> sink);
    void SetAnimSink(std::function<void(std::shared_ptr<AnimationData>)> sink);

    static void Decode(uint32_t index, const std::wstring& path, int level,
                       uint32_t fitW, uint32_t fitH, DecodedImage& out);

private:
    struct Task {
        uint32_t index = 0;
        int level = kLevelFit;
        int prio = 0;
        uint64_t seq = 0;
        bool anim = false;  // 动图任务（level 无意义）
        uint32_t animFitW = 0, animFitH = 0;  // 动图解码目标尺寸（0 = 原始）
    };

    void WorkerLoop();
    bool WantedLocked(uint32_t index) const;

    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<Task> queue_;
    std::vector<ImageFile> files_;
    std::vector<uint32_t> wanted_;
    std::vector<std::thread> threads_;
    std::atomic<bool> stop_{false};
    std::atomic<uint32_t> fitW_{1920};
    std::atomic<uint32_t> fitH_{1080};
    std::function<void(std::shared_ptr<DecodedImage>)> sink_;
    std::function<void(std::shared_ptr<AnimationData>)> animSink_;
    uint64_t seq_ = 0;
};
