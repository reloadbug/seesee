// seesee / 看看 — 图片装载器实现
#include "loader.h"

#include <algorithm>
#include <cwctype>
#include <cmath>

namespace {

double NowMs() {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) * 1000.0 / double(freq.QuadPart);
}

inline wchar_t Lower(wchar_t c) { return (wchar_t)std::towlower(c); }

// 自然排序：IMG_2 < IMG_10，忽略大小写
int NaturalCompare(const std::wstring& a, const std::wstring& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        wchar_t ca = a[i], cb = b[j];
        if (std::iswdigit(ca) && std::iswdigit(cb)) {
            size_t i0 = i, j0 = j;
            while (i < a.size() && std::iswdigit(a[i])) ++i;
            while (j < b.size() && std::iswdigit(b[j])) ++j;
            // 去掉前导 0 后按长度/字典序比较
            size_t ia = i0, jb = j0;
            while (ia < i - 1 && a[ia] == L'0') ++ia;
            while (jb < j - 1 && b[jb] == L'0') ++jb;
            size_t la = i - ia, lb = j - jb;
            if (la != lb) return la < lb ? -1 : 1;
            int cmp = a.compare(ia, la, b, jb, lb);
            if (cmp != 0) return cmp < 0 ? -1 : 1;
        } else {
            wchar_t la = Lower(ca), lb = Lower(cb);
            if (la != lb) return la < lb ? -1 : 1;
            ++i;
            ++j;
        }
    }
    if (i < a.size()) return 1;
    if (j < b.size()) return -1;
    return 0;
}

const wchar_t* kExtensions[] = {
    L".jpg", L".jpeg", L".jpe", L".jfif", L".png", L".bmp", L".dib", L".gif",
    L".webp", L".tif", L".tiff", L".ico", L".jxl", L".avif", L".heic", L".heif",
};

bool ReadAllBytes(const std::wstring& path, std::vector<uint8_t>& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (1LL << 32)) {
        CloseHandle(h);
        return false;
    }
    out.resize((size_t)sz.QuadPart);
    DWORD got = 0;
    BOOL ok = ReadFile(h, out.data(), (DWORD)out.size(), &got, nullptr);
    CloseHandle(h);
    if (!ok || got != out.size()) {
        out.clear();
        return false;
    }
    return true;
}

// 每个工作线程一份 WIC 工厂
IWICImagingFactory* ThreadFactory() {
    thread_local ComPtr<IWICImagingFactory> factory;
    thread_local bool init = false;
    if (!init) {
        init = true;
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER,
                         IID_PPV_ARGS(&factory));
        if (!factory) {
            CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                             IID_PPV_ARGS(&factory));
        }
    }
    return factory.Get();
}

// 读取 EXIF 方向 (1..8)
int ReadOrientation(IWICBitmapFrameDecode* frame) {
    ComPtr<IWICMetadataQueryReader> reader;
    if (FAILED(frame->GetMetadataQueryReader(&reader)) || !reader) return 1;

    PROPVARIANT v;
    PropVariantInit(&v);
    int orientation = 1;
    if (SUCCEEDED(reader->GetMetadataByName(L"/app1/ifd/{ushort=274}", &v))) {
        if (v.vt == VT_UI2) orientation = v.uiVal;
        else if (v.vt == VT_UI4) orientation = (int)v.ulVal;
        else if (v.vt == VT_I2) orientation = v.iVal;
    } else {
        // 少数文件放在子 IFD 里
        PropVariantClear(&v);
        PropVariantInit(&v);
        if (SUCCEEDED(reader->GetMetadataByName(L"/ifd/{ushort=274}", &v))) {
            if (v.vt == VT_UI2) orientation = v.uiVal;
            else if (v.vt == VT_UI4) orientation = (int)v.ulVal;
        }
    }
    PropVariantClear(&v);
    if (orientation < 1 || orientation > 8) orientation = 1;
    return orientation;
}

// 按 EXIF 方向把像素摆正
void ApplyOrientation(std::vector<uint8_t>& px, uint32_t& w, uint32_t& h, int orientation) {
    if (orientation <= 1 || orientation > 8 || w == 0 || h == 0) return;

    const bool swap = (orientation >= 5);  // 5..8 需要转置
    const uint32_t nw = swap ? h : w;
    const uint32_t nh = swap ? w : h;
    std::vector<uint8_t> dst((size_t)nw * nh * 4);
    const uint8_t* s = px.data();
    uint8_t* d = dst.data();

    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            uint32_t dx = x, dy = y;
            switch (orientation) {
                case 2: dx = w - 1 - x; dy = y; break;                 // 水平镜像
                case 3: dx = w - 1 - x; dy = h - 1 - y; break;         // 180°
                case 4: dx = x; dy = h - 1 - y; break;                 // 垂直镜像
                case 5: dx = y; dy = x; break;                         // 转置
                case 6: dx = h - 1 - y; dy = x; break;                 // 顺时针 90°
                case 7: dx = h - 1 - y; dy = w - 1 - x; break;         // 反转置
                case 8: dx = y; dy = w - 1 - x; break;                 // 逆时针 90°
                default: break;
            }
            const uint8_t* sp = s + ((size_t)y * w + x) * 4;
            uint8_t* dp = d + ((size_t)dy * nw + dx) * 4;
            dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2]; dp[3] = sp[3];
        }
    }
    px.swap(dst);
    w = nw;
    h = nh;
}

}  // namespace

bool IsSupportedImageFile(const std::wstring& name) {
    size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    std::wstring ext = name.substr(dot);
    for (auto& c : ext) c = Lower(c);
    for (auto* e : kExtensions) {
        if (ext == e) return true;
    }
    return false;
}

std::vector<ImageFile> ScanImageFolder(const std::wstring& dirPath) {
    std::vector<ImageFile> result;
    if (dirPath.empty()) return result;

    std::wstring pattern = dirPath;
    if (pattern.back() != L'\\' && pattern.back() != L'/') pattern += L'\\';
    pattern += L'*';

    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch,
                                nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return result;

    const std::wstring prefix = pattern.substr(0, pattern.size() - 1);
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring name = fd.cFileName;
        if (name.empty() || name[0] == L'.') continue;
        if (!IsSupportedImageFile(name)) continue;
        ImageFile f;
        f.name = name;
        f.path = prefix + name;
        f.size = (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        result.push_back(std::move(f));
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    std::sort(result.begin(), result.end(), [](const ImageFile& a, const ImageFile& b) {
        return NaturalCompare(a.name, b.name) < 0;
    });
    return result;
}

Loader::~Loader() { Stop(); }

void Loader::Start(int threadCount) {
    if (!threads_.empty()) return;
    stop_ = false;
    if (threadCount < 1) threadCount = 1;
    if (threadCount > 8) threadCount = 8;
    for (int i = 0; i < threadCount; ++i) {
        threads_.emplace_back([this] { WorkerLoop(); });
    }
}

void Loader::Stop() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        stop_ = true;
        queue_.clear();
    }
    cv_.notify_all();
    for (auto& t : threads_) {
        if (t.joinable()) t.join();
    }
    threads_.clear();
}

void Loader::SetFiles(std::vector<ImageFile> files) {
    std::lock_guard<std::mutex> lk(mtx_);
    files_ = std::move(files);
    queue_.clear();
}

size_t Loader::FileCount() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return files_.size();
}

std::wstring Loader::PathAt(uint32_t index) const {
    std::lock_guard<std::mutex> lk(mtx_);
    if (index >= files_.size()) return {};
    return files_[index].path;
}

uint64_t Loader::SizeAt(uint32_t index) const {
    std::lock_guard<std::mutex> lk(mtx_);
    if (index >= files_.size()) return 0;
    return files_[index].size;
}

void Loader::SetFitTarget(uint32_t w, uint32_t h) {
    fitW_ = w ? w : 1920;
    fitH_ = h ? h : 1080;
}

void Loader::SetWanted(const std::vector<uint32_t>& indices) {
    std::lock_guard<std::mutex> lk(mtx_);
    wanted_ = indices;
}

bool Loader::WantedLocked(uint32_t index) const {
    if (wanted_.empty()) return true;
    for (uint32_t i : wanted_) {
        if (i == index) return true;
    }
    return false;
}

void Loader::Request(uint32_t index, int level, int priority) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (files_.empty() || index >= files_.size()) return;
        if (!wanted_.empty() && !WantedLocked(index)) return;
        // 去重：同 index+level 已在队列里就不重复入队
        for (const auto& t : queue_) {
            if (t.index == index && t.level == level) return;
        }
        Task t;
        t.index = index;
        t.level = level;
        t.prio = priority;
        t.seq = ++seq_;
        if (priority == 0) {
            queue_.push_front(t);
            std::stable_sort(queue_.begin(), queue_.begin() + 1,
                             [](const Task&, const Task&) { return false; });
        } else {
            auto it = queue_.begin();
            while (it != queue_.end() && it->prio <= priority) ++it;
            queue_.insert(it, t);
        }
    }
    cv_.notify_one();
}

void Loader::ClearQueue() {
    std::lock_guard<std::mutex> lk(mtx_);
    queue_.clear();
}

void Loader::SetSink(std::function<void(std::shared_ptr<DecodedImage>)> sink) {
    sink_ = std::move(sink);
}

void Loader::WorkerLoop() {
    ThreadFactory();  // 提前初始化 COM + WIC
    for (;;) {
        Task task;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
            if (stop_) return;
            task = queue_.front();
            queue_.pop_front();
            if (!WantedLocked(task.index)) continue;
        }

        auto result = std::make_shared<DecodedImage>();
        std::wstring path = PathAt(task.index);
        Decode(task.index, path, task.level, fitW_.load(), fitH_.load(), *result);

        if (sink_) {
            if (stop_) return;
            sink_(std::move(result));
        }
    }
}

void Loader::Decode(uint32_t index, const std::wstring& path, int level, uint32_t fitW,
                    uint32_t fitH, DecodedImage& out) {
    out.index = index;
    out.level = level;

    IWICImagingFactory* factory = ThreadFactory();
    if (!factory) {
        out.error = L"WIC 初始化失败";
        return;
    }

    // 1) 一次性读进内存 —— 比让 WIC 直接读文件更快，且避免多次 IO
    double t0 = NowMs();
    std::vector<uint8_t> fileBytes;
    if (!ReadAllBytes(path, fileBytes)) {
        out.error = L"无法读取文件";
        return;
    }
    out.readMs = NowMs() - t0;

    double t1 = NowMs();
    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream))) {
        out.error = L"创建流失败";
        return;
    }
    // InitializeFromMemory 不拷贝，fileBytes 在本函数内保持有效
    if (FAILED(stream->InitializeFromMemory(fileBytes.data(), (DWORD)fileBytes.size()))) {
        out.error = L"内存流初始化失败";
        return;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = factory->CreateDecoderFromStream(stream.Get(), nullptr,
                                                  WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr) || !decoder) {
        out.error = L"解码器不支持该格式";
        return;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr) || !frame) {
        out.error = L"读取图片帧失败";
        return;
    }

    UINT sw = 0, sh = 0;
    if (FAILED(frame->GetSize(&sw, &sh)) || sw == 0 || sh == 0) {
        out.error = L"无法获取尺寸";
        return;
    }

    out.orientation = ReadOrientation(frame.Get());

    // 目标尺寸：适应窗口时按比例缩小，绝不放大
    double scale = 1.0;
    if (level == kLevelFit && fitW > 0 && fitH > 0) {
        double sx = double(fitW) / double(sw);
        double sy = double(fitH) / double(sh);
        scale = sx < sy ? sx : sy;
        if (scale > 1.0) scale = 1.0;
    }
    UINT dw = (UINT)std::lround(sw * scale);
    UINT dh = (UINT)std::lround(sh * scale);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;

    std::vector<uint8_t> pixels;
    bool decoded = false;

    // 2) 首选：JPEG 的 DCT 缩放开图（1/2、1/4、1/8 降采样解码，省掉大部分运算）
    ComPtr<IWICBitmapSourceTransform> transform;
    if (SUCCEEDED(frame->QueryInterface(IID_PPV_ARGS(&transform))) && transform) {
        UINT cw = dw, ch = dh;
        if (SUCCEEDED(transform->GetClosestSize(&cw, &ch)) && cw > 0 && ch > 0) {
            WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppPBGRA;
            UINT stride = cw * 4;
            pixels.resize((size_t)stride * ch);
            hr = transform->CopyPixels(nullptr, cw, ch, &fmt, WICBitmapTransformRotate0, stride,
                                       (UINT)pixels.size(), pixels.data());
            if (SUCCEEDED(hr) && fmt == GUID_WICPixelFormat32bppPBGRA) {
                dw = cw;
                dh = ch;
                decoded = true;
            } else if (SUCCEEDED(hr)) {
                // 解码器给了别的格式，做一次小图转换（此时已是缩小后的数据，很便宜）
                ComPtr<IWICBitmap> tmp;
                if (SUCCEEDED(factory->CreateBitmapFromMemory(cw, ch, fmt, stride,
                                                              (UINT)pixels.size(), pixels.data(),
                                                              &tmp))) {
                    ComPtr<IWICFormatConverter> conv;
                    if (SUCCEEDED(factory->CreateFormatConverter(&conv)) &&
                        SUCCEEDED(conv->Initialize(tmp.Get(), GUID_WICPixelFormat32bppPBGRA,
                                                   WICBitmapDitherTypeNone, nullptr, 0,
                                                   WICBitmapPaletteTypeCustom))) {
                        std::vector<uint8_t> converted((size_t)cw * ch * 4);
                        if (SUCCEEDED(conv->CopyPixels(nullptr, cw * 4, (UINT)converted.size(),
                                                       converted.data()))) {
                            pixels.swap(converted);
                            dw = cw;
                            dh = ch;
                            decoded = true;
                        }
                    }
                }
            }
            if (!decoded) pixels.clear();
        }
    }

    // 3) 兜底：完整解码 + 高质量缩放
    if (!decoded) {
        ComPtr<IWICFormatConverter> conv;
        if (FAILED(factory->CreateFormatConverter(&conv)) ||
            FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                                    WICBitmapDitherTypeNone, nullptr, 0,
                                    WICBitmapPaletteTypeCustom))) {
            out.error = L"像素格式转换失败";
            return;
        }
        if (dw != sw || dh != sh) {
            ComPtr<IWICBitmapScaler> scaler;
            if (FAILED(factory->CreateBitmapScaler(&scaler)) ||
                FAILED(scaler->Initialize(conv.Get(), dw, dh, WICBitmapInterpolationModeFant))) {
                out.error = L"缩放失败";
                return;
            }
            pixels.resize((size_t)dw * dh * 4);
            if (FAILED(scaler->CopyPixels(nullptr, dw * 4, (UINT)pixels.size(), pixels.data()))) {
                out.error = L"像素读取失败";
                return;
            }
        } else {
            pixels.resize((size_t)dw * dh * 4);
            if (FAILED(conv->CopyPixels(nullptr, dw * 4, (UINT)pixels.size(), pixels.data()))) {
                out.error = L"像素读取失败";
                return;
            }
        }
    }
    out.decodeMs = NowMs() - t1;

    // 4) EXIF 摆正
    double t2 = NowMs();
    ApplyOrientation(pixels, dw, dh, out.orientation);
    out.orientMs = NowMs() - t2;

    out.srcW = (out.orientation >= 5) ? sh : sw;
    out.srcH = (out.orientation >= 5) ? sw : sh;
    out.w = dw;
    out.h = dh;
    out.pixels = std::move(pixels);
    out.ok = true;
}
