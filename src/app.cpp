// seesee / 看看 — 主窗口与渲染实现
#include "app.h"
#include "assoc.h"

#include <d2d1helper.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <windowsx.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "dwmapi.lib")

namespace {

const wchar_t* kClassName = L"SeeSeeMainWindow";
const UINT WM_APP_DECODED = WM_APP + 1;
const UINT_PTR kIdleTimerId = 1;

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
    i18n::SetLanguageSetting(pendingLang_ >= 0 ? pendingLang_ : cfg.language);
    keys::Load();
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

    hwnd_ = CreateWindowExW(0, kClassName, i18n::T(i18n::kAppTitle),
                            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                            rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr,
                            hInst, this);
    if (!hwnd_) return false;

    // DPI：必须在这里取一次真实 DPI，否则 96dpi 尺寸会直接画在 4K 高分屏上（字小到看不清）
    {
        UINT dpi = 0;
        if (auto pGetDpiForWindow = reinterpret_cast<UINT(WINAPI*)(HWND)>(
                GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"))) {
            dpi = pGetDpiForWindow(hwnd_);
        }
        if (dpi == 0) {
            HDC dc = GetDC(hwnd_);
            if (dc) {
                dpi = (UINT)GetDeviceCaps(dc, LOGPIXELSX);
                ReleaseDC(hwnd_, dc);
            }
        }
        if (dpi == 0) dpi = 96;
        monitorScale_ = (float)dpi / 96.0f;
    }
    stripVisible_ = Settings::Get().filmstrip;
    ApplyUiScale();

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
    lastInputMs_ = NowMs();
    idleTimer_ = (SetTimer(hwnd_, kIdleTimerId, 120, nullptr) != 0);
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
    thumbs_.clear();  // 换目录了，旧缩略图作废
    stripScroll_ = 0.f;
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
    UpdateCaption();
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
    UpdateCaption();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::NextImage() { GoTo((int64_t)curIndex_ + 1); }
void App::PrevImage() { GoTo((int64_t)curIndex_ - 1); }

// 所有命令的唯一入口：自定义键位与设置面板按钮都走这里
void App::RunCommand(int cmd) {
    switch ((keys::Cmd)cmd) {
        case keys::kPrev: PrevImage(); break;
        case keys::kNext: NextImage(); break;
        case keys::kFirst: GoTo(0); break;
        case keys::kLast: GoTo((int64_t)loader_.FileCount() - 1); break;
        case keys::kZoomIn:
            SetZoomAnchored(CurrentZoom() * 1.25, AreaCenter());
            UpdateRequests();
            break;
        case keys::kZoomOut:
            SetZoomAnchored(CurrentZoom() / 1.25, AreaCenter());
            UpdateRequests();
            break;
        case keys::kFit: ResetViewToFit(); UpdateRequests(); break;
        case keys::kActual:
            zoomMode_ = ZoomMode::Actual;
            zoom_ = 1.0;
            offset_ = D2D1::Point2F(0.f, 0.f);
            UpdateRequests();
            break;
        case keys::kFullscreen: ToggleFullscreen(); break;
        case keys::kOpen: ShowOpenDialog(); break;
        case keys::kDefaultApp: RegisterAsDefault(); break;
        case keys::kStrip: {
            stripVisible_ = !stripVisible_;
            Settings& cfg = Settings::Get();
            cfg.filmstrip = stripVisible_;
            cfg.Save();
            UpdateRequests();
            break;
        }
        case keys::kSettings: ToggleSettings(); break;
        case keys::kUiScaleUp: SetUiScale(Settings::Get().uiScale * 1.1f); break;
        case keys::kUiScaleDown: SetUiScale(Settings::Get().uiScale / 1.1f); break;
        case keys::kCopyImage:
            ShowToast(i18n::T(CopyImageToClipboard() ? i18n::kCopiedImage : i18n::kOpFailed));
            break;
        case keys::kCopyFile:
            ShowToast(i18n::T(CopyFileToClipboard(false) ? i18n::kCopiedFile : i18n::kOpFailed));
            break;
        case keys::kCutFile:
            ShowToast(i18n::T(CopyFileToClipboard(true) ? i18n::kCutToClipboard : i18n::kOpFailed));
            break;
        case keys::kCopyPath:
            ShowToast(i18n::T(CopyPathToClipboard() ? i18n::kCopiedPath : i18n::kOpFailed));
            break;
        case keys::kReveal: RevealInExplorer(); break;
        case keys::kEdit: EditWithDefaultApp(); break;
        case keys::kDelete: DeleteCurrentFile(); break;
        default: break;
    }
}

// ------------------------------------------------------------- 剪贴板 / 文件操作

static bool OpenClipboardWithRetry(HWND owner) {
    for (int i = 0; i < 12; ++i) {
        if (OpenClipboard(owner)) return true;
        Sleep(8);
    }
    return false;
}

std::wstring App::CurrentPath() const {
    if (loader_.FileCount() == 0) return std::wstring();
    return loader_.PathAt(curIndex_);
}

bool App::HasImageFile() const { return !CurrentPath().empty(); }

bool App::CopyPathToClipboard() {
    const std::wstring path = CurrentPath();
    if (path.empty()) return false;
    const size_t bytes = (path.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!mem) return false;
    void* p = GlobalLock(mem);
    if (!p) {
        GlobalFree(mem);
        return false;
    }
    memcpy(p, path.c_str(), bytes);
    GlobalUnlock(mem);
    if (!OpenClipboardWithRetry(hwnd_)) {
        GlobalFree(mem);
        return false;
    }
    EmptyClipboard();
    bool ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
    CloseClipboard();
    if (!ok) GlobalFree(mem);
    return ok;
}

// 复制 / 剪切文件：CF_HDROP + Preferred DropEffect（剪切时资源管理器粘贴即移动）
bool App::CopyFileToClipboard(bool cut) {
    const std::wstring path = CurrentPath();
    if (path.empty()) return false;

    const size_t bytes = sizeof(DROPFILES) + (path.size() + 2) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    if (!mem) return false;
    DROPFILES* df = (DROPFILES*)GlobalLock(mem);
    if (!df) {
        GlobalFree(mem);
        return false;
    }
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;
    wchar_t* dst = (wchar_t*)((BYTE*)df + sizeof(DROPFILES));
    memcpy(dst, path.c_str(), (path.size() + 1) * sizeof(wchar_t));
    GlobalUnlock(mem);

    HGLOBAL eff = nullptr;
    const UINT effFmt = RegisterClipboardFormatW(L"Preferred DropEffect");
    if (effFmt) {
        eff = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
        if (eff) {
            DWORD* p = (DWORD*)GlobalLock(eff);
            if (p) {
                *p = cut ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
                GlobalUnlock(eff);
            }
        }
    }

    if (!OpenClipboardWithRetry(hwnd_)) {
        GlobalFree(mem);
        if (eff) GlobalFree(eff);
        return false;
    }
    EmptyClipboard();
    bool ok = SetClipboardData(CF_HDROP, mem) != nullptr;
    if (ok) mem = nullptr;  // 系统接管
    if (eff && effFmt && SetClipboardData(effFmt, eff)) eff = nullptr;
    CloseClipboard();
    if (mem) GlobalFree(mem);
    if (eff) GlobalFree(eff);
    return ok;
}

// 复制图片像素：解码全尺寸 → 反乘 alpha → CF_DIBV5
bool App::CopyImageToClipboard() {
    const std::wstring path = CurrentPath();
    if (path.empty()) return false;

    HCURSOR prevCursor = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    DecodedImage img;
    Loader::Decode(curIndex_, path, kLevelFull, 0, 0, img);
    SetCursor(prevCursor);
    if (!img.ok || img.pixels.empty() || img.w == 0 || img.h == 0) return false;

    const LONG w = (LONG)img.w;
    const LONG h = (LONG)img.h;
    const size_t stride = (size_t)w * 4;
    const size_t imgBytes = stride * (size_t)h;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPV5HEADER) + imgBytes);
    if (!mem) return false;
    BYTE* base = (BYTE*)GlobalLock(mem);
    if (!base) {
        GlobalFree(mem);
        return false;
    }

    BITMAPV5HEADER* bi = (BITMAPV5HEADER*)base;
    ZeroMemory(bi, sizeof(*bi));
    bi->bV5Size = sizeof(BITMAPV5HEADER);
    bi->bV5Width = w;
    bi->bV5Height = h;  // 正数 = 自下而上，下面按行翻转
    bi->bV5Planes = 1;
    bi->bV5BitCount = 32;
    bi->bV5Compression = BI_BITFIELDS;
    bi->bV5SizeImage = (DWORD)imgBytes;
    bi->bV5RedMask = 0x00FF0000;
    bi->bV5GreenMask = 0x0000FF00;
    bi->bV5BlueMask = 0x000000FF;
    bi->bV5AlphaMask = 0xFF000000;
    bi->bV5CSType = LCS_WINDOWS_COLOR_SPACE;

    BYTE* dst = base + sizeof(BITMAPV5HEADER);
    const uint8_t* src = img.pixels.data();
    for (LONG y = 0; y < h; ++y) {
        const uint8_t* srow = src + (size_t)y * stride;
        BYTE* drow = dst + (size_t)(h - 1 - y) * stride;
        for (LONG x = 0; x < w; ++x) {
            const uint8_t a = srow[x * 4 + 3];
            if (a == 0 || a == 255) {
                memcpy(drow + x * 4, srow + x * 4, 4);
            } else {  // 反乘 alpha，否则带透明度的图在别的软件里会偏暗
                for (int k = 0; k < 3; ++k) {
                    int v = srow[x * 4 + k] * 255 / a;
                    drow[x * 4 + k] = (BYTE)(v > 255 ? 255 : v);
                }
                drow[x * 4 + 3] = a;
            }
        }
    }
    GlobalUnlock(mem);

    if (!OpenClipboardWithRetry(hwnd_)) {
        GlobalFree(mem);
        return false;
    }
    EmptyClipboard();
    bool ok = SetClipboardData(CF_DIBV5, mem) != nullptr;
    CloseClipboard();
    if (!ok) GlobalFree(mem);
    return ok;
}

void App::RevealInExplorer() {
    const std::wstring path = CurrentPath();
    if (path.empty()) return;
    const std::wstring args = L"/select,\"" + path + L"\"";
    ShellExecuteW(hwnd_, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

void App::EditWithDefaultApp() {
    const std::wstring path = CurrentPath();
    if (path.empty()) return;
    HINSTANCE r = ShellExecuteW(hwnd_, L"edit", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if ((INT_PTR)r <= 32) {  // 没有注册编辑器 → 退回"打开"
        r = ShellExecuteW(hwnd_, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if ((INT_PTR)r <= 32) ShowToast(i18n::T(i18n::kOpFailed));
    }
}

void App::DeleteCurrentFile() {
    const std::wstring path = CurrentPath();
    if (path.empty()) return;
    std::wstring list = path;
    list.push_back(L'\0');
    list.push_back(L'\0');  // pFrom 需要双 null 结尾

    SHFILEOPSTRUCTW op{};
    op.hwnd = hwnd_;
    op.wFunc = FO_DELETE;
    op.pFrom = list.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_WANTNUKEWARNING;  // 进回收站，确认交给系统
    const int r = SHFileOperationW(&op);
    if (r == 0 && !op.fAnyOperationsAborted) {
        ShowToast(i18n::T(i18n::kDeletedFile));
        RescanAfterDelete();
    } else if (r != 0) {
        ShowToast(i18n::T(i18n::kOpFailed));
    }
}

void App::RescanAfterDelete() {
    const std::wstring path = CurrentPath();
    if (path.empty()) return;
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return;
    const std::wstring dir = path.substr(0, slash);
    const size_t want = curIndex_;
    auto files = ScanImageFolder(dir);
    if (files.empty()) {
        loader_.SetFiles(std::vector<ImageFile>{});
        cache_.clear();
        thumbs_.clear();
        curIndex_ = 0;
        haveImage_ = false;
        errorText_.clear();
        UpdateCaption();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    const size_t pick = want < files.size() ? want : files.size() - 1;
    OpenPath(files[pick].path);
}

void App::ImportRegFile() {
    if (lastRegFile_.empty()) {
        ShowToast(i18n::T(i18n::kOpFailed));
        return;
    }
    // 交给系统自己去合并（那个确认是系统行为，不是我们弹的窗口）
    HINSTANCE r =
        ShellExecuteW(hwnd_, L"open", lastRegFile_.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if ((INT_PTR)r <= 32) ShowToast(i18n::T(i18n::kOpFailed));
}

void App::RegisterAsDefault() {
    std::wstring regFile;
    if (assoc::Register(&regFile)) {
        lastRegFile_.clear();
        regBlocked_ = false;
        assoc::OpenDefaultAppsSettings();  // 直接引导到系统「默认应用」
        ShowToast(i18n::T(i18n::kSetDefaultDone));
        if (!settingsOpen_) ToggleSettings();
        return;
    }
    // 被安全策略拦下：不弹自家对话框，改成面板里的「导入关联文件」引导
    lastRegFile_ = regFile;
    regBlocked_ = true;
    if (!settingsOpen_) ToggleSettings();
    settingsTab_ = 0;
    setScroll_ = 0.f;
    ShowToast(i18n::T(regFile.empty() ? i18n::kOpFailed : i18n::kRegBlockedHint));
}

int App::ShowContextMenu() {
    HMENU m = CreatePopupMenu();
    if (!m) return 0;

    constexpr UINT kMenuIdSettings = 900;
    constexpr UINT kMenuIdExit = 901;

    const bool has = HasImageFile();
    auto add = [&](keys::Cmd cmd, i18n::Str label) {
        const UINT flags = MF_STRING | (has ? MF_ENABLED : MF_GRAYED);
        AppendMenuW(m, flags, (UINT)cmd + 1, i18n::T(label));
    };
    auto sep = [&]() { AppendMenuW(m, MF_SEPARATOR, 0, nullptr); };

    add(keys::kPrev, i18n::kCmdPrev);
    add(keys::kNext, i18n::kCmdNext);
    sep();
    add(keys::kFit, i18n::kCmdFit);
    add(keys::kActual, i18n::kCmdActual);
    add(keys::kZoomIn, i18n::kCmdZoomIn);
    add(keys::kZoomOut, i18n::kCmdZoomOut);
    add(keys::kFullscreen, i18n::kCmdFullscreen);
    sep();
    add(keys::kCopyImage, i18n::kCmdCopyImage);
    add(keys::kCopyFile, i18n::kCmdCopyFile);
    add(keys::kCutFile, i18n::kCmdCutFile);
    add(keys::kCopyPath, i18n::kCmdCopyPath);
    sep();
    add(keys::kReveal, i18n::kCmdReveal);
    add(keys::kEdit, i18n::kCmdEdit);
    add(keys::kDelete, i18n::kCmdDelete);
    sep();
    add(keys::kDefaultApp, i18n::kCmdDefaultApp);
    AppendMenuW(m, MF_STRING, kMenuIdSettings, i18n::T(i18n::kMenuSettings));
    sep();
    AppendMenuW(m, MF_STRING, kMenuIdExit, i18n::T(i18n::kMenuExit));

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd_);
    const int id = (int)TrackPopupMenu(m,
                                      TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y,
                                      0, hwnd_, nullptr);
    DestroyMenu(m);
    if (id == 0) return 0;
    if (id == (int)kMenuIdExit) {
        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        return id;
    }
    if (id == (int)kMenuIdSettings) {
        RunCommand((int)keys::kSettings);
        return id;
    }
    RunCommand(id - 1);  // 菜单 id = 命令 + 1
    return id;
}

void App::UpdateCaption() {
    if (!hwnd_) return;
    std::wstring cap = i18n::T(i18n::kAppTitle);
    const size_t n = loader_.FileCount();
    if (n > 0) {
        std::wstring name = FileNameOf(loader_.PathAt(curIndex_));
        if (!name.empty()) {
            cap = std::wstring(i18n::T(i18n::kAppName)) + L" · " + name;
            if (n > 1) cap += L"  (" + std::to_wstring(curIndex_ + 1) + L"/" + std::to_wstring(n) + L")";
        }
    }
    SetWindowTextW(hwnd_, cap.c_str());
}

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
    // 胶片条可见时，把当前附近的索引也纳入"想要"，否则缩略图请求会被丢弃
    if (StripHeight() > 0.f) {
        int64_t lo = (int64_t)curIndex_ - 40, hi = (int64_t)curIndex_ + 40;
        if (lo < 0) lo = 0;
        if (hi > (int64_t)loader_.FileCount() - 1) hi = (int64_t)loader_.FileCount() - 1;
        for (int64_t i = lo; i <= hi; ++i) wanted.push_back((uint32_t)i);
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

    RequestThumbs();
    SyncStripScroll();
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
            if (img->level == kLevelThumb) {
                thumbs_[img->index] = nullptr;  // 标记已尝试，避免反复重试
                continue;
            }
            if (img->index == curIndex_) {
                errorText_ = img->error.empty() ? i18n::T(i18n::kCannotOpen) : img->error;
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

        // 缩略图走独立表，不进大图缓存
        if (img->level == kLevelThumb) {
            thumbs_[img->index] = bmp;
            img->pixels.clear();
            img->pixels.shrink_to_fit();
            if (stripVisible_) InvalidateRect(hwnd_, nullptr, FALSE);
            continue;
        }

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

    // 淘汰过多的缓存：条目数 + 字节预算双上限，且不淘汰当前正在显示的图
    const size_t kMaxEntries = 12;
    const size_t kMaxBytes = 192ull * 1024 * 1024;
    auto cacheBytes = [this] {
        size_t total = 0;
        for (const auto& kv : cache_)
            total += (size_t)kv.second.decW * (size_t)kv.second.decH * 4u;
        return total;
    };
    while (cache_.size() > 1 && (cache_.size() > kMaxEntries || cacheBytes() > kMaxBytes)) {
        auto worst = cache_.end();
        for (auto it = cache_.begin(); it != cache_.end(); ++it) {
            if ((it->first >> 2) == (uint64_t)curIndex_) continue;  // 正在显示，保留
            if (worst == cache_.end() || it->second.lastUse < worst->second.lastUse) worst = it;
        }
        if (worst == cache_.end()) break;
        cache_.erase(worst);
    }

    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ---------------------------------------------------------------- 视图

RECT App::AreaRect() const {
    RECT rc{};
    if (!hwnd_) return rc;
    GetClientRect(hwnd_, &rc);
    rc.bottom -= (LONG)StripHeight();
    if (rc.bottom < rc.top + 40) rc.bottom = rc.top + 40;
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
        ApplyUiScale();
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
        case WM_NCHITTEST:
            if (fullscreen_) return HTCLIENT;
            break;  // 缩放边框 / 拖动 / 双击最大化 / 贴边分屏全部交给系统窗口边框

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
            monitorScale_ = (float)HIWORD(wp) / 96.0f;
            ApplyUiScale();
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

        case WM_TIMER:
            if (wp == kIdleTimerId) {
                UpdateIdle();
                return 0;
            }
            break;

        case WM_MOUSEMOVE: {
            lastInputMs_ = NowMs();
            if (chromeAlpha_ < 1.f) chromeAlpha_ = 1.f;
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

        case WM_CONTEXTMENU: {
            // 右键菜单（也覆盖 Shift+F10 / 菜单键）
            lastInputMs_ = NowMs();
            if (chromeAlpha_ < 1.f) chromeAlpha_ = 1.f;
            ShowContextMenu();
            return 0;
        }

        case WM_LBUTTONDOWN: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            lastInputMs_ = NowMs();
            if (chromeAlpha_ < 1.f) chromeAlpha_ = 1.f;
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
                if (id >= 200 && id < kBtnThumbBase) {  // 设置面板控件
                    OnPanelClick(id);
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                if (id >= kBtnThumbBase) {
                    GoTo((int64_t)(id - kBtnThumbBase));
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                switch (id) {
                    case kBtnSettings: ToggleSettings(); break;
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
            POINT wheelPt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &wheelPt);
            // 指针在设置面板上 → 滚动面板（lp 对滚轮是屏幕坐标）
            if (settingsOpen_ && wheelPt.x >= (LONG)panelRc_.left &&
                wheelPt.x <= (LONG)panelRc_.right) {
                setScroll_ -= (delta > 0 ? 60.f : -60.f) * dpiScale_;
                if (setScroll_ < 0.f) setScroll_ = 0.f;
                if (setScroll_ > setScrollMax_) setScroll_ = setScrollMax_;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
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

        // Alt+X 走 WM_SYSKEYDOWN：捕获态要用它绑 Alt 组合，平时命中绑定就执行
        case WM_SYSKEYDOWN: {
            lastInputMs_ = NowMs();
            if (chromeAlpha_ < 1.f) chromeAlpha_ = 1.f;
            const UINT vk = (UINT)wp;
            if (captureCmd_ >= 0) {
                TryCaptureKey(vk);
                return 0;
            }
            if (vk != VK_MENU) {
                keys::Cmd cmd = keys::Match(vk, (GetKeyState(VK_CONTROL) & 0x8000) != 0,
                                            (GetKeyState(VK_SHIFT) & 0x8000) != 0, true);
                if (cmd != keys::kCount) {
                    RunCommand((int)cmd);
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
            }
            break;
        }

        case WM_KEYDOWN: {
            lastInputMs_ = NowMs();
            if (chromeAlpha_ < 1.f) chromeAlpha_ = 1.f;
            const UINT vk = (UINT)wp;

            // 设置面板正在等新键位：这次按键只用于绑定
            if (captureCmd_ >= 0) {
                TryCaptureKey(vk);
                return 0;
            }

            // Esc 是硬绑定：先关设置面板，再退全屏，最后关窗口
            if (vk == VK_ESCAPE) {
                if (settingsOpen_) {
                    settingsOpen_ = false;
                    captureCmd_ = -1;
                } else if (fullscreen_) {
                    ToggleFullscreen();
                } else {
                    PostMessageW(hwnd, WM_CLOSE, 0, 0);
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
            keys::Cmd cmd = keys::Match(vk, ctrl, shift, alt);
            if (cmd != keys::kCount) {
                RunCommand((int)cmd);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
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
            if (idleTimer_) {
                KillTimer(hwnd, kIdleTimerId);
                idleTimer_ = false;
            }
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
    thumbs_.clear();
    layer_.Reset();
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
    // 后画的（设置面板）优先命中
    for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
        const Hit& h = *it;
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
                 (float)(a.right - a.left), 42, 15.5f * dpiScale_, Brush(kColorText2), 2);
        } else if (loader_.FileCount() > 0) {
            Text(i18n::T(i18n::kLoading), (float)a.left, (float)(a.top + a.bottom) / 2 - 20,
                 (float)(a.right - a.left), 40, 13 * dpiScale_, br, 2);
        } else {
            Text(i18n::T(i18n::kEmptyHint), (float)a.left,
                 (float)(a.top + a.bottom) / 2 - 20, (float)(a.right - a.left), 40,
                 15 * dpiScale_, br, 2);
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

    if (!fullscreen_) { /* 系统标题栏负责品牌与文件名，客户区不再画 */ }

    // 底部控件可淡出（鼠标静止 2.6s 后降到 0，一动即回）
    const float ca = ChromeAlpha();
    bool fade = (!fullscreen_) && ca < 0.999f;
    if (fade) {
        if (!layer_ && FAILED(rt_->CreateLayer(nullptr, layer_.ReleaseAndGetAddressOf()))) fade = false;
        if (fade) {
            D2D1_LAYER_PARAMETERS lp = D2D1::LayerParameters(
                D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                D2D1::IdentityMatrix(), ca, nullptr, D2D1_LAYER_OPTIONS_NONE);
            rt_->PushLayer(&lp, layer_.Get());
        }
    }
    if (!fullscreen_) {
        DrawBottomBar();
        DrawInfoPill();
    }
    DrawArrows();
    if (fade) rt_->PopLayer();

    DrawFilmstrip();
    if (settingsOpen_) DrawSettingsPanel();

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
        if (!shotPath_.empty()) {
            if (!benchMode_) {
                if (pendingPanel_ >= 0) {  // --panel：截图前把设置面板打开
                    settingsOpen_ = true;
                    settingsTab_ = pendingPanel_;
                    setScroll_ = 0.f;
                    if (pendingCapture_) captureCmd_ = keys::kPrev;
                }
                // 截图前把附近的缩略图收完（顺便验证胶片条）
                ULONGLONG deadline = GetTickCount64() + 2000;
                MSG m;
                while (GetTickCount64() < deadline) {
                    bool allDone = true;
                    size_t n = loader_.FileCount();
                    for (size_t k = 0; k < n && k < 24; ++k) {
                        if (!thumbs_.count((uint32_t)k)) {
                            allDone = false;
                            break;
                        }
                    }
                    if (allDone) break;
                    Sleep(25);
                    while (PeekMessageW(&m, hwnd_, WM_APP_DECODED, WM_APP_DECODED, PM_REMOVE))
                        OnDecodedReady();
                }
                RenderAndBlit();
            }
            SaveShot(shotPath_);
        }
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

void App::ApplyUiScale() {
    float ui = Settings::Get().uiScale;
    if (ui < 0.75f) ui = 0.75f;
    if (ui > 3.0f) ui = 3.0f;
    dpiScale_ = monitorScale_ * ui;
    if (dpiScale_ < 0.75f) dpiScale_ = 0.75f;
    titleH_ = 0.f;  // 顶部标题栏交给系统窗口边框，客户区不再自绘第二根标题栏
}

void App::SetUiScale(float v) {
    v = std::clamp(v, 0.75f, 3.0f);
    Settings& cfg = Settings::Get();
    if (std::fabs(v - cfg.uiScale) < 0.001f) return;
    cfg.uiScale = v;
    cfg.Save();
    ApplyUiScale();
    UpdateRequests();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

float App::StripHeight() const {
    if (fullscreen_ || !stripVisible_ || loader_.FileCount() == 0) return 0.f;
    return std::round(86.f * dpiScale_);
}

float App::ChromeAlpha() const {
    return Settings::Get().hideChrome ? chromeAlpha_ : 1.f;
}

void App::UpdateIdle() {
    Settings& cfg = Settings::Get();
    if (!cfg.hideChrome) {
        if (chromeAlpha_ != 1.f) {
            chromeAlpha_ = 1.f;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return;
    }
    double now = NowMs();
    bool idle = (lastInputMs_ > 0) && (now - lastInputMs_ > 2600.0);
    float target = idle ? 0.0f : 1.f;
    if (hover_ != kBtnNone) target = 1.f;
    if (settingsOpen_) target = 1.f;  // 设置面板开着时不淡出
    float next = chromeAlpha_;
    const float step = 0.12f;
    if (next < target) next = std::min(target, next + step);
    else if (next > target) next = std::max(target, next - step);
    if (std::fabs(next - chromeAlpha_) > 0.001f) {
        chromeAlpha_ = next;
        InvalidateRect(hwnd_, nullptr, FALSE);
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
        {kBtnSettings, iconW},
    };
    const int n = (int)(sizeof(items) / sizeof(items[0]));
    float total = pad * 2;
    for (int i = 0; i < n; ++i) total += items[i].w + gap;
    total += 3 * (10 * s);  // 三个分隔条

    float bx = (w - total) * 0.5f;
    float by = h - StripHeight() - 14 * s - barH;

    D2D1_RECT_F barRc = D2D1::RectF(bx, by, bx + total, by + barH);
    FillRoundRect(barRc, barH * 0.5f, Brush(D2D1::ColorF(0x1B1E23, 0.94f)));
    StrokeRoundRect(barRc, barH * 0.5f, Brush(D2D1::ColorF(0x2A2F36, 0.9f)), 1.f);

    float x = bx + pad;
    for (int i = 0; i < n; ++i) {
        if (i == 2 || i == 5 || i == 7) {
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
                IDWriteTextFormat* monoFmt = Format(12.5f * s, true, 2);
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
            case kBtnSettings: {
                // 齿轮：圆 + 六根齿
                const float rr = g * 0.72f;
                rt_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rr, rr), fg, 1.25f * s);
                for (int k = 0; k < 6; ++k) {
                    double a = k * 3.14159265358979 / 3.0;
                    rt_->DrawLine(D2D1::Point2F(cx + (float)std::cos(a) * (rr + 1.0f * s),
                                                cy + (float)std::sin(a) * (rr + 1.0f * s)),
                                  D2D1::Point2F(cx + (float)std::cos(a) * (rr + 2.6f * s),
                                                cy + (float)std::sin(a) * (rr + 2.6f * s)),
                                  fg, 1.25f * s);
                }
                break;
            }
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

    const float pillH = 30 * s;
    const float padX = 13 * s;
    const float gap = 16 * s;
    const float textSize = 12.5f * s;
    const float wInfo = MeasureText(info, textSize, false);
    const float wIdx = MeasureText(idx, textSize, true);
    const float pillW = padX * 2 + wInfo + gap + wIdx;
    const float px = 16 * s;
    const float py = h - StripHeight() - 14 * s - pillH;

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

void App::SyncStripScroll() {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const float s = dpiScale_;
    const float pad = 12 * s, gap = 8 * s, ts = 60 * s;
    const size_t n = loader_.FileCount();
    float W = (float)rc.right;
    float contentW = pad * 2 + (float)n * ts + (n > 1 ? (float)(n - 1) * gap : 0.f);
    if (contentW <= W) {  // 图少时整排居中，别贴着左边
        stripScroll_ = (W - contentW) * 0.5f;
        return;
    }
    float maxNeg = std::min(0.f, W - contentW);  // <= 0
    float ideal = W * 0.5f - (pad + (float)curIndex_ * (ts + gap) + ts * 0.5f);
    stripScroll_ = std::clamp(ideal, maxNeg, 0.f);
}

// 底部胶片条：缩略图横向排列，点击跳转，当前项高亮
void App::DrawFilmstrip() {
    const float sh = StripHeight();
    if (sh <= 0.f) return;

    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const float W = (float)rc.right, H = (float)rc.bottom;
    const float s = dpiScale_;
    const float top = H - sh;

    rt_->FillRectangle(D2D1::RectF(0, top, W, H), Brush(kColorTitleBg));
    rt_->DrawLine(D2D1::Point2F(0, top + 0.5f), D2D1::Point2F(W, top + 0.5f), Brush(kColorBorder), 1.f);

    const size_t n = loader_.FileCount();
    if (n == 0) return;

    const float pad = 12 * s, gap = 8 * s, ts = 60 * s;
    const float cy = top + sh * 0.5f;

    rt_->PushAxisAlignedClip(D2D1::RectF(0, top + 1.f, W, H), D2D1_ANTIALIAS_MODE_ALIASED);
    for (size_t i = 0; i < n; ++i) {
        float x = pad + (float)i * (ts + gap) + stripScroll_;
        if (x > W || x + ts < 0) continue;
        D2D1_RECT_F cell = D2D1::RectF(x, cy - ts * 0.5f, x + ts, cy + ts * 0.5f);
        FillRoundRect(cell, 5 * s, Brush(D2D1::ColorF(0x232830, 1.f)));

        auto it = thumbs_.find((uint32_t)i);
        if (it != thumbs_.end() && it->second) {
            D2D1_SIZE_F bsz = it->second->GetSize();
            if (bsz.width > 0 && bsz.height > 0) {
                float k = std::min(ts / bsz.width, ts / bsz.height);
                float dw = bsz.width * k, dh = bsz.height * k;
                D2D1_RECT_F dr = D2D1::RectF(x + (ts - dw) * 0.5f, cy - dh * 0.5f,
                                             x + (ts + dw) * 0.5f, cy + dh * 0.5f);
                rt_->DrawBitmap(it->second.Get(), dr, 1.0f,
                                D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, nullptr);
            }
        }
        if (i == curIndex_) {
            StrokeRoundRect(D2D1::RectF(cell.left - 1.5f * s, cell.top - 1.5f * s,
                                        cell.right + 1.5f * s, cell.bottom + 1.5f * s),
                            6 * s, Brush(kColorAccent), 2 * s);
        }
        AddHit(cell, kBtnThumbBase + (int)i);
    }
    rt_->PopAxisAlignedClip();
}

// ---------------------------------------------------------------- 设置面板

float App::PanelWidth() const {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    return std::min(380.f * dpiScale_, (float)rc.right * 0.46f);
}

void App::ToggleSettings() {
    settingsOpen_ = !settingsOpen_;
    captureCmd_ = -1;
    toast_.clear();
    if (settingsOpen_) {
        setScroll_ = 0.f;
        lastInputMs_ = NowMs();
        chromeAlpha_ = 1.f;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::RequestPanel(int tab, bool capture) {
    pendingPanel_ = std::clamp(tab, 0, 2);
    pendingCapture_ = capture;
}

void App::RequestLanguage(int v) { pendingLang_ = std::clamp(v, 0, 2); }

void App::SetLanguage(int v) {
    Settings& cfg = Settings::Get();
    cfg.language = std::clamp(v, 0, 2);
    cfg.Save();
    i18n::SetLanguageSetting(cfg.language);
    UpdateCaption();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::ShowToast(const std::wstring& text) {
    toast_ = text;
    toastUntil_ = NowMs() + 4000.0;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// 捕获态下把一次按键转成绑定：Esc 取消、Delete/Backspace 解绑、纯修饰键忽略
void App::TryCaptureKey(UINT vk) {
    if (captureCmd_ < 0) return;
    if (vk == VK_ESCAPE) {
        captureCmd_ = -1;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (vk == VK_DELETE || vk == VK_BACK) {
        keys::Clear((keys::Cmd)captureCmd_);
        captureCmd_ = -1;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN ||
        vk == VK_CAPITAL) {
        return;  // 纯修饰键不算一次绑定
    }
    CaptureKey(vk, (GetKeyState(VK_CONTROL) & 0x8000) != 0,
               (GetKeyState(VK_SHIFT) & 0x8000) != 0, (GetKeyState(VK_MENU) & 0x8000) != 0);
}

void App::CaptureKey(UINT vk, bool ctrl, bool shift, bool alt) {
    if (captureCmd_ < 0 || captureCmd_ >= keys::kCount) {
        captureCmd_ = -1;
        return;
    }
    const keys::Cmd cmd = (keys::Cmd)captureCmd_;
    keys::Chord ch;
    ch.vk = vk;
    ch.ctrl = ctrl;
    ch.shift = shift;
    ch.alt = alt;
    // 一个键只对应一个命令：先把冲突的那条解绑
    int clearedCmd = -1;
    for (int i = 0; i < keys::kCount; ++i) {
        if (i == (int)cmd) continue;
        for (const keys::Chord& other : keys::Current((keys::Cmd)i)) {
            if (other.vk == ch.vk && other.ctrl == ch.ctrl && other.shift == ch.shift &&
                other.alt == ch.alt) {
                keys::Clear((keys::Cmd)i);
                clearedCmd = i;
                break;
            }
        }
    }
    keys::Set(cmd, ch);
    captureCmd_ = -1;
    if (clearedCmd >= 0) {
        wchar_t buf[256];
        swprintf_s(buf, i18n::T(i18n::kKeysConflict),
                   i18n::T(keys::LabelStr((keys::Cmd)clearedCmd)));
        ShowToast(buf);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::OnPanelClick(int id) {
    Settings& cfg = Settings::Get();

    if (id >= kSetKeyBase && id < kSetKeyBase + (int)keys::kCount) {
        const int cmd = id - kSetKeyBase;
        captureCmd_ = (captureCmd_ == cmd) ? -1 : cmd;
        lastInputMs_ = NowMs();
        return;
    }

    switch (id) {
        case kSetClose:
            settingsOpen_ = false;
            captureCmd_ = -1;
            UpdateRequests();
            break;
        case kSetTabGeneral:
        case kSetTabKeys:
        case kSetTabAbout:
            settingsTab_ = (id == kSetTabGeneral) ? 0 : (id == kSetTabKeys ? 1 : 2);
            setScroll_ = 0.f;
            captureCmd_ = -1;
            break;
        case kSetLangAuto: SetLanguage(0); break;
        case kSetLangZh: SetLanguage(1); break;
        case kSetLangEn: SetLanguage(2); break;
        case kSetTgFilmstrip:
            stripVisible_ = !stripVisible_;
            cfg.filmstrip = stripVisible_;
            cfg.Save();
            stripScroll_ = 0.f;
            UpdateRequests();
            break;
        case kSetTgHideChrome:
            cfg.hideChrome = !cfg.hideChrome;
            cfg.Save();
            chromeAlpha_ = 1.f;
            break;
        case kSetTgWheelNav:
            cfg.wheelNavigates = !cfg.wheelNavigates;
            cfg.Save();
            break;
        case kSetTgUpscale:
            cfg.fitUpscale = !cfg.fitUpscale;
            cfg.Save();
            ResetViewToFit();
            UpdateRequests();
            break;
        case kSetZoomOut: SetUiScale(cfg.uiScale / 1.1f); break;
        case kSetZoomIn: SetUiScale(cfg.uiScale * 1.1f); break;
        case kSetZoomReset: SetUiScale(1.0f); break;
        case kSetRegister: RunCommand((int)keys::kDefaultApp); break;
        case kSetImportReg: ImportRegFile(); break;
        case kSetOpenApps: assoc::OpenDefaultAppsSettings(); break;
        case kSetKeysReset:
            keys::ResetAll();
            captureCmd_ = -1;
            ShowToast(i18n::T(i18n::kKeysResetAll));
            break;
        default: break;
    }
}

void App::DrawSettingsPanel() {
    RECT client{};
    GetClientRect(hwnd_, &client);
    const float s = dpiScale_;
    const float W = (float)client.right;
    const float bottomLimit = (float)client.bottom - StripHeight();
    const float pw = PanelWidth();
    panelRc_ = D2D1::RectF(W - pw, 0.f, W, bottomLimit);

    // 面板底色比画布略亮一点，并在左缘画一条分隔线，形成"浮在图片上"的层次
    rt_->FillRectangle(panelRc_, Brush(D2D1::ColorF(0x14171C, 0.985f)));
    rt_->DrawLine(D2D1::Point2F(panelRc_.left, panelRc_.top),
                  D2D1::Point2F(panelRc_.left, panelRc_.bottom), Brush(kColorBorder), 1.f);

    const float padX = 18 * s;
    const float x0 = panelRc_.left + padX;
    const float x1 = panelRc_.right - padX;
    const float cw = x1 - x0;

    // 标题 + 关闭
    float y = 16 * s;
    Text(i18n::T(i18n::kSettingsTitle), x0, y, cw - 30 * s, 30 * s, 15.5f * s, Brush(kColorText));
    {
        D2D1_RECT_F closeRc = D2D1::RectF(x1 - 24 * s, y + 3 * s, x1, y + 27 * s);
        const bool hot = (hover_ == kSetClose);
        if (hot) FillRoundRect(closeRc, 6 * s, Brush(kColorHover));
        const float ccx = (closeRc.left + closeRc.right) * 0.5f;
        const float ccy = (closeRc.top + closeRc.bottom) * 0.5f;
        const float g = 4.6f * s;
        ID2D1SolidColorBrush* fg = Brush(hot ? kColorText : kColorText2);
        rt_->DrawLine(D2D1::Point2F(ccx - g, ccy - g), D2D1::Point2F(ccx + g, ccy + g), fg, 1.4f * s);
        rt_->DrawLine(D2D1::Point2F(ccx - g, ccy + g), D2D1::Point2F(ccx + g, ccy - g), fg, 1.4f * s);
        AddHit(closeRc, kSetClose);
    }
    y += 34 * s;

    // 三个标签页
    {
        struct TabDef {
            int id;
            i18n::Str label;
        };
        const TabDef tabs[3] = {{kSetTabGeneral, i18n::kTabGeneral},
                                {kSetTabKeys, i18n::kTabKeys},
                                {kSetTabAbout, i18n::kTabAbout}};
        const float gapT = 4 * s;
        const float tw = (cw - gapT * 2) / 3.f;
        const float th = 30 * s;
        FillRoundRect(D2D1::RectF(x0 - 3 * s, y - 3 * s, x1 + 3 * s, y + th + 3 * s), 9 * s,
                      Brush(D2D1::ColorF(0x0F1114, 0.9f)));
        for (int i = 0; i < 3; ++i) {
            const float tx = x0 + i * (tw + gapT);
            D2D1_RECT_F tr = D2D1::RectF(tx, y, tx + tw, y + th);
            const bool sel = (settingsTab_ == i);
            if (sel) FillRoundRect(tr, 8 * s, Brush(D2D1::ColorF(0x262B33)));
            else if (hover_ == tabs[i].id) FillRoundRect(tr, 8 * s, Brush(kColorHover));
            Text(i18n::T(tabs[i].label), tr.left, tr.top, tw, th, 12.5f * s,
                 Brush(sel ? kColorText : kColorText2), 2);
            AddHit(tr, tabs[i].id);
        }
        y += th + 16 * s;
    }

    const float clipTop = y;
    // 快捷键页底部留一条固定页脚（Esc 提示 + 恢复默认），列表在它上面滚动
    const float footerH = (settingsTab_ == 1) ? 78 * s : 0.f;
    const D2D1_RECT_F clip = D2D1::RectF(panelRc_.left, clipTop, panelRc_.right,
                                         bottomLimit - 10 * s - footerH);
    const float clipH = clip.bottom - clip.top;
    rt_->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    float yy = clipTop - setScroll_;
    const float rowH = 34 * s;
    Settings& cfg = Settings::Get();

    auto toggleRow = [&](i18n::Str label, bool on, int id) {
        D2D1_RECT_F row = D2D1::RectF(x0 - 8 * s, yy, x1 + 8 * s, yy + rowH);
        if (hover_ == id) FillRoundRect(row, 8 * s, Brush(kColorHover));
        Text(i18n::T(label), x0, yy, cw - 54 * s, rowH, 12.5f * s, Brush(kColorText));
        const float sw = 38 * s, sh = 22 * s;
        D2D1_RECT_F track = D2D1::RectF(x1 - sw, yy + (rowH - sh) * 0.5f, x1, yy + (rowH + sh) * 0.5f);
        FillRoundRect(track, sh * 0.5f,
                      Brush(on ? D2D1::ColorF(0x5B9CFF, 0.85f) : D2D1::ColorF(0x2A2F36)));
        const float kr = (sh - 4 * s) * 0.5f;
        const float kx = on ? track.right - kr - 2 * s : track.left + kr + 2 * s;
        rt_->FillEllipse(
            D2D1::Ellipse(D2D1::Point2F(kx, (track.top + track.bottom) * 0.5f), kr, kr),
            Brush(D2D1::ColorF(0xFFFFFF, on ? 1.0f : 0.75f)));
        AddHit(row, id);
        yy += rowH;
    };

    auto section = [&](i18n::Str label) {
        yy += 8 * s;
        Text(i18n::T(label), x0, yy, cw, 16 * s, 10.5f * s, Brush(kColorText3));
        yy += 20 * s;
    };

    auto segment3 = [&](i18n::Str l0, i18n::Str l1, i18n::Str l2, int sel, int id0, int id1,
                        int id2) {
        const float h = 30 * s, gapT = 4 * s;
        const float w = (cw - gapT * 2) / 3.f;
        const i18n::Str labels[3] = {l0, l1, l2};
        const int ids[3] = {id0, id1, id2};
        for (int i = 0; i < 3; ++i) {
            const float tx = x0 + i * (w + gapT);
            D2D1_RECT_F r = D2D1::RectF(tx, yy, tx + w, yy + h);
            const bool on = (sel == i);
            FillRoundRect(r, 8 * s, Brush(on ? D2D1::ColorF(0x262B33) : D2D1::ColorF(0x1B1E23)));
            if (!on && hover_ == ids[i]) FillRoundRect(r, 8 * s, Brush(kColorHover));
            Text(i18n::T(labels[i]), r.left, r.top, w, h, 12 * s,
                 Brush(on ? kColorText : kColorText2), 2);
            if (on) StrokeRoundRect(r, 8 * s, Brush(D2D1::ColorF(0x5B9CFF, 0.55f)), 1.f);
            AddHit(r, ids[i]);
        }
        yy += h + 8 * s;
    };

    auto smallButton = [&](D2D1_RECT_F b, int id, bool minus, bool plus) {
        FillRoundRect(b, 7 * s, Brush(D2D1::ColorF(0x1B1E23)));
        if (hover_ == id) FillRoundRect(b, 7 * s, Brush(kColorHover));
        const float bcx = (b.left + b.right) * 0.5f, bcy = (b.top + b.bottom) * 0.5f;
        const float g = 4.5f * s;
        ID2D1SolidColorBrush* fg = Brush(kColorText);
        if (minus || plus) {
            rt_->DrawLine(D2D1::Point2F(bcx - g, bcy), D2D1::Point2F(bcx + g, bcy), fg, 1.3f * s);
        }
        if (plus) {
            rt_->DrawLine(D2D1::Point2F(bcx, bcy - g), D2D1::Point2F(bcx, bcy + g), fg, 1.3f * s);
        }
        AddHit(b, id);
    };

    auto textButton = [&](i18n::Str label, D2D1_RECT_F b, int id) {
        FillRoundRect(b, 7 * s, Brush(D2D1::ColorF(0x1B1E23)));
        if (hover_ == id) FillRoundRect(b, 7 * s, Brush(kColorHover));
        Text(i18n::T(label), b.left, b.top, b.right - b.left, b.bottom - b.top, 11.5f * s,
             Brush(kColorText2), 2);
        AddHit(b, id);
    };

    auto wideButton = [&](i18n::Str label, int id, bool primary) {
        const float h = 34 * s;
        D2D1_RECT_F r = D2D1::RectF(x0, yy + 4 * s, x1, yy + 4 * s + h);
        FillRoundRect(r, 8 * s, Brush(primary ? D2D1::ColorF(0x5B9CFF, 0.14f)
                                              : D2D1::ColorF(0x1B1E23)));
        if (hover_ == id) FillRoundRect(r, 8 * s, Brush(kColorHover));
        StrokeRoundRect(r, 8 * s,
                        Brush(primary ? D2D1::ColorF(0x5B9CFF, 0.5f) : kColorBorder), 1.f);
        Text(i18n::T(label), r.left, r.top, x1 - x0, h, 12.5f * s,
             Brush(primary ? kColorAccent : kColorText2), 2);
        AddHit(r, id);
        yy += h + 10 * s;
    };

    if (settingsTab_ == 0) {
        section(i18n::kLanguage);
        segment3(i18n::kLangSystem, i18n::kLangZh, i18n::kLangEn, cfg.language, kSetLangAuto,
                 kSetLangZh, kSetLangEn);

        section(i18n::kSectionWindow);
        {
            // 界面缩放：文字 + [−] 百分比 [+] [重置]
            const float h = 38 * s, bh = 26 * s, bw = 30 * s;
            const float byy = yy + (h - bh) * 0.5f;
            Text(i18n::T(i18n::kUiScale), x0, yy, cw * 0.45f, h, 12.5f * s, Brush(kColorText));
            const float resetW = MeasureText(i18n::T(i18n::kReset), 11.5f * s, false) + 20 * s;
            D2D1_RECT_F resetRc = D2D1::RectF(x1 - resetW, byy, x1, byy + bh);
            D2D1_RECT_F plusRc =
                D2D1::RectF(resetRc.left - 10 * s - bw, byy, resetRc.left - 10 * s, byy + bh);
            D2D1_RECT_F minusRc = D2D1::RectF(plusRc.left - 4 * s - bw, byy, plusRc.left - 4 * s,
                                              byy + bh);
            smallButton(minusRc, kSetZoomOut, true, false);
            smallButton(plusRc, kSetZoomIn, false, true);
            textButton(i18n::kReset, resetRc, kSetZoomReset);
            wchar_t buf[32];
            swprintf_s(buf, L"%d%%", (int)std::lround(cfg.uiScale * 100.0f));
            Text(buf, minusRc.left - 54 * s, yy, 48 * s, h, 12 * s, Brush(kColorText2), 1);
            yy += h;
        }
        toggleRow(i18n::kFilmstrip, stripVisible_, kSetTgFilmstrip);
        toggleRow(i18n::kHideChrome, cfg.hideChrome, kSetTgHideChrome);

        section(i18n::kSectionView);
        toggleRow(i18n::kWheelNav, cfg.wheelNavigates, kSetTgWheelNav);
        toggleRow(i18n::kFitUpscale, cfg.fitUpscale, kSetTgUpscale);

        yy += 8 * s;
        wideButton(i18n::kSetDefaultApp, kSetRegister, true);
        if (regBlocked_ && !lastRegFile_.empty()) {
            // 被系统/安全软件拦下时不再弹自家对话框：改成面板里的两步引导
            Text(i18n::T(i18n::kRegBlockedHint), x0, yy - 2 * s, cw, 30 * s, 10.5f * s,
                 Brush(kColorText3));
            yy += 28 * s;
            wideButton(i18n::kImportRegFile, kSetImportReg, true);
            wideButton(i18n::kOpenSystemSettings, kSetOpenApps, false);
        }
        if (!toast_.empty() && NowMs() < toastUntil_) {
            Text(toast_, x0, yy - 4 * s, cw, 34 * s, 11 * s, Brush(kColorText3));
            yy += 26 * s;
        }
    } else if (settingsTab_ == 1) {
        Text(i18n::T(i18n::kKeysHint), x0, yy, cw, 20 * s, 10.5f * s, Brush(kColorText3));
        yy += 24 * s;
        for (int i = 0; i < keys::kCount; ++i) {
            const keys::Cmd c = (keys::Cmd)i;
            const float h = 34 * s;
            const bool cap = (captureCmd_ == i);
            D2D1_RECT_F row = D2D1::RectF(x0 - 8 * s, yy, x1 + 8 * s, yy + h);
            if (cap) FillRoundRect(row, 8 * s, Brush(D2D1::ColorF(0x5B9CFF, 0.16f)));
            else if (hover_ == kSetKeyBase + i) FillRoundRect(row, 8 * s, Brush(kColorHover));

            std::wstring txt;
            if (cap) {
                txt = i18n::T(i18n::kKeysCapturing);
            } else {
                std::vector<keys::Chord> chords = keys::Current(c);
                for (size_t k = 0; k < chords.size(); ++k) {
                    if (k) txt += L" · ";
                    txt += keys::Format(chords[k]);
                }
                if (txt.empty()) txt = i18n::T(i18n::kKeysEmpty);
            }
            const float chipH = 24 * s;
            const float chipW = MeasureText(txt, 11 * s, false) + 20 * s;
            D2D1_RECT_F chip =
                D2D1::RectF(x1 - chipW, yy + (h - chipH) * 0.5f, x1, yy + (h + chipH) * 0.5f);
            FillRoundRect(chip, 6 * s, Brush(D2D1::ColorF(0x24282F)));
            if (cap) StrokeRoundRect(chip, 6 * s, Brush(D2D1::ColorF(0x5B9CFF, 0.8f)), 1.2f * s);
            Text(txt, chip.left, chip.top, chipW, chipH, 11 * s,
                 Brush(cap ? kColorAccent : kColorText2), 2);
            Text(i18n::T(keys::LabelStr(c)), x0, yy, cw - chipW - 12 * s, h, 12.5f * s,
                 Brush(kColorText));
            AddHit(row, kSetKeyBase + i);
            yy += h;
        }
    } else {
        Text(std::wstring(i18n::T(i18n::kAppName)) + L" · " + i18n::T(i18n::kVersion) + L" 0.3.1",
             x0, yy, cw, 28 * s, 14 * s, Brush(kColorText));
        yy += 30 * s;
        Text(i18n::T(i18n::kAboutTagline), x0, yy, cw, 22 * s, 12 * s, Brush(kColorText2));
        yy += 28 * s;
        Text(i18n::T(i18n::kAboutTech), x0, yy, cw, 44 * s, 11 * s, Brush(kColorText3));
        yy += 40 * s;
        Text(i18n::T(i18n::kAboutPerf), x0, yy, cw, 22 * s, 11 * s, Brush(kColorText3));
        yy += 30 * s;
        Text(i18n::T(i18n::kAboutRepo), x0, yy, cw, 22 * s, 11.5f * s, Brush(kColorAccent));
        yy += 26 * s;
        Text(i18n::T(i18n::kAboutLicense), x0, yy, cw, 22 * s, 11 * s, Brush(kColorText3));
        yy += 28 * s;
    }

    rt_->PopAxisAlignedClip();

    // 滚动范围：内容高 - 可视高
    const float contentH = (yy + setScroll_) - clipTop;
    setScrollMax_ = std::max(0.f, contentH - clipH);
    if (setScroll_ > setScrollMax_) setScroll_ = setScrollMax_;

    // 细滚动条
    if (setScrollMax_ > 0.f && clipH > 1.f) {
        const float thumbH = std::max(28 * s, clipH * (clipH / (clipH + setScrollMax_)));
        const float t = setScroll_ / setScrollMax_;
        const float thumbTop = clip.top + (clipH - thumbH) * t;
        const float sbx = panelRc_.right - 7 * s;
        FillRoundRect(D2D1::RectF(sbx, thumbTop, sbx + 3 * s, thumbTop + thumbH), 1.5f * s,
                      Brush(D2D1::ColorF(0xFFFFFF, 0.16f)));
    }

    // 快捷键页的固定页脚：Esc 提示 + 恢复默认（不随列表滚动）
    if (footerH > 0.f) {
        const float fy = clip.bottom;
        rt_->DrawLine(D2D1::Point2F(x0, fy + 1.f), D2D1::Point2F(x1, fy + 1.f),
                      Brush(kColorBorder), 1.f);
        Text(i18n::T(i18n::kKeysEscHint), x0, fy + 6 * s, cw, 18 * s, 10.5f * s,
             Brush(kColorText3));
        D2D1_RECT_F btn = D2D1::RectF(x0, fy + 26 * s, x1, fy + 26 * s + 32 * s);
        FillRoundRect(btn, 8 * s, Brush(D2D1::ColorF(0x1B1E23)));
        if (hover_ == kSetKeysReset) FillRoundRect(btn, 8 * s, Brush(kColorHover));
        StrokeRoundRect(btn, 8 * s, Brush(kColorBorder), 1.f);
        Text(i18n::T(i18n::kKeysResetAll), btn.left, btn.top, x1 - x0, 32 * s, 12.5f * s,
             Brush(kColorText2), 2);
        AddHit(btn, kSetKeysReset);
    }
}

void App::RequestThumbs() {
    size_t n = loader_.FileCount();
    if (n == 0) return;
    const int64_t win = 40;
    int64_t lo = (int64_t)curIndex_ - win;
    int64_t hi = (int64_t)curIndex_ + win;
    if (lo < 0) lo = 0;
    if (hi > (int64_t)n - 1) hi = (int64_t)n - 1;
    for (int64_t i = lo; i <= hi; ++i) {
        uint32_t idx = (uint32_t)i;
        if (thumbs_.find(idx) != thumbs_.end()) continue;
        loader_.Request(idx, kLevelThumb, 2);
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
    RECT crc{};
    GetClientRect(hwnd_, &crc);
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
               L"  稳态每帧（平移 30 帧平均）: %.1f ms  (~%.0f fps)\n"
               L"  界面缩放 %.2fx（显示器 %.0f%%）/ 胶片条 %.0f px / 窗口 %ld×%ld 客户区 %ld×%ld\n",
               procMs, preMain, tFactory_ - tStart_, tWindowCreated_ - tFactory_,
               benchFileCount_, tOpen_ - tWindowCreated_, firstPaintMs_, deviceMs_, drawMs_,
               std::max<double>(0, tFirstImage_ - tWindowShown_ - readMs_ - decMs_ - uploadMs_),
               readMs_, decMs_, uploadMs_, frameMs_, frameMs_ > 0 ? 1000.0 / frameMs_ : 0.0,
               dpiScale_, monitorScale_ * 100.0, StripHeight(),
               (long)(winRect_.right - winRect_.left), (long)(winRect_.bottom - winRect_.top),
               (long)crc.right, (long)crc.bottom);
    (void)scanMs_;
    OutputDebugStringW(buf);
    FILE* f = nullptr;
    if (_wfopen_s(&f, L"seesee-bench.txt", L"w, ccs=UTF-8") == 0 && f) {
        fputws(buf, f);
        fclose(f);
    }
}
