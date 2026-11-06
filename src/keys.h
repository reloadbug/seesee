// seesee / 看看 — 可自定义的键位
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>
#include <vector>

#include "i18n.h"

namespace keys {

enum Cmd : int {
    kPrev = 0,
    kNext,
    kFirst,
    kLast,
    kZoomIn,
    kZoomOut,
    kFit,
    kActual,
    kFullscreen,
    kOpen,
    kDefaultApp,
    kStrip,
    kSettings,
    kUiScaleUp,
    kUiScaleDown,
    kCopyImage,
    kCopyFile,
    kCutFile,
    kCopyPath,
    kReveal,
    kEdit,
    kDelete,
    kPlayPause,
    kCount
};

struct Chord {
    UINT vk = 0;  // 0 = 未绑定
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
    bool valid() const { return vk != 0; }
};

const wchar_t* Id(Cmd c);              // 写进 settings.ini 的稳定标识（ASCII）
i18n::Str LabelStr(Cmd c);             // 面板里显示的命令名
std::vector<Chord> Defaults(Cmd c);    // 出厂键位（可能多个）
std::vector<Chord> Current(Cmd c);     // 当前生效键位（用户覆盖优先）
bool HasOverride(Cmd c);               // 是否被用户改过
void Set(Cmd c, const Chord& ch);      // 改键（持久化）
void Clear(Cmd c);                     // 解除绑定（持久化）
void ResetAll();                       // 全部恢复默认（持久化）

std::wstring Format(const Chord& ch);           // "Ctrl + Shift + P"（键名跟随系统）
Chord FromVk(UINT vk);                          // 用当前修饰键状态组装
Cmd Match(UINT vk, bool ctrl, bool shift, bool alt);  // 返回 kCount = 没匹配上
void Load();                                    // 从 Settings 读覆盖
std::vector<Cmd> All();

}  // namespace keys
