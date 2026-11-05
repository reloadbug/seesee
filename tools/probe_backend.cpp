// 一次性探针 2：比较各种渲染后端冷启动代价（不属于产品代码）
#include <windows.h>
#include <d3d11.h>
#include <d2d1.h>
#include <d2d1_1.h>
#include <cstdio>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dxgi.lib")

static double NowMs() {
    static LARGE_INTEGER f{};
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) * 1000.0 / double(f.QuadPart);
}

int main(int argc, char** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"probe2";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, L"probe2", L"probe2", WS_OVERLAPPEDWINDOW, 100, 100, 1200, 800,
                                nullptr, nullptr, wc.hInstance, nullptr);

    int which = (argc > 1) ? atoi(argv[1]) : 0;

    if (which == 0) {
        double t0 = NowMs();
        ID3D11Device* dev = nullptr;
        ID3D11DeviceContext* ctx = nullptr;
        D3D_FEATURE_LEVEL fl{};
        D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                       D3D11_CREATE_DEVICE_BGRA_SUPPORT, want, 2, D3D11_SDK_VERSION,
                                       &dev, &fl, &ctx);
        double t1 = NowMs();
        printf("[hw]      D3D11CreateDevice %.1f ms hr=0x%08lX\n", t1 - t0, (unsigned long)hr);

        double t2 = NowMs();
        ID3D11Device* dev2 = nullptr;
        ID3D11DeviceContext* ctx2 = nullptr;
        D3D_FEATURE_LEVEL fl2{};
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                               D3D11_CREATE_DEVICE_BGRA_SUPPORT, want, 2, D3D11_SDK_VERSION, &dev2,
                               &fl2, &ctx2);
        printf("[hw 2nd]  D3D11CreateDevice %.1f ms hr=0x%08lX\n", NowMs() - t2, (unsigned long)hr);
        (void)dev, (void)ctx, (void)dev2, (void)ctx2;
    } else if (which == 1) {
        double t0 = NowMs();
        ID3D11Device* dev = nullptr;
        ID3D11DeviceContext* ctx = nullptr;
        D3D_FEATURE_LEVEL fl{};
        D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_0};
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                                       D3D11_CREATE_DEVICE_BGRA_SUPPORT, want, 1, D3D11_SDK_VERSION,
                                       &dev, &fl, &ctx);
        printf("[warp]    D3D11CreateDevice %.1f ms hr=0x%08lX\n", NowMs() - t0, (unsigned long)hr);
        if (dev) {
            IDXGIDevice* dg = nullptr;
            dev->QueryInterface(IID_PPV_ARGS(&dg));
            double a = NowMs();
            ID2D1Factory1* f1 = nullptr;
            D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr,
                              reinterpret_cast<void**>(&f1));
            ID2D1Device* d2d = nullptr;
            ID2D1DeviceContext* dc = nullptr;
            if (f1 && dg) f1->CreateDevice(dg, &d2d);
            if (d2d) d2d->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc);
            printf("[warp]    D2D device+context %.1f ms\n", NowMs() - a);
            (void)dc;
        }
    } else if (which == 2) {
        // D2D 1.0 硬件 HwndRenderTarget（产品当前路线）
        ID2D1Factory* f = nullptr;
        double t0 = NowMs();
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &f);
        ID2D1HwndRenderTarget* rt = nullptr;
        D2D1_RENDER_TARGET_PROPERTIES rtp = D2D1::RenderTargetProperties();
        D2D1_HWND_RENDER_TARGET_PROPERTIES hrtp = D2D1::HwndRenderTargetProperties(
            hwnd, D2D1::SizeU(1200, 800), D2D1_PRESENT_OPTIONS_IMMEDIATELY);
        if (f) f->CreateHwndRenderTarget(rtp, hrtp, &rt);
        printf("[d2d hwnd] factory+HwndRenderTarget %.1f ms\n", NowMs() - t0);
        (void)rt;
    } else if (which == 3) {
        // GDI 双缓冲：DIB + BitBlt
        double t0 = NowMs();
        HDC screen = GetDC(hwnd);
        HDC mem = CreateCompatibleDC(screen);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = 1200;
        bi.bmiHeader.biHeight = -800;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP dib = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        HGDIOBJ old = SelectObject(mem, dib);
        printf("[gdi]     DIB setup %.1f ms\n", NowMs() - t0);
        double t1 = NowMs();
        RECT rc{0, 0, 1200, 800};
        HBRUSH b = CreateSolidBrush(RGB(15, 17, 20));
        FillRect(mem, &rc, b);
        BitBlt(screen, 0, 0, 1200, 800, mem, 0, 0, SRCCOPY);
        printf("[gdi]     fill+blit %.1f ms\n", NowMs() - t1);
        SelectObject(mem, old);
        DeleteObject(dib);
        DeleteDC(mem);
        ReleaseDC(hwnd, screen);
    } else if (which == 4) {
        // D2D 软件：DC render target（画到 GDI 位图，再 BitBlt）
        double t0 = NowMs();
        ID2D1Factory* f = nullptr;
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &f);
        ID2D1DCRenderTarget* rt = nullptr;
        D2D1_RENDER_TARGET_PROPERTIES rtp = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96, 96);
        if (f) f->CreateDCRenderTarget(&rtp, &rt);
        printf("[d2d dc]  factory+CreateDCRenderTarget %.1f ms\n", NowMs() - t0);
        if (rt) {
            HDC screen = GetDC(hwnd);
            HDC mem = CreateCompatibleDC(screen);
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = 1200;
            bi.bmiHeader.biHeight = -800;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            void* bits = nullptr;
            HBITMAP dib = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
            HGDIOBJ old = SelectObject(mem, dib);
            RECT rc{0, 0, 1200, 800};
            double t1 = NowMs();
            rt->BindDC(mem, &rc);
            rt->BeginDraw();
            rt->Clear(D2D1::ColorF(0.06f, 0.07f, 0.08f));
            rt->EndDraw();
            double t2 = NowMs();
            BitBlt(screen, 0, 0, 1200, 800, mem, 0, 0, SRCCOPY);
            printf("[d2d dc]  first BindDC+draw %.1f ms, blit %.1f ms\n", t2 - t1, NowMs() - t2);
            SelectObject(mem, old);
            DeleteObject(dib);
            DeleteDC(mem);
            ReleaseDC(hwnd, screen);
        }
    }
    return 0;
}
