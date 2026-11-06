// seesee / 看看 — 动图解码实现（GIF 合成 + 动画 WebP 兜底）
#include "animdecode.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

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

bool ReadAll(const std::wstring& path, std::vector<uint8_t>& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                           nullptr);
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

bool GetU32(IWICMetadataQueryReader* reader, const wchar_t* name, UINT& value) {
    if (!reader) return false;
    PROPVARIANT p;
    PropVariantInit(&p);
    bool ok = false;
    if (SUCCEEDED(reader->GetMetadataByName(name, &p))) {
        switch (p.vt) {
            case VT_UI1:
                value = p.bVal;
                ok = true;
                break;
            case VT_UI2:
                value = p.uiVal;
                ok = true;
                break;
            case VT_UI4:
                value = p.ulVal;
                ok = true;
                break;
            case VT_I1:
                value = (UINT)(uint8_t)p.cVal;
                ok = true;
                break;
            case VT_I2:
                value = (UINT)p.iVal;
                ok = true;
                break;
            case VT_I4:
                value = (UINT)p.lVal;
                ok = true;
                break;
            default:
                break;
        }
    }
    PropVariantClear(&p);
    return ok;
}

// src 叠在 dst 上（都是预乘 BGRA）
void BlendPremul(uint8_t* dst, const uint8_t* src, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        const size_t o = i * 4;
        unsigned sa = src[o + 3];
        if (sa == 0) continue;
        if (sa == 255) {
            dst[o] = src[o];
            dst[o + 1] = src[o + 1];
            dst[o + 2] = src[o + 2];
            dst[o + 3] = 255;
            continue;
        }
        unsigned inv = 255u - sa;
        for (int c = 0; c < 3; ++c) {
            unsigned d = dst[o + c];
            unsigned s = src[o + c];
            dst[o + c] = (uint8_t)std::min(255u, s + (d * inv + 127u) / 255u);
        }
        dst[o + 3] = (uint8_t)std::min(255u, sa + (dst[o + 3] * inv + 127u) / 255u);
    }
}

// 预乘 BGRA 双线性缩放（预乘空间插值才是正确的）
void ScalePremul(const uint8_t* src, UINT sw, UINT sh, uint8_t* dst, UINT dw, UINT dh) {
    if (sw == dw && sh == dh) {
        std::memcpy(dst, src, (size_t)dw * dh * 4);
        return;
    }
    const double rx = double(sw) / double(dw);
    const double ry = double(sh) / double(dh);
    for (UINT y = 0; y < dh; ++y) {
        double sy = (double(y) + 0.5) * ry - 0.5;
        if (sy < 0) sy = 0;
        UINT y0 = (UINT)sy;
        if (y0 > sh - 1) y0 = sh - 1;
        UINT y1 = std::min(y0 + 1, sh - 1);
        double fy = sy - double(y0);
        if (fy < 0) fy = 0;
        for (UINT x = 0; x < dw; ++x) {
            double sx = (double(x) + 0.5) * rx - 0.5;
            if (sx < 0) sx = 0;
            UINT x0 = (UINT)sx;
            if (x0 > sw - 1) x0 = sw - 1;
            UINT x1 = std::min(x0 + 1, sw - 1);
            double fx = sx - double(x0);
            if (fx < 0) fx = 0;

            const uint8_t* p00 = src + ((size_t)y0 * sw + x0) * 4;
            const uint8_t* p10 = src + ((size_t)y0 * sw + x1) * 4;
            const uint8_t* p01 = src + ((size_t)y1 * sw + x0) * 4;
            const uint8_t* p11 = src + ((size_t)y1 * sw + x1) * 4;
            uint8_t* d = dst + ((size_t)y * dw + x) * 4;
            const double w00 = (1 - fx) * (1 - fy), w10 = fx * (1 - fy);
            const double w01 = (1 - fx) * fy, w11 = fx * fy;
            for (int c = 0; c < 4; ++c) {
                double v = w00 * p00[c] + w10 * p10[c] + w01 * p01[c] + w11 * p11[c];
                d[c] = (uint8_t)std::lround(std::clamp(v, 0.0, 255.0));
            }
        }
    }
}

std::wstring LowerExt(const std::wstring& path) {
    size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return L"";
    std::wstring ext = path.substr(dot);
    for (auto& c : ext) c = (wchar_t)std::towlower(c);
    return ext;
}

}  // namespace

bool MightBeAnimated(const std::wstring& path) {
    std::wstring ext = LowerExt(path);
    return ext == L".gif" || ext == L".webp" || ext == L".apng" || ext == L".png";
}

bool DecodeAnimation(IWICImagingFactory* factory, const std::wstring& path, uint32_t fitW,
                     uint32_t fitH, AnimationData& out, int maxFrames, size_t maxBytes) {
    double t0 = NowMs();
    if (!factory) {
        out.error = L"WIC 工厂不可用";
        return false;
    }
    std::vector<uint8_t> file;
    if (!ReadAll(path, file)) {
        out.error = L"读取文件失败";
        return false;
    }

    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(file.data(), (DWORD)file.size()))) {
        out.error = L"内存流初始化失败";
        return false;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr,
                                                WICDecodeMetadataCacheOnDemand, &decoder)) ||
        !decoder) {
        out.error = L"解码器不支持该格式";
        return false;
    }

    UINT count = 0;
    if (FAILED(decoder->GetFrameCount(&count)) || count < 2) {
        out.error = L"不是动图";
        return false;
    }

    ComPtr<IWICBitmapFrameDecode> first;
    if (FAILED(decoder->GetFrame(0, &first)) || !first) {
        out.error = L"读取第一帧失败";
        return false;
    }
    UINT W = 0, H = 0;
    if (FAILED(first->GetSize(&W, &H)) || W == 0 || H == 0) {
        out.error = L"无法获取尺寸";
        return false;
    }
    // 画布别太大，防止恶意/异常文件把内存撑爆
    if ((uint64_t)W * H > 8192ull * 8192ull) {
        out.error = L"动图尺寸过大";
        return false;
    }

    out.srcW = W;
    out.srcH = H;

    double scale = 1.0;
    if (fitW && fitH) {
        double s = std::min(double(fitW) / double(W), double(fitH) / double(H));
        if (s < 1.0) scale = s;
    }
    UINT dw = (UINT)std::max<long>(1, (long)std::llround(double(W) * scale));
    UINT dh = (UINT)std::max<long>(1, (long)std::llround(double(H) * scale));
    out.w = dw;
    out.h = dh;

    const size_t canvasBytes = (size_t)W * H * 4;
    std::vector<uint8_t> canvas(canvasBytes, 0);
    std::vector<uint8_t> prev;
    std::vector<uint8_t> framePx;
    std::vector<uint8_t> scaled((size_t)dw * dh * 4);

    UINT limit = count;
    if (maxFrames > 0 && (int)limit > maxFrames) limit = (UINT)maxFrames;

    size_t totalBytes = 0;
    for (UINT i = 0; i < limit; ++i) {
        ComPtr<IWICBitmapFrameDecode> frame;
        if (i == 0) {
            frame = first;
        } else if (FAILED(decoder->GetFrame(i, &frame)) || !frame) {
            continue;
        }

        ComPtr<IWICMetadataQueryReader> md;
        frame->GetMetadataQueryReader(&md);
        UINT left = 0, top = 0, delayUnits = 0, disposal = 0;
        GetU32(md.Get(), L"/imgdesc/Left", left);
        GetU32(md.Get(), L"/imgdesc/Top", top);
        bool hasDelay = GetU32(md.Get(), L"/grctlext/Delay", delayUnits);
        GetU32(md.Get(), L"/grctlext/Disposal", disposal);
        if (!hasDelay) hasDelay = GetU32(md.Get(), L"/ANIM/FrameDelay", delayUnits);

        int delayMs = hasDelay ? (int)delayUnits * 10 : 100;
        if (delayMs < 20) delayMs = 100;  // 和浏览器一致：过短的延时按 100ms
        if (delayMs > 3000) delayMs = 3000;

        UINT fw = 0, fh = 0;
        if (FAILED(frame->GetSize(&fw, &fh)) || fw == 0 || fh == 0) continue;

        // 每帧都新建一个格式转换器：同一个 IWICFormatConverter 反复 Initialize 会失败
        ComPtr<IWICFormatConverter> conv;
        if (FAILED(factory->CreateFormatConverter(&conv)) || !conv) continue;
        if (FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                                    WICBitmapDitherTypeNone, nullptr, 0.0,
                                    WICBitmapPaletteTypeCustom))) {
            continue;
        }
        framePx.resize((size_t)fw * fh * 4);
        if (FAILED(conv->CopyPixels(nullptr, fw * 4, (UINT)framePx.size(), framePx.data()))) {
            continue;
        }

        // disposal==3 需要保存上一帧的画布
        if (disposal == 3 && prev.size() != canvasBytes) prev.resize(canvasBytes);
        if (disposal == 3) std::memcpy(prev.data(), canvas.data(), canvasBytes);

        UINT cropW = (left < W) ? std::min(fw, W - left) : 0;
        UINT cropH = (top < H) ? std::min(fh, H - top) : 0;
        for (UINT y = 0; y < cropH; ++y) {
            const uint8_t* s = framePx.data() + (size_t)y * fw * 4;
            uint8_t* d = canvas.data() + ((size_t)(top + y) * W + left) * 4;
            BlendPremul(d, s, cropW);
        }

        const uint8_t* outPx = canvas.data();
        if (dw != W || dh != H) {
            ScalePremul(canvas.data(), W, H, scaled.data(), dw, dh);
            outPx = scaled.data();
        }

        AnimFrame af;
        af.delayMs = delayMs;
        af.pixels.assign(outPx, outPx + (size_t)dw * dh * 4);
        totalBytes += af.pixels.size();
        out.frames.push_back(std::move(af));

        // 帧间清理
        if (disposal == 2) {
            for (UINT y = 0; y < cropH; ++y) {
                std::memset(canvas.data() + ((size_t)(top + y) * W + left) * 4, 0,
                            (size_t)cropW * 4);
            }
        } else if (disposal == 3) {
            std::memcpy(canvas.data(), prev.data(), canvasBytes);
        }

        if (totalBytes > maxBytes) break;
    }

    if (out.frames.empty()) {
        out.error = L"解码动图失败";
        return false;
    }
    out.ok = true;
    out.decodeMs = NowMs() - t0;
    return true;
}
