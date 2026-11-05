// seesee / 看看 — 主窗口与渲染实现
#include "app.h"
#include "assoc.h"

#include <d2d1helper.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <windowsx.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <cwctype>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "dwmapi.lib")

namespace {

const wchar_t* kClassName = L"SeeSeeMainWindow";
const UINT WM_APP_DECODED = WM_APP + 1;

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif

// 设计令牌
const D2D1_COLOR_F kColorBg = D2D1::ColorF(0x0F1114);
const D2D1_COLOR_F kColorTitleBg = D2D1::ColorF(0x16191E);
const D2D1_COLOR_F kColorSurface = D2D1::ColorF(0x1B1E23);
const D2D1_COLOR_F kColorBorder = D2D1::ColorF(0x2A2F36);
const D2D1_COLOR_F kColorText = D2D1::ColorF(0xE8EAED);
const D2D1_COLOR_F kColorText2 = D2D1::ColorF(0x9AA2AE);
const D2D1_COLOR_F kColorText3 = D2D1::ColorF(0x6B7480);
const D2D1_COLOR_F kColorAccent = D2D1::ColorF(0x5B9CFF);
const D2D1_COLOR_F kColorHover = D2D1::ColorF(0xFFFFFF, 0.08f);
const D2D1_COLOR_F kColorPressed = D2D1::ColorF(0xFFFFFF, 0.14f);
const D2D1_COLOR_F kColorClose = D2D1::ColorF(0xE81123);

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

double ProcessStartMs() {
    FILETIME create, exitT, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &create, &exitT, &kernel, &user)) return 0;
    ULARGE_INTEGER u;
    u.LowPart = create.dwLowDateTime;
    u.HighPart = create.dwHighDateTime;
    // FILETIME 是 1601 纪元的 100ns
    static const ULONGLONG kEpochDiff = 116444736000000000ULL;  // 1970 与 1601 之间的 100ns 数
    return double(u.QuadPart - kEpochDiff) / 10000.0;           // ms since epoch
}

std::wstring ExtUpper(const std::wstring& path) {
    size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return L"?";
    std::wstring e = path.substr(dot + 1);
    for (auto& c : e) c = (wchar_t)std::towupper(c);
    if (e == L"JPEG") e = L"JPG";
    if (e == L"TIFF") e = L"TIF";
    return e;
}

std::wstring FileNameOf(const std::wstring& path) {
    size_t p = path.find_last_of(L"\\/");
    return p == std::wstring::npos ? path : path.substr(p + 1);
}

std::wstring HumanSize(uint64_t bytes) {
    wchar_t buf[64];
    if (bytes < 1024) swprintf_s(buf, L"%llu B", bytes);
    else if (bytes < 1024ull * 1024) swprintf_s(buf, L"%.0f KB", bytes / 1024.0);
    else if (bytes < 1024ull * 1024 * 1024) swprintf_s(buf, L"%.1f MB", bytes / (1024.0 * 1024.0));
    else swprintf_s(buf, L"%.2f GB", bytes / (1024.0 * 1024.0 * 1024.0));
    return buf;
}

std::string FmtKey(float size, bool mono, int align) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%.1f|%d|%d", size, mono ? 1 : 0, align);
    return buf;
}

// 我们按目标尺寸做解码级缩放（JPEG DCT 缩放），上屏时的插值只需线性即可
D2D1_BITMAP_INTERPOLATION_MODE InterpFor(double z) {
    (void)z;
    return D2D1_BITMAP_INTERPOLATION_MODE_LINEAR;
}

}  // namespace

App& App::Instance() {
    static App app;
    return app;
}

// ---------------------------------------------------------------- 启动

int App::Run(HINSTANCE hInst, const std::wstring& initialFile, bool benchMode,
             const std::wstring& shotPath) {
    hinst_ = hInst;
    benchMode_ = benchMode;
    shotPath_ = shotPath;
    tStart_ = NowMs();

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2dFactory_.ReleaseAndGetAddressOf())))
        return 1;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dwrite_.ReleaseAndGetAddressOf()))))
        return 1;
    tFactory_ = NowMs();

    Settings& cfg = Settings::Get();
    loader_.Start(cfg.workerThreads);
    loader_.SetSink([this](std::shared_ptr<DecodedImage> img) {
        {
            std::lock_guard<std::mutex> lk(pendingMtx_);
            pending_.push_back(std::move(img));
        }
        if (hwnd_) PostMessageW(hwnd_, WM_APP_DECODED, 0, 0);
    });

    // 只建窗不显示：先把解码请求发出去（worker 线程跑），再显示窗口去建 D2D 设备，两段并行
    if (!CreateMainWindow(hInst)) return 1;
    tWindowCreated_ = NowMs();

    if (!initialFile.empty()) {
        OpenPath(initialFile);
    }

    ShowMainWindow();
    tWindowShown_ = NowMs();

    if (initialFile.empty()) InvalidateRect(hwnd_, nullptr, FALSE);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    Shutdown();
    return 0;
}

RECT App::DefaultWindowRect() const {
    // 窗口默认占工作区的 82% × 86%，居中
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int w = (int)((work.right - work.left) * 0.82);
    int h = (int)((work.bottom - work.top) * 0.86);
    RECT rc{};
    rc.left = work.left + ((work.right - work.left) - w) / 2;
    rc.top = work.top + ((work.bottom - work.top) - h) / 2;
    rc.right = rc.left + w;
    rc.bottom = rc.top + h;
    return rc;
}

bool App::CreateMainWindow(HINSTANCE hInst) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = StaticWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kClassName;
    wc.hIcon = LoadIconW(hInst, L"APPICON");
    wc.hIconSm = wc.hIcon;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    RECT rc = DefaultWindowRect();
    winRect_ = rc;

    hwnd_ = CreateWindowExW(0, kClassName, L"看看 SeeSee", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                            rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr,
                            hInst, this);
    if (!hwnd_) return false;

    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    int corner = 2;  // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    MARGINS margins{1, 1, 1, 1};
    DwmExtendFrameIntoClientArea(hwnd_, &margins);

    DragAcceptFiles(hwnd_, TRUE);
    return true;
}

void App::ShowMainWindow() {
    if (!hwnd_) return;
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
}

void App::Shutdown() {
    loader_.Stop();
    if (hwnd_) {
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        hwnd_ = nullptr;
    }
}

LRESULT CALLBACK App::StaticWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App* self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = reinterpret_cast<App*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    return self->WndProc(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------- 打开

bool App::OpenPath(const std::wstring& path, bool keepView) {
    std::wstring full = path;
    // 规范化为绝对路径
    wchar_t buf[MAX_PATH * 2];
    if (GetFullPathNameW(path.c_str(), MAX_PATH * 2, buf, nullptr)) full = buf;

    std::wstring dir = full;
    size_t p = dir.find_last_of(L"\\/");
    if (p == std::wstring::npos) return false;
    dir = dir.substr(0, p);

    DWORD attr = GetFileAttributesW(full.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return false;
    if (attr & FILE_ATTRIBUTE_DIRECTORY) return false;

    double tScan0 = NowMs();
    auto files = ScanImageFolder(dir);
    scanMs_ = NowMs() - tScan0;
    if (files.empty()) return false;

    uint32_t idx = 0;
    bool found = false;
    for (uint32_t i = 0; i < files.size(); ++i) {
        if (_wcsicmp(files[i].path.c_str(), full.c_str()) == 0) {
            idx = i;
            found = true;
            break;
        }
    }
    if (!found) {
        ImageFile f;
        f.path = full;
        f.name = FileNameOf(full);
        files.insert(files.begin(), f);
        idx = 0;
    }

    loader_.SetFiles(std::move(files));
    benchFileCount_ = (uint32_t)loader_.FileCount();
    if (!keepView) {
        cache_.clear();
        zoomMode_ = ZoomMode::Fit;
        offset_ = D2D1::Point2F(0.f, 0.f);
    }
    curIndex_ = idx;
    haveImage_ = false;
    errorText_.clear();
    ResetViewToFit();
    UpdateRequests();
    tOpen_ = NowMs();
    InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
}

void App::ReloadCurrent() {
    haveImage_ = false;
    errorText_.clear();
    ResetViewToFit();
    UpdateRequests();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::GoTo(int64_t index) {
    size_t n = loader_.FileCount();
    if (n == 0) return;
    if (index < 0) index = 0;
    if (index >= (int64_t)n) index = (int64_t)n - 1;
    if ((uint32_t)index == curIndex_ && haveImage_) return;
    curIndex_ = (uint32_t)index;
    requestedLevel_ = (CurrentZoom() >= 0.95) ? kLevelFull : kLevelFit;
    ResetViewToFit();
    UpdateRequests();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::NextImage() { GoTo((int64_t)curIndex_ + 1); }
void App::PrevImage() { GoTo((int64_t)curIndex_ - 1); }

void App::UpdateRequests() {
    if (loader_.FileCount() == 0) return;
    RECT area = AreaRect();
    loader_.SetFitTarget((uint32_t)std::max<LONG>(1, area.right - area.left),
                         (uint32_t)std::max<LONG>(1, area.bottom - area.top));

    Settings& cfg = Settings::Get();
    int level = (CurrentZoom() >= 0.95) ? kLevelFull : kLevelFit;
    requestedLevel_ = level;

    std::vector<uint32_t> wanted;
    wanted.push_back(curIndex_);
    int n46 = std::max(cfg.preloadPrev, cfg.preloadNext);
    for (int i = 1; i <= n46; ++i) {
        if ((int64_t)curIndex_ + i < (int64_t)loader_.FileCount())
            wanted.push_back(curIndex_ + i);
        if ((int64_t)curIndex_ - i >= 0) wanted.push_back(curIndex_ - i);
    }
    loader_.SetWanted(wanted);

    loader_.Request(curIndex_, level, 0);
    if (level == kLevelFull) loader_.Request(curIndex_, kLevelFit, 0);

    for (int i = 1; i <= cfg.preloadNext; ++i) {
        if (curIndex_ + i < loader_.FileCount()) loader_.Request(curIndex_ + i, kLevelFit, 1);
    }
    for (int i = 1; i <= cfg.preloadPrev; ++i) {
        if ((int64_t)curIndex_ - i >= 0) loader_.Request(curIndex_ - i, kLevelFit, 1);
    }
}

void App::OnDecodedReady() {
    std::vector<std::shared_ptr<DecodedImage>> items;
    {
        std::lock_guard<std::mutex> lk(pendingMtx_);
        items.swap(pending_);
    }
    for (auto& img : items) {
        if (!img) continue;
        if (!img->ok) {
            if (img->index == curIndex_) {
                errorText_ = img->error.empty() ? L"无法打开此图片" : img->error;
                loading_ = false;
                haveImage_ = false;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            continue;
        }
        if (!EnsureDevice()) continue;

        uint64_t key = ((uint64_t)img->index << 2) | (uint64_t)img->level;
        ComPtr<ID2D1Bitmap> bmp;
        double tUp0 = NowMs();
        HRESULT hr = rt_->CreateBitmap(
            D2D1::SizeU(img->w, img->h), img->pixels.data(), img->w * 4,
            D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                                     D2D1_ALPHA_MODE_PREMULTIPLIED)),
            &bmp);
        double uploadMs = NowMs() - tUp0;
        if (FAILED(hr) || !bmp) continue;

        CacheEntry e;
        e.bmp = bmp;
        e.srcW = img->srcW;
        e.srcH = img->srcH;
        e.decW = img->w;
        e.decH = img->h;
        e.level = img->level;
        e.lastUse = ++useCounter_;
        cache_[key] = std::move(e);

        if (img->index == curIndex_) {
            haveImage_ = true;
            loading_ = false;
            errorText_.clear();
            readMs_ = img->readMs;
            decMs_ = img->decodeMs + img->orientMs;
            uploadMs_ = uploadMs;
        }
        // 位图已上传 GPU，CPU 像素可以释放
        img->pixels.clear();
        img->pixels.shrink_to_fit();
    }

    // 淘汰过多的缓存
    const size_t kMaxEntries = 12;
    while (cache_.size() > kMaxEntries) {
        auto worst = cache_.begin();
        for (auto it = cache_.begin(); it != cache_.end(); ++it) {
            if (it->second.lastUse < worst->second.lastUse) worst = it;
        }
        cache_.erase(worst);
    }

    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ---------------------------------------------------------------- 视图

RECT App::AreaRect() const {
    RECT rc{};
    if (!hwnd_) return rc;
    GetClientRect(hwnd_, &rc);
    rc.top += (LONG)titleH_;
    return rc;
}

D2D1_POINT_2F App::AreaCenter() const {
    RECT a = AreaRect();
    return D2D1::Point2F((a.left + a.right) * 0.5f, (a.top + a.bottom) * 0.5f);
}

double App::FitZoom() const {
    RECT a = AreaRect();
    float aw = (float)(a.right - a.left);
    float ah = (float)(a.bottom - a.top);
    if (aw < 8) aw = 8;
    if (ah < 8) ah = 8;

    // 用当前索引的源尺寸；没有就用缓存里的
    uint32_t sw = 0, sh = 0;
    for (int lvl = 0; lvl < kLevelCount && sw == 0; ++lvl) {
        auto it = cache_.find(((uint64_t)curIndex_ << 2) | (uint64_t)lvl);
        if (it != cache_.end()) {
            sw = it->second.srcW;
            sh = it->second.srcH;
        }
    }
    if (sw == 0 || sh == 0) return 1.0;

    double z = std::min(double(aw) / sw, double(ah) / sh);
    if (!Settings::Get().fitUpscale && z > 1.0) z = 1.0;
    return z;
}

double App::CurrentZoom() const {
    return zoomMode_ == ZoomMode::Fit ? FitZoom() : zoom_;
}

void App::ResetViewToFit() {
    zoomMode_ = ZoomMode::Fit;
    offset_ = D2D1::Point2F(0.f, 0.f);
}

void App::ToggleFullscreen() {
    if (!hwnd_) return;
    fullscreen_ = !fullscreen_;
    if (fullscreen_) {
        GetWindowRect(hwnd_, &savedRect_);
        savedStyle_ = GetWindowLongW(hwnd_, GWL_STYLE);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongW(hwnd_, GWL_STYLE, savedStyle_ & ~(WS_CAPTION | WS_THICKFRAME | WS_MAXIMIZEBOX));
        SetWindowPos(hwnd_, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED);
        titleH_ = 0;
    } else {
        SetWindowLongW(hwnd_, GWL_STYLE, savedStyle_);
        SetWindowPos(hwnd_, nullptr, savedRect_.left, savedRect_.top, savedRect_.right - savedRect_.left,
                     savedRect_.bottom - savedRect_.top, SWP_FRAMECHANGED | SWP_NOZORDER);
        titleH_ = std::round(34.f * dpiScale_);
    }
    UpdateRequests();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::SetZoomAnchored(double newZoom, D2D1_POINT_2F anchor) {
    double oldZ = CurrentZoom();
    if (oldZ <= 0.0001) oldZ = 1.0;
    newZoom = std::clamp(newZoom, 0.02, 32.0);

    D2D1_POINT_2F ac = AreaCenter();
    // 锚点下的图片坐标
    double px = (anchor.x - (ac.x + offset_.x)) / oldZ;
    double py = (anchor.y - (ac.y + offset_.y)) / oldZ;

    zoomMode_ = ZoomMode::Actual;
    zoom_ = newZoom;

    offset_.x = (float)(anchor.x - ac.x - px * newZoom);
    offset_.y = (float)(anchor.y - ac.y - py * newZoom);
    ClampOffset();
}

void App::ClampOffset() {
    RECT a = AreaRect();
    float aw = (float)(a.right - a.left);
    float ah = (float)(a.bottom - a.top);
    double z = CurrentZoom();

    uint32_t sw = 0, sh = 0;
    for (int lvl = 0; lvl < kLevelCount && sw == 0; ++lvl) {
        auto it = cache_.find(((uint64_t)curIndex_ << 2) | (uint64_t)lvl);
        if (it != cache_.end()) {
            sw = it->second.srcW;
            sh = it->second.srcH;
        }
    }
    if (sw == 0 || sh == 0) {
        offset_ = D2D1::Point2F(0.f, 0.f);
        return;
    }

    double dispW = sw * z;
    double dispH = sh * z;
    double maxX = std::max(0.0, (dispW - aw) * 0.5);
    double maxY = std::max(0.0, (dispH - ah) * 0.5);
    offset_.x = (float)std::clamp<double>(offset_.x, -maxX, maxX);
    offset_.y = (float)std::clamp<double>(offset_.y, -maxY, maxY);
}

// ---------------------------------------------------------------- 输入

LRESULT App::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_NCCALCSIZE: {
            if (wp) {
                if (fullscreen_) return 0;
                auto* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
                if (IsZoomed(hwnd)) {
                    // 最大化时贴合工作区，避免边框溢出屏幕
                    MONITORINFO mi{sizeof(mi)};
                    if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi))
                        p->rgrc[0] = mi.rcWork;
                }
                return 0;
            }
            break;
        }

        case WM_NCHITTEST: {
            if (fullscreen_) return HTCLIENT;
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            RECT wr{};
            GetWindowRect(hwnd, &wr);
            const int b = (int)(6 * dpiScale_);
            const bool left = pt.x < wr.left + b;
            const bool right = pt.x >= wr.right - b;
            const bool top = pt.y < wr.top + b;
            const bool bottom = pt.y >= wr.bottom - b;
            if (!IsZoomed(hwnd)) {
                if (top && left) return HTTOPLEFT;
                if (top && right) return HTTOPRIGHT;
                if (bottom && left) return HTBOTTOMLEFT;
                if (bottom && right) return HTBOTTOMRIGHT;
                if (left) return HTLEFT;
                if (right) return HTRIGHT;
                if (top) return HTTOP;
                if (bottom) return HTBOTTOM;
            }
            POINT c = pt;
            ScreenToClient(hwnd, &c);
            if (c.y < (int)titleH_) {
                if (HitTestScreen(pt) > kBtnNone) return HTCLIENT;
                return HTCAPTION;
            }
            return HTCLIENT;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            Paint();
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_SIZE: {
            UpdateRequests();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_DPICHANGED: {
            dpiScale_ = (float)HIWORD(wp) / 96.0f;
            titleH_ = std::round(34.f * dpiScale_);
            RECT* r = reinterpret_cast<RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            UpdateRequests();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            mmi->ptMinTrackSize.x = (LONG)(420 * dpiScale_);
            mmi->ptMinTrackSize.y = (LONG)(320 * dpiScale_);
            return 0;
        }

        case WM_APP_DECODED:
            OnDecodedReady();
            return 0;

        case WM_MOUSEMOVE: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (panning_) {
                POINT cur{pt.x, pt.y};
                offset_.x = panOffsetStart_.x + (float)(cur.x - panStart_.x);
                offset_.y = panOffsetStart_.y + (float)(cur.y - panStart_.y);
                ClampOffset();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            int h = HitTest(pt);
            if (h != hover_) {
                hover_ = h;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            return 0;
        }

        case WM_MOUSELEAVE: {
            if (hover_ != kBtnNone) {
                hover_ = kBtnNone;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        case WM_LBUTTONDOWN: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            SetFocus(hwnd);
            int id = HitTest(pt);
            if (id != kBtnNone) {
                pressed_ = id;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            // 拖动平移
            if (haveImage_) {
                panning_ = true;
                panStart_ = pt;
                panOffsetStart_ = offset_;
                SetCapture(hwnd);
            }
            return 0;
        }

        case WM_LBUTTONUP: {
            if (panning_) {
                panning_ = false;
                ReleaseCapture();
                return 0;
            }
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            int id = HitTest(pt);
            int was = pressed_;
            pressed_ = kBtnNone;
            if (id != kBtnNone && id == was) {
                switch (id) {
                    case kBtnPrev:
                    case kBtnArrowLeft: PrevImage(); break;
                    case kBtnNext:
                    case kBtnArrowRight: NextImage(); break;
                    case kBtnZoomIn:
                        SetZoomAnchored(CurrentZoom() * 1.25, AreaCenter());
                        UpdateRequests();
                        break;
                    case kBtnZoomOut:
                        SetZoomAnchored(CurrentZoom() / 1.25, AreaCenter());
                        UpdateRequests();
                        break;
                    case kBtnFit: ResetViewToFit(); UpdateRequests(); break;
                    case kBtnActual:
                        zoomMode_ = ZoomMode::Actual;
                        zoom_ = 1.0;
                        offset_ = D2D1::Point2F(0.f, 0.f);
                        UpdateRequests();
                        break;
                    case kBtnFullscreen:
                        ToggleFullscreen();
                        break;
                    case kBtnMin: ShowWindow(hwnd, SW_MINIMIZE); break;
                    case kBtnMax:
                        ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
                        break;
                    case kBtnClose: PostMessageW(hwnd, WM_CLOSE, 0, 0); break;
                    default: break;
                }
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            bool ctrl = (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL) != 0;
            if (ctrl || !Settings::Get().wheelNavigates) {
                POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                ScreenToClient(hwnd, &pt);
                double factor = delta > 0 ? 1.15 : 1.0 / 1.15;
                SetZoomAnchored(CurrentZoom() * factor, D2D1::Point2F((float)pt.x, (float)pt.y));
                UpdateRequests();
            } else {
                if (delta > 0) PrevImage();
                else NextImage();
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_LBUTTONDBLCLK: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (HitTest(pt) == kBtnNone && pt.y > (LONG)titleH_) {
                if (zoomMode_ == ZoomMode::Fit) {
                    SetZoomAnchored(1.0, D2D1::Point2F((float)pt.x, (float)pt.y));
                } else {
                    ResetViewToFit();
                }
                UpdateRequests();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        case WM_KEYDOWN: {
            switch (wp) {
                case VK_RIGHT:
                case VK_DOWN:
                case VK_NEXT: NextImage(); return 0;
                case VK_LEFT:
                case VK_UP:
                case VK_PRIOR: PrevImage(); return 0;
                case VK_HOME: GoTo(0); return 0;
                case VK_END: GoTo((int64_t)loader_.FileCount() - 1); return 0;
                case VK_ESCAPE:
                    if (fullscreen_) {
                        ToggleFullscreen();
                    } else {
                        PostMessageW(hwnd, WM_CLOSE, 0, 0);
                    }
                    return 0;
                case VK_F11:
                    ToggleFullscreen();
                    return 0;
                case VK_OEM_PLUS:
                case VK_ADD: {
                    SetZoomAnchored(CurrentZoom() * 1.25, AreaCenter());
                    UpdateRequests();
                    return 0;
                }
                case VK_OEM_MINUS:
                case VK_SUBTRACT: {
                    SetZoomAnchored(CurrentZoom() / 1.25, AreaCenter());
                    UpdateRequests();
                    return 0;
                }
                case '0': ResetViewToFit(); UpdateRequests(); return 0;
                case '1':
                    zoomMode_ = ZoomMode::Actual;
                    zoom_ = 1.0;
                    offset_ = D2D1::Point2F(0.f, 0.f);
                    UpdateRequests();
                    return 0;
                case 'O':
                    if (GetKeyState(VK_CONTROL) & 0x8000) ShowOpenDialog();
                    return 0;
                case 'D':
                    // Ctrl+D —— 一键设为默认看图工具
                    if (GetKeyState(VK_CONTROL) & 0x8000) {
                        std::wstring regFile;
                        if (assoc::Register(&regFile)) {
                            assoc::OpenDefaultAppsSettings();
                        } else if (!regFile.empty()) {
                            std::wstring text = L"注册表写入被拒绝（可能被安全软件拦截）。\n\n"
                                               L"已生成关联文件，双击导入即可完成关联：\n" +
                                               regFile;
                            MessageBoxW(hwnd, text.c_str(), L"看看 SeeSee", MB_OK | MB_ICONINFORMATION);
                        } else {
                            MessageBoxW(hwnd, L"注册失败：无法写入注册表，也无法生成 .reg 文件。",
                                        L"看看 SeeSee", MB_OK | MB_ICONERROR);
                        }
                    }
                    return 0;
                default: break;
            }
            break;
        }

        case WM_DROPFILES: {
            HDROP drop = (HDROP)wp;
            UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            if (count > 0) {
                wchar_t path[MAX_PATH * 2];
                if (DragQueryFileW(drop, 0, path, MAX_PATH * 2)) OpenPath(path);
            }
            DragFinish(drop);
            return 0;
        }

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            loader_.Stop();
            DiscardDevice();
            if (memDC_) {
                DeleteDC(memDC_);
                memDC_ = nullptr;
            }
            hwnd_ = nullptr;
            PostQuitMessage(0);
            return 0;

        default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------- 渲染

bool App::EnsureDevice() {
    if (rt_) return true;
    if (!d2dFactory_ || !hwnd_) return false;

    auto props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
    if (FAILED(d2dFactory_->CreateDCRenderTarget(&props, rt_.ReleaseAndGetAddressOf())) || !rt_)
        return false;

    HDC screen = GetDC(hwnd_);
    memDC_ = CreateCompatibleDC(screen);
    ReleaseDC(hwnd_, screen);
    return memDC_ != nullptr;
}

// 按客户区尺寸（物理像素）准备 32bpp 顶朝下 DIB
bool App::EnsureBackBuffer(int w, int h) {
    if (w < 1 || h < 1) return false;
    if (dib_ && bufSize_.cx == w && bufSize_.cy == h) return true;

    if (dib_) {
        SelectObject(memDC_, nullptr);
        DeleteObject(dib_);
        dib_ = nullptr;
    }
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // 顶朝下
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    dib_ = CreateDIBSection(memDC_, &bi, DIB_RGB_COLORS, &dibBits_, nullptr, 0);
    if (!dib_) { bufSize_ = {0, 0}; return false; }
    SelectObject(memDC_, dib_);
    bufSize_ = {w, h};
    return true;
}

void App::DiscardDevice() {
    cache_.clear();
    brushes_.clear();
    if (dib_) {
        SelectObject(memDC_, nullptr);
        DeleteObject(dib_);
        dib_ = nullptr;
    }
    dibBits_ = nullptr;
    bufSize_ = {0, 0};
    rt_.Reset();
}

ID2D1SolidColorBrush* App::Brush(D2D1_COLOR_F c) {
    if (!rt_) return nullptr;
    uint32_t key = (uint32_t)((uint8_t)(c.r * 255) << 24) | ((uint32_t)(uint8_t)(c.g * 255) << 16) |
                   ((uint32_t)(uint8_t)(c.b * 255) << 8) | (uint32_t)(uint8_t)(c.a * 255);
    auto it = brushes_.find(key);
    if (it != brushes_.end()) return it->second.Get();
    ComPtr<ID2D1SolidColorBrush> b;
    if (FAILED(rt_->CreateSolidColorBrush(c, &b)) || !b) return nullptr;
    brushes_[key] = b;
    return b.Get();
}

IDWriteTextFormat* App::Format(float sizePx, bool mono, int align) {
    std::string key = FmtKey(sizePx, mono, align);
    auto it = formats_.find(key);
    if (it != formats_.end()) return it->second.Get();
    if (!dwrite_) return nullptr;

    const wchar_t* family = mono ? L"Cascadia Mono" : L"Segoe UI Variable Text";
    ComPtr<IDWriteTextFormat> fmt;
    HRESULT hr = dwrite_->CreateTextFormat(family, nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                           DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                           sizePx, L"zh-CN", &fmt);
    if (FAILED(hr) || !fmt) {
        family = mono ? L"Consolas" : L"Segoe UI";
        if (FAILED(dwrite_->CreateTextFormat(family, nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                             sizePx, L"zh-CN", &fmt)))
            return nullptr;
    }
    fmt->SetTextAlignment(align == 1 ? DWRITE_TEXT_ALIGNMENT_TRAILING
                                     : (align == 2 ? DWRITE_TEXT_ALIGNMENT_CENTER
                                                   : DWRITE_TEXT_ALIGNMENT_LEADING));
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    formats_[key] = fmt;
    return fmt.Get();
}

float App::MeasureText(const std::wstring& s, float sizePx, bool mono) {
    if (!dwrite_ || s.empty()) return 0.f;
    IDWriteTextFormat* fmt = Format(sizePx, mono, 0);
    if (!fmt) return 0.f;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite_->CreateTextLayout(s.c_str(), (UINT32)s.size(), fmt, 4096.f, 4096.f,
                                                &layout)) ||
        !layout)
        return 0.f;
    DWRITE_TEXT_METRICS m{};
    if (FAILED(layout->GetMetrics(&m))) return 0.f;
    return m.width;
}

void App::Text(const std::wstring& s, float x, float y, float w, float h, float sizePx,
               ID2D1Brush* brush, int align) {
    if (!rt_ || s.empty() || !brush) return;
    IDWriteTextFormat* fmt = Format(sizePx, false, align);
    if (!fmt) return;
    D2D1_RECT_F rc = D2D1::RectF(x, y, x + w, y + h);
    rt_->DrawTextW(s.c_str(), (UINT32)s.size(), fmt, rc, brush,
                   D2D1_DRAW_TEXT_OPTIONS_CLIP | D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
}

void App::FillRoundRect(D2D1_RECT_F rc, float radius, ID2D1Brush* brush) {
    if (!rt_ || !brush) return;
    rt_->FillRoundedRectangle(D2D1::RoundedRect(rc, radius, radius), brush);
}

void App::StrokeRoundRect(D2D1_RECT_F rc, float radius, ID2D1Brush* brush, float width) {
    if (!rt_ || !brush) return;
    rt_->DrawRoundedRectangle(D2D1::RoundedRect(rc, radius, radius), brush, width);
}

void App::AddHit(D2D1_RECT_F rc, int id, bool circular) {
    Hit h;
    h.rc = rc;
    h.id = id;
    h.circular = circular;
    hits_.push_back(h);
}

int App::HitTest(POINT pt) const {
    float x = (float)pt.x, y = (float)pt.y;
    for (const auto& h : hits_) {
        if (x >= h.rc.left && x <= h.rc.right && y >= h.rc.top && y <= h.rc.bottom) {
            if (h.circular) {
                float cx = (h.rc.left + h.rc.right) * 0.5f;
                float cy = (h.rc.top + h.rc.bottom) * 0.5f;
                float r = (h.rc.right - h.rc.left) * 0.5f;
                float dx = x - cx, dy = y - cy;
                if (dx * dx + dy * dy > r * r) continue;
            }
            return h.id;
        }
    }
    return kBtnNone;
}

int App::HitTestScreen(POINT screenPt) const {
    if (!hwnd_) return kBtnNone;
    POINT c = screenPt;
    ScreenToClient(hwnd_, &c);
    return HitTest(c);
}

void App::DrawImageLayer() {
    RECT a = AreaRect();
    D2D1_POINT_2F ac = AreaCenter();
    double z = CurrentZoom();

    uint64_t keyFit = ((uint64_t)curIndex_ << 2) | (uint64_t)kLevelFit;
    uint64_t keyFull = ((uint64_t)curIndex_ << 2) | (uint64_t)kLevelFull;
    uint64_t preferred = (requestedLevel_ == kLevelFull) ? keyFull : keyFit;
    uint64_t other = (preferred == keyFit) ? keyFull : keyFit;

    CacheEntry* e = nullptr;
    auto it = cache_.find(preferred);
    if (it != cache_.end()) e = &it->second;
    else {
        it = cache_.find(other);
        if (it != cache_.end()) e = &it->second;
    }
    if (e) e->lastUse = ++useCounter_;

    if (!e || !e->bmp) {
        // 空状态 / 载入中
        ID2D1SolidColorBrush* br = Brush(D2D1::ColorF(0x6B7480, 0.9f));
        if (!errorText_.empty()) {
            Text(errorText_, (float)a.left, (float)(a.top + a.bottom) / 2 - 20,
                 (float)(a.right - a.left), 40, 14 * dpiScale_, Brush(kColorText2), 2);
        } else if (loader_.FileCount() > 0) {
            Text(L"载入中…", (float)a.left, (float)(a.top + a.bottom) / 2 - 20,
                 (float)(a.right - a.left), 40, 13 * dpiScale_, br, 2);
        } else {
            Text(L"拖入图片，或按 Ctrl+O 打开", (float)a.left,
                 (float)(a.top + a.bottom) / 2 - 20, (float)(a.right - a.left), 40,
                 14 * dpiScale_, br, 2);
        }
        return;
    }

    // 目标矩形：按源图尺寸 * 缩放；高度用解码位图的宽高比校正，避免轻微拉伸
    float aspect = (float)e->decH / (float)e->decW;
    float destW = (float)(e->srcW * z);
    float destH = destW * aspect;
    float cx = ac.x + offset_.x;
    float cy = ac.y + offset_.y;
    D2D1_RECT_F dest = D2D1::RectF(cx - destW * 0.5f, cy - destH * 0.5f, cx + destW * 0.5f,
                                   cy + destH * 0.5f);

    rt_->SetTransform(D2D1::Matrix3x2F::Identity());
    rt_->DrawBitmap(e->bmp.Get(), dest, 1.0f, InterpFor(z), nullptr);
}

void App::RenderAndBlit() {
    RECT client{};
    GetClientRect(hwnd_, &client);
    if (!EnsureBackBuffer(client.right, client.bottom)) return;

    hits_.clear();
    rt_->BindDC(memDC_, &client);

    rt_->BeginDraw();
    rt_->SetTransform(D2D1::Matrix3x2F::Identity());
    rt_->Clear(kColorBg);

    DrawImageLayer();

    if (!fullscreen_) {
        DrawTitleBar();
        DrawBottomBar();
        DrawInfoPill();
    }
    DrawArrows();

    HRESULT hr = rt_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        DiscardDevice();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    // 上屏：DIB → 窗口 DC
    HDC dc = GetDC(hwnd_);
    if (dc) {
        BitBlt(dc, 0, 0, client.right, client.bottom, memDC_, 0, 0, SRCCOPY);
        ReleaseDC(hwnd_, dc);
    }
}

// 连续重绘 30 帧（模拟平移/缩放），得到稳态每帧耗时
void App::MeasureFrames() {
    if (!haveImage_) return;
    D2D1_POINT_2F saved = offset_;
    double t0 = NowMs();
    const int N = 30;
    for (int i = 0; i < N; ++i) {
        offset_.x = saved.x + (float)(i * 4);
        RenderAndBlit();
    }
    frameMs_ = (NowMs() - t0) / N;
    offset_ = saved;
    RenderAndBlit();
}

void App::Paint() {
    bool firstPaint = (firstPaintMs_ == 0);
    double tPaint0 = firstPaint ? NowMs() : 0;
    if (!EnsureDevice()) return;
    if (firstPaint) deviceMs_ = NowMs() - tPaint0;

    RenderAndBlit();

    if (firstPaint) {
        firstPaintMs_ = NowMs() - tPaint0;
        drawMs_ = firstPaintMs_ - deviceMs_;
    }

    if (!benchDone_ && haveImage_ && (benchMode_ || !shotPath_.empty())) {
        benchDone_ = true;
        tFirstImage_ = NowMs();
        tProcFirstImage_ = ProcElapsedMs();
        if (benchMode_) MeasureFrames();
        if (!shotPath_.empty()) SaveShot(shotPath_);
        if (benchMode_) ReportBench();
        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
    }
}

// 从进程创建算起的总耗时（ms）
double App::ProcElapsedMs() {
    FILETIME create, exitT, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &create, &exitT, &kernel, &user)) return 0;
    ULARGE_INTEGER c;
    c.LowPart = create.dwLowDateTime;
    c.HighPart = create.dwHighDateTime;
    FILETIME nowFt;
    GetSystemTimeAsFileTime(&nowFt);
    ULARGE_INTEGER n;
    n.LowPart = nowFt.dwLowDateTime;
    n.HighPart = nowFt.dwHighDateTime;
    return double(n.QuadPart - c.QuadPart) / 10000.0;
}

void App::DrawTitleBar() {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    float w = (float)rc.right;
    float h = titleH_;
    ID2D1SolidColorBrush* bg = Brush(kColorTitleBg);
    rt_->FillRectangle(D2D1::RectF(0, 0, w, h), bg);
    rt_->DrawLine(D2D1::Point2F(0, h - 0.5f), D2D1::Point2F(w, h - 0.5f), Brush(kColorBorder), 1.f);

    // 左侧：品牌点 + 名称 + 文件名
    float x = 14 * dpiScale_;
    float cy = h * 0.5f;
    ID2D1SolidColorBrush* accent = Brush(kColorAccent);
    rt_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x + 5 * dpiScale_, cy), 5 * dpiScale_, 5 * dpiScale_),
                     accent, 1.4f * dpiScale_);
    x += 16 * dpiScale_;
    Text(L"看看", x, 0, 40 * dpiScale_, h, 12.5f * dpiScale_, Brush(kColorText));
    x += 30 * dpiScale_;
    std::wstring sep = L"·";
    Text(sep, x, 0, 10 * dpiScale_, h, 12.5f * dpiScale_, Brush(kColorText3));
    x += 12 * dpiScale_;

    std::wstring name;
    if (loader_.FileCount() > 0) name = FileNameOf(loader_.PathAt(curIndex_));
    if (name.empty()) name = L"看看 SeeSee";
    float avail = w - x - 140 * dpiScale_;
    Text(name, x, 0, std::max<float>(20, avail), h, 12.5f * dpiScale_, Brush(kColorText2));

    // 右侧窗口按钮
    float bw = 46 * dpiScale_;
    float bx = w - bw * 3;
    struct { int id; int kind; } btns[3] = {{kBtnMin, 0}, {kBtnMax, 1}, {kBtnClose, 2}};
    for (int i = 0; i < 3; ++i) {
        D2D1_RECT_F br = D2D1::RectF(bx, 0, bx + bw, h);
        bool hot = (hover_ == btns[i].id);
        if (hot) rt_->FillRectangle(br, Brush(btns[i].id == kBtnClose ? kColorClose : kColorHover));
        ID2D1SolidColorBrush* fg = Brush(hot && btns[i].id == kBtnClose ? D2D1::ColorF(0xFFFFFF) : kColorText2);
        float cx = bx + bw * 0.5f;
        float g = 4.5f * dpiScale_;
        if (btns[i].kind == 0) {
            rt_->DrawLine(D2D1::Point2F(cx - g, cy), D2D1::Point2F(cx + g, cy), fg, 1.1f * dpiScale_);
        } else if (btns[i].kind == 1) {
            rt_->DrawRectangle(D2D1::RectF(cx - g, cy - g, cx + g, cy + g), fg, 1.1f * dpiScale_);
        } else {
            rt_->DrawLine(D2D1::Point2F(cx - g, cy - g), D2D1::Point2F(cx + g, cy + g), fg,
                          1.1f * dpiScale_);
            rt_->DrawLine(D2D1::Point2F(cx + g, cy - g), D2D1::Point2F(cx - g, cy + g), fg,
                          1.1f * dpiScale_);
        }
        AddHit(br, btns[i].id);
        bx += bw;
    }
}

void App::DrawBottomBar() {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    float w = (float)rc.right, h = (float)rc.bottom;

    const float s = dpiScale_;
    const float barH = 44 * s;
    const float iconW = 34 * s;
    const float zoomW = 52 * s;
    const float gap = 2 * s;
    const float pad = 6 * s;

    struct Item { int id; float w; };
    Item items[] = {
        {kBtnPrev, iconW}, {kBtnNext, iconW},
        {kBtnZoomOut, iconW}, {kBtnZoomIn, iconW},
        {kBtnFit, iconW}, {kBtnActual, zoomW},
        {kBtnFullscreen, iconW},
    };
    const int n = (int)(sizeof(items) / sizeof(items[0]));
    float total = pad * 2;
    for (int i = 0; i < n; ++i) total += items[i].w + gap;
    total += 2 * (10 * s);  // 两个分隔条

    float bx = (w - total) * 0.5f;
    float by = h - 14 * s - barH;

    D2D1_RECT_F barRc = D2D1::RectF(bx, by, bx + total, by + barH);
    FillRoundRect(barRc, barH * 0.5f, Brush(D2D1::ColorF(0x1B1E23, 0.94f)));
    StrokeRoundRect(barRc, barH * 0.5f, Brush(D2D1::ColorF(0x2A2F36, 0.9f)), 1.f);

    float x = bx + pad;
    for (int i = 0; i < n; ++i) {
        if (i == 2 || i == 5) {
            float sx = x + 4 * s;
            rt_->DrawLine(D2D1::Point2F(sx, by + barH * 0.5f - 10 * s),
                          D2D1::Point2F(sx, by + barH * 0.5f + 10 * s), Brush(kColorBorder), 1.f);
            x += 10 * s;
        }
        D2D1_RECT_F br = D2D1::RectF(x, by + (barH - iconW) * 0.5f, x + items[i].w,
                                     by + (barH + iconW) * 0.5f);
        bool hot = (hover_ == items[i].id);
        if (hot) FillRoundRect(br, 8 * s, Brush(kColorHover));
        float cx = (br.left + br.right) * 0.5f;
        float cy = (br.top + br.bottom) * 0.5f;
        ID2D1SolidColorBrush* fg = Brush(hot ? kColorText : kColorText2);
        float g = 5.f * s;
        switch (items[i].id) {
            case kBtnPrev:
                rt_->DrawLine(D2D1::Point2F(cx + g * 0.6f, cy - g), D2D1::Point2F(cx - g * 0.6f, cy), fg, 1.4f * s);
                rt_->DrawLine(D2D1::Point2F(cx - g * 0.6f, cy), D2D1::Point2F(cx + g * 0.6f, cy + g), fg, 1.4f * s);
                break;
            case kBtnNext:
                rt_->DrawLine(D2D1::Point2F(cx - g * 0.6f, cy - g), D2D1::Point2F(cx + g * 0.6f, cy), fg, 1.4f * s);
                rt_->DrawLine(D2D1::Point2F(cx + g * 0.6f, cy), D2D1::Point2F(cx - g * 0.6f, cy + g), fg, 1.4f * s);
                break;
            case kBtnZoomOut:
                rt_->DrawLine(D2D1::Point2F(cx - g, cy), D2D1::Point2F(cx + g, cy), fg, 1.3f * s);
                break;
            case kBtnZoomIn:
                rt_->DrawLine(D2D1::Point2F(cx - g, cy), D2D1::Point2F(cx + g, cy), fg, 1.3f * s);
                rt_->DrawLine(D2D1::Point2F(cx, cy - g), D2D1::Point2F(cx, cy + g), fg, 1.3f * s);
                break;
            case kBtnFit:
                rt_->DrawRectangle(D2D1::RectF(cx - g, cy - g * 0.75f, cx + g, cy + g * 0.75f), fg,
                                   1.2f * s);
                break;
            case kBtnActual: {
                wchar_t buf[16];
                swprintf_s(buf, L"%d%%", (int)std::lround(CurrentZoom() * 100.0));
                IDWriteTextFormat* monoFmt = Format(11.5f * s, true, 2);
                if (monoFmt) {
                    rt_->DrawTextW(buf, (UINT32)wcslen(buf), monoFmt, br, fg,
                                   D2D1_DRAW_TEXT_OPTIONS_CLIP);
                }
                break;
            }
            case kBtnFullscreen:
                rt_->DrawLine(D2D1::Point2F(cx - g, cy - g + 2 * s), D2D1::Point2F(cx - g, cy - g), fg, 1.2f * s);
                rt_->DrawLine(D2D1::Point2F(cx - g, cy - g), D2D1::Point2F(cx - g + 2 * s, cy - g), fg, 1.2f * s);
                rt_->DrawLine(D2D1::Point2F(cx + g - 2 * s, cy - g), D2D1::Point2F(cx + g, cy - g), fg, 1.2f * s);
                rt_->DrawLine(D2D1::Point2F(cx + g, cy - g), D2D1::Point2F(cx + g, cy - g + 2 * s), fg, 1.2f * s);
                rt_->DrawLine(D2D1::Point2F(cx - g, cy + g - 2 * s), D2D1::Point2F(cx - g, cy + g), fg, 1.2f * s);
                rt_->DrawLine(D2D1::Point2F(cx - g, cy + g), D2D1::Point2F(cx - g + 2 * s, cy + g), fg, 1.2f * s);
                rt_->DrawLine(D2D1::Point2F(cx + g - 2 * s, cy + g), D2D1::Point2F(cx + g, cy + g), fg, 1.2f * s);
                rt_->DrawLine(D2D1::Point2F(cx + g, cy + g), D2D1::Point2F(cx + g, cy + g - 2 * s), fg, 1.2f * s);
                break;
            default: break;
        }
        AddHit(br, items[i].id);
        x += items[i].w + gap;
    }
}

void App::DrawInfoPill() {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    float h = (float)rc.bottom;
    const float s = dpiScale_;

    if (loader_.FileCount() == 0) return;

    std::wstring path = loader_.PathAt(curIndex_);
    uint32_t sw = 0, sh = 0;
    for (int lvl = 0; lvl < kLevelCount && sw == 0; ++lvl) {
        auto it = cache_.find(((uint64_t)curIndex_ << 2) | (uint64_t)lvl);
        if (it != cache_.end()) {
            sw = it->second.srcW;
            sh = it->second.srcH;
        }
    }
    if (sw == 0) return;

    wchar_t dims[64];
    swprintf_s(dims, L"%u × %u", sw, sh);
    std::wstring info = dims;
    info += L"  ·  ";
    info += ExtUpper(path);
    info += L"  ·  ";
    info += HumanSize(loader_.SizeAt(curIndex_));

    wchar_t idxBuf[64];
    swprintf_s(idxBuf, L"%u / %zu", curIndex_ + 1, loader_.FileCount());
    std::wstring idx = idxBuf;

    const float pillH = 28 * s;
    const float padX = 12 * s;
    const float gap = 16 * s;
    const float textSize = 11.5f * s;
    const float wInfo = MeasureText(info, textSize, false);
    const float wIdx = MeasureText(idx, textSize, true);
    const float pillW = padX * 2 + wInfo + gap + wIdx;
    const float px = 16 * s;
    const float py = h - 14 * s - pillH;

    D2D1_RECT_F pill = D2D1::RectF(px, py, px + pillW, py + pillH);
    FillRoundRect(pill, pillH * 0.5f, Brush(D2D1::ColorF(0x1B1E23, 0.9f)));
    Text(info, px + padX, py, wInfo + 2 * s, pillH, textSize, Brush(kColorText2));
    Text(idx, px + padX + wInfo + gap, py, wIdx + 2 * s, pillH, textSize, Brush(kColorText3), 0);
}

void App::DrawArrows() {
    RECT a = AreaRect();
    const float s = dpiScale_;
    if (loader_.FileCount() <= 1) return;
    float cy = (a.top + a.bottom) * 0.5f;
    float r = 18 * s;

    struct { int id; float cx; bool left; } arrows[2] = {
        {kBtnArrowLeft, (float)a.left + 22 * s, true},
        {kBtnArrowRight, (float)a.right - 22 * s, false},
    };
    for (auto& ar : arrows) {
        D2D1_RECT_F rc = D2D1::RectF(ar.cx - r, cy - r, ar.cx + r, cy + r);
        bool hot = (hover_ == ar.id);
        bool enabled = ar.left ? curIndex_ > 0 : curIndex_ + 1 < loader_.FileCount();
        float alpha = enabled ? (hot ? 0.90f : 0.42f) : 0.12f;
        FillRoundRect(rc, r, Brush(D2D1::ColorF(0x1B1E23, alpha)));
        ID2D1SolidColorBrush* fg = Brush(D2D1::ColorF(0xFFFFFF, enabled ? (hot ? 1.0f : 0.75f) : 0.25f));
        float g = 4.5f * s;
        if (ar.left) {
            rt_->DrawLine(D2D1::Point2F(ar.cx + g * 0.7f, cy - g), D2D1::Point2F(ar.cx - g * 0.7f, cy), fg, 1.6f * s);
            rt_->DrawLine(D2D1::Point2F(ar.cx - g * 0.7f, cy), D2D1::Point2F(ar.cx + g * 0.7f, cy + g), fg, 1.6f * s);
        } else {
            rt_->DrawLine(D2D1::Point2F(ar.cx - g * 0.7f, cy - g), D2D1::Point2F(ar.cx + g * 0.7f, cy), fg, 1.6f * s);
            rt_->DrawLine(D2D1::Point2F(ar.cx + g * 0.7f, cy), D2D1::Point2F(ar.cx - g * 0.7f, cy + g), fg, 1.6f * s);
        }
        if (enabled) AddHit(rc, ar.id, true);
    }
}

void App::ShowOpenDialog() {
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                               IID_PPV_ARGS(&dlg))) ||
        !dlg)
        return;

    COMDLG_FILTERSPEC filters[] = {
        {L"图片文件", L"*.jpg;*.jpeg;*.jpe;*.jfif;*.png;*.bmp;*.dib;*.gif;*.webp;*.tif;*.tiff;*.ico;*.avif;*.heic"},
        {L"所有文件", L"*.*"},
    };
    dlg->SetFileTypes(2, filters);
    FILEOPENDIALOGOPTIONS opts{};
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);

    if (FAILED(dlg->Show(hwnd_))) return;

    ComPtr<IShellItem> item;
    if (FAILED(dlg->GetResult(&item)) || !item) return;
    PWSTR path = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
        OpenPath(path);
        CoTaskMemFree(path);
    }
}

// 把当前 DIB 帧存成 PNG（调试 / 回归用：所见即所得，不依赖抓屏）
bool App::SaveShot(const std::wstring& path) {
    auto fail = [](const wchar_t* step, HRESULT hr) {
        wchar_t b[256];
        swprintf_s(b, L"SaveShot failed at %s hr=0x%08lX\n", step, (unsigned long)hr);
        OutputDebugStringW(b);
        FILE* f = nullptr;
        if (_wfopen_s(&f, L"shot-err.txt", L"w, ccs=UTF-8") == 0 && f) {
            fputws(b, f);
            fclose(f);
        }
        return false;
    };
    if (!dib_ || bufSize_.cx < 1) return fail(L"nobuf", E_FAIL);
    ComPtr<IWICImagingFactory> wic;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&wic));
    if (FAILED(hr) || !wic) return fail(L"factory", hr);

    ComPtr<IWICBitmap> bmp;
    const UINT w = (UINT)bufSize_.cx, h = (UINT)bufSize_.cy, stride = w * 4;
    if (!dibBits_) return fail(L"nobits", E_FAIL);
    hr = wic->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat32bppBGRA, stride, stride * h,
                                     static_cast<BYTE*>(dibBits_), &bmp);
    if (FAILED(hr) || !bmp) return fail(L"frommemory", hr);

    // 编码到内存流再落盘（IWICStream::InitializeFromFilename 要求文件已存在，不可靠）
    ComPtr<IStream> memStream;
    hr = CreateStreamOnHGlobal(nullptr, TRUE, &memStream);
    if (FAILED(hr) || !memStream) return fail(L"hglobal", hr);
    ComPtr<IWICStream> stream;
    hr = wic->CreateStream(&stream);
    if (FAILED(hr)) return fail(L"stream", hr);
    hr = stream->InitializeFromIStream(memStream.Get());
    if (FAILED(hr)) return fail(L"initstream", hr);

    ComPtr<IWICBitmapEncoder> enc;
    hr = wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
    if (FAILED(hr) || !enc) return fail(L"encoder", hr);
    hr = enc->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(hr)) return fail(L"encinit", hr);

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> bag;
    hr = enc->CreateNewFrame(&frame, &bag);
    if (FAILED(hr) || !frame) return fail(L"newframe", hr);
    hr = frame->Initialize(bag.Get());
    if (FAILED(hr)) return fail(L"frameinit", hr);
    frame->SetSize(w, h);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    hr = frame->SetPixelFormat(&fmt);
    if (FAILED(hr)) return fail(L"pixfmt", hr);
    hr = frame->WriteSource(bmp.Get(), nullptr);
    if (FAILED(hr)) return fail(L"writesource", hr);
    hr = frame->Commit();
    if (FAILED(hr)) return fail(L"framecommit", hr);
    hr = enc->Commit();
    if (FAILED(hr)) return fail(L"commit", hr);

    // 把内存流写进文件
    HGLOBAL glob = nullptr;
    if (FAILED(GetHGlobalFromStream(memStream.Get(), &glob)) || !glob) return fail(L"gethglobal", E_FAIL);
    SIZE_T size = GlobalSize(glob);
    void* data = GlobalLock(glob);
    if (!data || !size) return fail(L"lock", E_FAIL);
    FILE* f = nullptr;
    errno_t e = _wfopen_s(&f, path.c_str(), L"wb");
    if (e != 0 || !f) {
        GlobalUnlock(glob);
        return fail(L"openfile", E_FAIL);
    }
    size_t wrote = fwrite(data, 1, size, f);
    fclose(f);
    GlobalUnlock(glob);
    if (wrote != size) return fail(L"writefile", E_FAIL);
    return true;
}

void App::ReportBench() {
    double procMs = tProcFirstImage_;
    double preMain = procMs - (tFirstImage_ - tStart_);
    wchar_t buf[1024];
    swprintf_s(buf,
               L"[bench] 冷启动 → 首张图片上屏: %.1f ms\n"
               L"  启动墙钟分帧:\n"
               L"    exe 加载 / CRT 启动        %6.1f ms\n"
               L"    D2D / DWrite 工厂           %6.1f ms\n"
               L"    窗口创建                    %6.1f ms\n"
               L"    扫目录 (%u 个文件) + 发请求   %6.1f ms\n"
               L"    显示窗口 + 建 D2D 设备首帧   %6.1f ms  (建设备 %.1f / 绘制 %.1f)\n"
               L"    解码回来到首张上屏          %6.1f ms\n"
               L"  并行段（与建 D2D 设备重叠）:\n"
               L"    读文件 %.1f ms / WIC 解码 %.1f ms / 上传 GPU %.1f ms\n"
               L"  稳态每帧（平移 30 帧平均）: %.1f ms  (~%.0f fps)\n",
               procMs, preMain, tFactory_ - tStart_, tWindowCreated_ - tFactory_,
               benchFileCount_, tOpen_ - tWindowCreated_, firstPaintMs_, deviceMs_, drawMs_,
               std::max<double>(0, tFirstImage_ - tWindowShown_ - readMs_ - decMs_ - uploadMs_),
               readMs_, decMs_, uploadMs_, frameMs_, frameMs_ > 0 ? 1000.0 / frameMs_ : 0.0);
    (void)scanMs_;
    OutputDebugStringW(buf);
    FILE* f = nullptr;
    if (_wfopen_s(&f, L"seesee-bench.txt", L"w, ccs=UTF-8") == 0 && f) {
        fputws(buf, f);
        fclose(f);
    }
}
