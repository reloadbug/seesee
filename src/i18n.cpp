// seesee / 看看 — 多语言字符串表
#include "i18n.h"

#include <windows.h>

namespace i18n {
namespace {

struct Row {
    Str id;
    const wchar_t* zh;
    const wchar_t* en;
};

// 顺序必须与 enum Str 一致
const Row kTable[] = {
    {kAppName, L"看看", L"SeeSee"},
    {kAppTitle, L"看看 SeeSee", L"SeeSee"},
    {kEmptyHint, L"拖入图片，或按 Ctrl+O 打开", L"Drop an image here, or press Ctrl+O"},
    {kLoading, L"载入中…", L"Loading…"},
    {kCannotOpen, L"无法打开此图片", L"Cannot open this image"},

    {kSettingsTitle, L"设置", L"Settings"},
    {kTabGeneral, L"通用", L"General"},
    {kTabKeys, L"快捷键", L"Shortcuts"},
    {kTabAbout, L"关于", L"About"},
    {kSectionView, L"查看", L"Viewing"},
    {kSectionWindow, L"界面", L"Interface"},
    {kLanguage, L"语言", L"Language"},
    {kLangSystem, L"跟随系统", L"System"},
    {kLangZh, L"中文", L"中文"},
    {kLangEn, L"English", L"English"},
    {kUiScale, L"界面缩放", L"UI scale"},
    {kReset, L"重置", L"Reset"},
    {kFilmstrip, L"底部胶片条", L"Filmstrip"},
    {kHideChrome, L"控件自动隐藏", L"Auto-hide controls"},
    {kWheelNav, L"滚轮翻页", L"Wheel changes image"},
    {kFitUpscale, L"小图放大到窗口", L"Upscale small images"},
    {kSetDefaultApp, L"设为默认看图工具", L"Set as default viewer"},
    {kSetDefaultDone, L"已注册，请在系统「默认应用」里确认", L"Registered — confirm it in Default apps"},
    {kSetDefaultFailed, L"注册被拒绝，已生成 .reg 文件", L"Registration refused — a .reg file was written"},

    {kKeysHint, L"点右侧按键即可重新设置", L"Click a key on the right to rebind"},
    {kKeysCapturing, L"请按新按键…", L"Press a new key…"},
    {kKeysEmpty, L"未设置", L"Unbound"},
    {kKeysResetAll, L"恢复默认", L"Restore defaults"},
    {kKeysEscHint, L"Esc 取消 · Delete 清除", L"Esc cancels · Delete clears"},
    {kKeysConflict, L"该键已用于「%s」", L"Already used by \u201c%s\u201d"},

    {kAboutTagline, L"极速 Windows 看图工具", L"A fast image viewer for Windows"},
    {kAboutTech, L"原生 C++20 · Win32 · Direct2D · WIC · 无运行时依赖", L"Native C++20 · Win32 · Direct2D · WIC · no runtime deps"},
    {kAboutPerf, L"冷启动约 80 ms · 4K 下 60 fps 以上", L"~80 ms cold start · 60 fps+ at 4K"},
    {kAboutRepo, L"github.com/reloadbug/seesee", L"github.com/reloadbug/seesee"},
    {kAboutLicense, L"MIT License", L"MIT License"},
    {kVersion, L"版本", L"Version"},

    {kCmdPrev, L"上一张", L"Previous image"},
    {kCmdNext, L"下一张", L"Next image"},
    {kCmdFirst, L"第一张", L"First image"},
    {kCmdLast, L"最后一张", L"Last image"},
    {kCmdZoomIn, L"放大", L"Zoom in"},
    {kCmdZoomOut, L"缩小", L"Zoom out"},
    {kCmdFit, L"适应窗口", L"Fit to window"},
    {kCmdActual, L"原始大小", L"Actual size"},
    {kCmdFullscreen, L"全屏", L"Fullscreen"},
    {kCmdOpen, L"打开文件", L"Open file"},
    {kCmdDefaultApp, L"设为默认看图工具", L"Register as default viewer"},
    {kCmdStrip, L"显示 / 隐藏胶片条", L"Toggle filmstrip"},
    {kCmdSettings, L"打开设置", L"Open settings"},
    {kCmdUiScaleUp, L"界面放大", L"Larger UI"},
    {kCmdUiScaleDown, L"界面缩小", L"Smaller UI"},
    {kCmdCopyImage, L"复制图片", L"Copy image"},
    {kCmdCopyFile, L"复制文件", L"Copy file"},
    {kCmdCutFile, L"剪切文件", L"Cut file"},
    {kCmdCopyPath, L"复制路径", L"Copy path"},
    {kCmdReveal, L"在资源管理器中显示", L"Show in Explorer"},
    {kCmdEdit, L"用默认程序编辑", L"Edit in default app"},
    {kCmdDelete, L"移到回收站", L"Move to Recycle Bin"},
    {kRegBlockedHint, L"系统阻止了自动写入，请点下方「导入关联文件」",
     L"Automatic registration was blocked - use \"Import association file\" below"},
    {kImportRegFile, L"导入关联文件", L"Import association file"},
    {kOpenSystemSettings, L"打开系统设置", L"Open Windows settings"},
    {kCopiedImage, L"图片已复制到剪贴板", L"Image copied to clipboard"},
    {kCopiedFile, L"文件已复制", L"File copied"},
    {kCutToClipboard, L"文件已剪切", L"File cut"},
    {kCopiedPath, L"路径已复制", L"Path copied"},
    {kOpFailed, L"操作失败", L"Operation failed"},
    {kDeletedFile, L"已移到回收站", L"Moved to Recycle Bin"},
    {kMenuSettings, L"设置…", L"Settings…"},
    {kMenuExit, L"退出", L"Exit"},
    {kRawPreview, L"RAW 预览", L"RAW preview"},
};

int g_setting = 0;   // 0 = 跟随系统
bool g_zh = true;

bool DetectSystemChinese() {
    LANGID lang = GetUserDefaultUILanguage();
    return PRIMARYLANGID(lang) == LANG_CHINESE;
}

}  // namespace

void SetLanguageSetting(int v) {
    if (v < 0 || v > 2) v = 0;
    g_setting = v;
    g_zh = (v == 0) ? DetectSystemChinese() : (v == 1);
}

int LanguageSetting() { return g_setting; }

bool IsChinese() { return g_zh; }

const wchar_t* T(Str s) {
    if ((int)s < 0 || s >= kCount) return L"";
    const Row& r = kTable[s];
    return g_zh ? r.zh : r.en;
}

}  // namespace i18n
