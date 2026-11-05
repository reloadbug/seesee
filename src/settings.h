// seesee / 看看 — 极简 settings.ini
#pragma once
#include <string>

struct Settings {
    int preloadPrev = 2;   // 预加载前面几张
    int preloadNext = 3;   // 预加载后面几张
    int workerThreads = 3; // 解码线程数
    bool fitUpscale = true; // 小图在"适应窗口"下是否放大
    bool wheelNavigates = true; // 滚轮翻页（Ctrl+滚轮 始终缩放）
    bool hideChrome = false;    // 鼠标静止后淡出控件（v0.2）

    static Settings& Get();
    void Load();
    void Save() const;
    std::wstring Path() const;
};
