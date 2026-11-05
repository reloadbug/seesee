// seesee / 看看 — 设置读写
#include "settings.h"

#include <windows.h>
#include <shlobj.h>

#include <cmath>
#include <cstdio>
#include <cwchar>

Settings& Settings::Get() {
    static Settings s;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        s.Load();
    }
    return s;
}

std::wstring Settings::Path() const {
    PWSTR dir = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &dir))) {
        out = dir;
        out += L"\\SeeSee";
        CreateDirectoryW(out.c_str(), nullptr);
        out += L"\\settings.ini";
    }
    if (dir) CoTaskMemFree(dir);
    return out;
}

void Settings::Load() {
    std::wstring p = Path();
    if (p.empty()) return;
    FILE* f = nullptr;
    if (_wfopen_s(&f, p.c_str(), L"rb") != 0 || !f) return;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char key[128] = {0};
        int value = 0;
        if (sscanf_s(line, "%127[^=]=%d", key, (unsigned)sizeof(key), &value) == 2) {
            if (strcmp(key, "preloadPrev") == 0) preloadPrev = value;
            else if (strcmp(key, "preloadNext") == 0) preloadNext = value;
            else if (strcmp(key, "workerThreads") == 0) workerThreads = value;
            else if (strcmp(key, "fitUpscale") == 0) fitUpscale = value != 0;
            else if (strcmp(key, "wheelNavigates") == 0) wheelNavigates = value != 0;
            else if (strcmp(key, "hideChrome") == 0) hideChrome = value != 0;
            else if (strcmp(key, "uiScalePct") == 0) uiScale = (float)value / 100.0f;
            else if (strcmp(key, "filmstrip") == 0) filmstrip = value != 0;
        }
    }
    fclose(f);

    if (preloadPrev < 0) preloadPrev = 0;
    if (preloadPrev > 20) preloadPrev = 20;
    if (preloadNext < 0) preloadNext = 0;
    if (preloadNext > 20) preloadNext = 20;
    if (workerThreads < 1) workerThreads = 1;
    if (workerThreads > 8) workerThreads = 8;
    if (uiScale < 0.75f) uiScale = 0.75f;
    if (uiScale > 3.0f) uiScale = 3.0f;
}

void Settings::Save() const {
    std::wstring p = Path();
    if (p.empty()) return;
    FILE* f = nullptr;
    if (_wfopen_s(&f, p.c_str(), L"wb") != 0 || !f) return;
    fprintf(f, "preloadPrev=%d\n", preloadPrev);
    fprintf(f, "preloadNext=%d\n", preloadNext);
    fprintf(f, "workerThreads=%d\n", workerThreads);
    fprintf(f, "fitUpscale=%d\n", fitUpscale ? 1 : 0);
    fprintf(f, "wheelNavigates=%d\n", wheelNavigates ? 1 : 0);
    fprintf(f, "hideChrome=%d\n", hideChrome ? 1 : 0);
    fprintf(f, "uiScalePct=%d\n", (int)std::lround(uiScale * 100.0f));
    fprintf(f, "filmstrip=%d\n", filmstrip ? 1 : 0);
    fclose(f);
}
