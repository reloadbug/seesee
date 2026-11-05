// seesee / 看看 — 主窗口与渲染
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "loader.h"
#include "settings.h"

enum class ZoomMode { Fit, Actual };

class App {
public:
    static App& Instance();

    int Run(HINSTANCE hInst, const std::wstring& initialFile, bool benchMode,
            const std::wstring& shotPath);

private:
    enum BtnId {
        kBtnNone = 0,
        kBtnPrev,
        kBtnNext,
        kBtnZoomOut,
        kBtnZoomIn,
        kBtnFit,
        kBtnActual,
        kBtnFullscreen,
        kBtnMin,
        kBtnMax,
        kBtnClose,
        kBtnArrowLeft,
        kBtnArrowRight,
        kBtnCount
    };

    struct Hit {
        D2D1_RECT_F rc;
        int id;
        bool circular;
    };

    struct CacheEntry {
        ComPtr<ID2D1Bitmap> bmp;
        uint32_t srcW = 0, srcH = 0;
        uint32_t decW = 0, decH = 0;
        int level = kLevelFit;
        uint64_t lastUse = 0;
    };

    // 生命周期
    bool CreateMainWindow(HINSTANCE hInst);
    void ShowMainWindow();
    RECT DefaultWindowRect() const;
    static LRESULT CALLBACK StaticWndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);
    void OnCreate();
    void Shutdown();

    // 打开 / 导航
    bool OpenPath(const std::wstring& path, bool keepView = false);
    void GoTo(int64_t index);
    void NextImage();
    void PrevImage();
    void ReloadCurrent();
    void UpdateRequests();
    void OnDecodedReady();

    // 视图
    RECT AreaRect() const;             // 图片区域（物理像素）
    D2D1_POINT_2F AreaCenter() const;
    double FitZoom() const;
    double CurrentZoom() const;
    void SetZoomAnchored(double newZoom, D2D1_POINT_2F anchor);
    void ClampOffset();
    void ResetViewToFit();
    void ToggleFullscreen();

    // 渲染
    bool EnsureDevice();
    bool EnsureBackBuffer(int w, int h);
    void DiscardDevice();
    void Paint();
    void RenderAndBlit();
    void MeasureFrames();
    void DrawImageLayer();
    void DrawChrome();
    void DrawTitleBar();
    void DrawBottomBar();
    void DrawInfoPill();
    void DrawArrows();
    void AddHit(D2D1_RECT_F rc, int id, bool circular = false);
    int HitTest(POINT pt) const;
    int HitTestScreen(POINT screenPt) const;

    // 文本 / 图形  (align: 0=左 1=右 2=居中)
    IDWriteTextFormat* Format(float sizePx, bool mono = false, int align = 0);
    void Text(const std::wstring& s, float x, float y, float w, float h, float sizePx,
              ID2D1Brush* brush, int align = 0);
    ID2D1SolidColorBrush* Brush(D2D1_COLOR_F c);
    void FillRoundRect(D2D1_RECT_F rc, float radius, ID2D1Brush* brush);
    void StrokeRoundRect(D2D1_RECT_F rc, float radius, ID2D1Brush* brush, float width);

    void ShowOpenDialog();
    void ReportBench();
    bool SaveShot(const std::wstring& path);
    double ProcElapsedMs();

    // ---- 状态 ----
    HINSTANCE hinst_ = nullptr;
    HWND hwnd_ = nullptr;
    Loader loader_;
    std::mutex pendingMtx_;
    std::vector<std::shared_ptr<DecodedImage>> pending_;

    ComPtr<ID2D1Factory> d2dFactory_;
    // 软件光栅后端：D2D DC 渲染目标 + 32bpp DIB + BitBlt
    // 理由见 docs/02-roadmap.md「为什么不用 D3D 硬件后端」：本机冷启动 D3D11CreateDevice
    // 就要 156ms，而软件后端建目标仅 9ms、首帧 3ms，首屏时间差一个数量级。
    ComPtr<ID2D1DCRenderTarget> rt_;
    HDC memDC_ = nullptr;
    HBITMAP dib_ = nullptr;
    void* dibBits_ = nullptr;
    SIZE bufSize_{0, 0};
    ComPtr<IDWriteFactory> dwrite_;
    std::map<uint32_t, ComPtr<ID2D1SolidColorBrush>> brushes_;
    std::map<std::string, ComPtr<IDWriteTextFormat>> formats_;

    std::map<uint64_t, CacheEntry> cache_;
    uint64_t useCounter_ = 0;

    uint32_t curIndex_ = 0;
    uint32_t requestedLevel_ = kLevelFit;
    bool haveImage_ = false;
    bool loading_ = false;
    std::wstring errorText_;

    ZoomMode zoomMode_ = ZoomMode::Fit;
    double zoom_ = 1.0;
    D2D1_POINT_2F offset_{0.f, 0.f};

    float dpiScale_ = 1.0f;
    float titleH_ = 34.f;
    bool fullscreen_ = false;
    bool maximized_ = false;
    RECT savedRect_{};
    LONG savedStyle_ = 0;

    int hover_ = kBtnNone;
    int pressed_ = kBtnNone;
    bool panning_ = false;
    POINT panStart_{};
    D2D1_POINT_2F panOffsetStart_{};

    std::vector<Hit> hits_;

    bool benchMode_ = false;
    std::wstring shotPath_;
    bool benchDone_ = false;
    // 冷启动分段耗时（bench 用，单位 ms，绝对时间轴）
    double tStart_ = 0;        // wWinMain 入口
    double tFactory_ = 0;      // D2D / DWrite 工厂就绪
    double tWindowCreated_ = 0;// CreateWindowExW 返回
    double tWindowShown_ = 0;  // 窗口已显示（空壳）
    double tOpen_ = 0;         // 目录扫描完成、解码请求已发出
    double tFirstImage_ = 0;   // 首张图片真正上屏
    double scanMs_ = 0;        // 扫目录耗时
    double readMs_ = 0;        // 读文件耗时
    double decMs_ = 0;         // 解码耗时
    double uploadMs_ = 0;      // CPU 像素 → GPU 位图
    double firstPaintMs_ = 0;  // 首次建 D2D 设备并绘制
    double deviceMs_ = 0;      // 首次 CreateDevice 耗时
    double drawMs_ = 0;        // 首帧绘制耗时
    double frameMs_ = 0;       // 稳态每帧耗时（30 帧平均）
    double tProcFirstImage_ = 0;  // 首张上屏那一刻的进程总耗时（含 exe 加载）
    uint32_t benchFileCount_ = 0;
    RECT winRect_{};
};
