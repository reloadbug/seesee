// 一次性探针：测量 D3D11/DXGI/D2D 各阶段冷启动耗时（不属于产品代码）
#include <windows.h>
#include <d3d11.h>
#include <d2d1_1.h>
#include <dxgi1_2.h>
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

int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    double t0 = NowMs();

    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl{};
    D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, want, 3,
                                   D3D11_SDK_VERSION, &dev, &fl, &ctx);
    double t1 = NowMs();
    printf("D3D11CreateDevice      %7.1f ms  hr=0x%08lX fl=0x%X\n", t1 - t0, (unsigned long)hr, fl);

    // 建窗
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"probe";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, L"probe", L"probe", WS_OVERLAPPEDWINDOW, 100, 100, 1200, 800,
                                nullptr, nullptr, wc.hInstance, nullptr);
    double t2 = NowMs();
    printf("CreateWindow           %7.1f ms\n", t2 - t1);

    IDXGIDevice* dxgiDev = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory2* factory = nullptr;
    dev->QueryInterface(IID_PPV_ARGS(&dxgiDev));
    dxgiDev->GetAdapter(&adapter);
    adapter->GetParent(IID_PPV_ARGS(&factory));

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = 1200;
    sd.Height = 800;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    IDXGISwapChain1* sc = nullptr;
    hr = factory->CreateSwapChainForHwnd(dev, hwnd, &sd, nullptr, nullptr, &sc);
    double t3 = NowMs();
    printf("CreateSwapChainForHwnd %7.1f ms  hr=0x%08lX\n", t3 - t2, (unsigned long)hr);

    IDXGISurface* surf = nullptr;
    if (sc) sc->GetBuffer(0, IID_PPV_ARGS(&surf));
    double t4 = NowMs();
    printf("GetBuffer              %7.1f ms\n", t4 - t3);

    ID2D1Factory1* d2f = nullptr;
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr,
                      reinterpret_cast<void**>(&d2f));
    double t5 = NowMs();
    printf("D2D factory(1.1)       %7.1f ms\n", t5 - t4);

    ID2D1Device* d2dev = nullptr;
    if (d2f) d2f->CreateDevice(dxgiDev, &d2dev);
    double t6 = NowMs();
    printf("D2D1Factory::CreateDevice %4.1f ms\n", t6 - t5);

    ID2D1DeviceContext* dc = nullptr;
    if (d2dev) d2dev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc);
    double t7 = NowMs();
    printf("CreateDeviceContext    %7.1f ms\n", t7 - t6);

    ID2D1Bitmap1* target = nullptr;
    if (dc && surf) {
        D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96, 96);
        dc->CreateBitmapFromDxgiSurface(surf, &bp, &target);
    }
    double t8 = NowMs();
    printf("CreateBitmapFromSurface %6.1f ms\n", t8 - t7);

    if (dc) {
        dc->SetTarget(target);
        dc->BeginDraw();
        dc->Clear(D2D1::ColorF(0.1f, 0.1f, 0.1f));
        dc->EndDraw();
    }
    double t9 = NowMs();
    printf("first BeginDraw/EndDraw %6.1f ms\n", t9 - t8);
    if (sc) {
        sc->Present(1, 0);
        double t10 = NowMs();
        printf("first Present          %7.1f ms\n", t10 - t9);
        printf("TOTAL                  %7.1f ms\n", t10 - t0);
    }

    // 顺便测：D2D 1.0 HwndRenderTarget 路线
    double a0 = NowMs();
    ID2D1Factory* f10 = nullptr;
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &f10);
    double a1 = NowMs();
    ID2D1HwndRenderTarget* rt = nullptr;
    D2D1_RENDER_TARGET_PROPERTIES rtp = D2D1::RenderTargetProperties();
    D2D1_HWND_RENDER_TARGET_PROPERTIES hrtp =
        D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(1200, 800), D2D1_PRESENT_OPTIONS_IMMEDIATELY);
    if (f10) f10->CreateHwndRenderTarget(rtp, hrtp, &rt);
    double a2 = NowMs();
    printf("[2nd] D2D1.0 factory %.1f ms, CreateHwndRenderTarget %.1f ms (warm driver)\n", a1 - a0,
           a2 - a1);
    return 0;
}
