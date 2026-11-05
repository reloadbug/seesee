// seesee / 看看 — 键位表：默认值、用户覆盖、持久化、匹配
#include "keys.h"

#include "settings.h"

#include <cstdio>
#include <cstdlib>
#include <cwchar>

namespace keys {
namespace {

const wchar_t* const kIds[kCount] = {
    L"Prev",   L"Next",   L"First",  L"Last",      L"ZoomIn",   L"ZoomOut", L"Fit",
    L"Actual", L"Fullscreen", L"Open", L"DefaultApp", L"Strip",  L"Settings",
    L"UiScaleUp", L"UiScaleDown",
    L"CopyImage", L"CopyFile", L"CutFile", L"CopyPath", L"Reveal", L"Edit", L"Delete",
};

const i18n::Str kLabels[kCount] = {
    i18n::kCmdPrev,     i18n::kCmdNext,     i18n::kCmdFirst,  i18n::kCmdLast,
    i18n::kCmdZoomIn,   i18n::kCmdZoomOut,  i18n::kCmdFit,    i18n::kCmdActual,
    i18n::kCmdFullscreen, i18n::kCmdOpen,   i18n::kCmdDefaultApp, i18n::kCmdStrip,
    i18n::kCmdSettings, i18n::kCmdUiScaleUp, i18n::kCmdUiScaleDown,
    i18n::kCmdCopyImage, i18n::kCmdCopyFile, i18n::kCmdCutFile, i18n::kCmdCopyPath,
    i18n::kCmdReveal, i18n::kCmdEdit, i18n::kCmdDelete,
};

Chord Make(UINT vk, bool ctrl = false, bool shift = false, bool alt = false) {
    Chord c;
    c.vk = vk;
    c.ctrl = ctrl;
    c.shift = shift;
    c.alt = alt;
    return c;
}

// 用户覆盖 → 存在 Settings::keybind 里，值形如 "39:0"（vk:修饰键位）
std::string IdA(Cmd c) {
    const wchar_t* w = kIds[c];
    std::string s;
    for (; *w; ++w) s.push_back((char)*w);
    return s;
}

std::wstring Store(const Chord& ch) {
    wchar_t buf[32];
    int mods = (ch.ctrl ? 1 : 0) | (ch.shift ? 2 : 0) | (ch.alt ? 4 : 0);
    swprintf_s(buf, L"%u:%d", ch.vk, mods);
    return buf;
}

bool LoadStore(const std::wstring& text, Chord* out) {
    unsigned vk = 0;
    int mods = 0;
    if (swscanf_s(text.c_str(), L"%u:%d", &vk, &mods) != 2) return false;
    out->vk = vk;
    out->ctrl = (mods & 1) != 0;
    out->shift = (mods & 2) != 0;
    out->alt = (mods & 4) != 0;
    return true;
}

// 媒体键/方向键等需要扩展位才能拿到正确的系统键名
bool IsExtendedKey(UINT vk) {
    switch (vk) {
        case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END:
        case VK_PRIOR: case VK_NEXT: case VK_LEFT: case VK_RIGHT:
        case VK_UP: case VK_DOWN: case VK_DIVIDE: case VK_NUMLOCK:
        case VK_RCONTROL: case VK_RMENU: case VK_LWIN: case VK_RWIN:
        case VK_APPS: case VK_SNAPSHOT:
            return true;
        default:
            return false;
    }
}

std::wstring KeyName(UINT vk) {
    if (vk == 0) return std::wstring(i18n::T(i18n::kKeysEmpty));
    // 系统给出的是"未按 Shift 时"的字符（= , . 等），这里改用更直观的符号
    switch (vk) {
        case VK_OEM_PLUS: return L"+";
        case VK_OEM_MINUS: return L"-";
        case VK_OEM_COMMA: return L",";
        case VK_OEM_PERIOD: return L".";
        default: break;
    }
    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    LONG lp = (LONG)(sc << 16);
    if (IsExtendedKey(vk)) lp |= (1L << 24);
    wchar_t buf[64] = {0};
    if (GetKeyNameTextW(lp, buf, 64) > 0) return buf;
    wchar_t tmp[16];
    swprintf_s(tmp, L"0x%02X", vk);
    return tmp;
}

}  // namespace

const wchar_t* Id(Cmd c) { return ((int)c >= 0 && c < kCount) ? kIds[c] : L""; }

i18n::Str LabelStr(Cmd c) { return ((int)c >= 0 && c < kCount) ? kLabels[c] : i18n::kCmdPrev; }

std::vector<Chord> Defaults(Cmd c) {
    std::vector<Chord> v;
    switch (c) {
        case kPrev: v = {Make(VK_LEFT), Make(VK_UP), Make(VK_PRIOR)}; break;
        case kNext: v = {Make(VK_RIGHT), Make(VK_DOWN), Make(VK_NEXT)}; break;
        case kFirst: v = {Make(VK_HOME)}; break;
        case kLast: v = {Make(VK_END)}; break;
        case kZoomIn: v = {Make(VK_OEM_PLUS), Make(VK_ADD)}; break;
        case kZoomOut: v = {Make(VK_OEM_MINUS), Make(VK_SUBTRACT)}; break;
        case kFit: v = {Make('0')}; break;
        case kActual: v = {Make('1')}; break;
        case kFullscreen: v = {Make(VK_F11)}; break;
        case kOpen: v = {Make('O', true)}; break;
        case kDefaultApp: v = {Make('D', true)}; break;
        case kStrip: v = {Make('T')}; break;
        case kSettings: v = {Make(VK_OEM_COMMA, true)}; break;
        case kUiScaleUp: v = {Make(VK_OEM_PLUS, true, true)}; break;
        case kUiScaleDown: v = {Make(VK_OEM_MINUS, true, true)}; break;
        case kCopyImage: v = {Make('C', true, true)}; break;
        case kCopyFile: v = {Make('C', true)}; break;
        case kCutFile: v = {Make('X', true)}; break;
        case kCopyPath: v = {Make('C', true, false, true)}; break;
        case kReveal: v = {Make('E', true, true)}; break;
        case kEdit: v = {Make('E', true)}; break;
        case kDelete: v = {Make(VK_DELETE)}; break;
        default: break;
    }
    return v;
}

bool HasOverride(Cmd c) {
    return Settings::Get().keybind.find(IdA(c)) != Settings::Get().keybind.end();
}

std::vector<Chord> Current(Cmd c) {
    auto& map = Settings::Get().keybind;
    auto it = map.find(IdA(c));
    if (it == map.end()) return Defaults(c);
    Chord ch;
    if (!LoadStore(it->second, &ch)) return Defaults(c);
    if (!ch.valid()) return {};  // 用户清成了"未设置"
    return {ch};
}

void Set(Cmd c, const Chord& ch) {
    Settings& cfg = Settings::Get();
    cfg.keybind[IdA(c)] = Store(ch);
    cfg.Save();
}

void Clear(Cmd c) {
    Settings& cfg = Settings::Get();
    cfg.keybind[IdA(c)] = L"0:0";
    cfg.Save();
}

void ResetAll() {
    Settings& cfg = Settings::Get();
    cfg.keybind.clear();
    cfg.Save();
}

void Load() {
    // 覆盖项在 Settings::Load 时已原样读入，这里只做格式校验（把非法项删掉）
    Settings& cfg = Settings::Get();
    for (auto it = cfg.keybind.begin(); it != cfg.keybind.end();) {
        Chord ch;
        if (!LoadStore(it->second, &ch)) it = cfg.keybind.erase(it);
        else ++it;
    }
}

std::wstring Format(const Chord& ch) {
    if (!ch.valid()) return std::wstring(i18n::T(i18n::kKeysEmpty));
    std::wstring out;
    if (ch.ctrl) out += L"Ctrl + ";
    if (ch.shift) out += L"Shift + ";
    if (ch.alt) out += L"Alt + ";
    out += KeyName(ch.vk);
    return out;
}

Chord FromVk(UINT vk) {
    Chord c;
    c.vk = vk;
    c.ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    c.shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    c.alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    return c;
}

Cmd Match(UINT vk, bool ctrl, bool shift, bool alt) {
    if (vk == 0) return kCount;
    for (int i = 0; i < kCount; ++i) {
        for (const Chord& ch : Current((Cmd)i)) {
            if (ch.vk == vk && ch.ctrl == ctrl && ch.shift == shift && ch.alt == alt) return (Cmd)i;
        }
    }
    return kCount;
}

std::vector<Cmd> All() {
    std::vector<Cmd> v;
    for (int i = 0; i < kCount; ++i) v.push_back((Cmd)i);
    return v;
}

}  // namespace keys
