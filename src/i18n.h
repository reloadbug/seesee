// seesee / 看看 — 多语言（简体中文 / English）
#pragma once

namespace i18n {

// 所有会显示给用户的字符串。加字符串只需在这里加一项 + 在 i18n.cpp 的表里补两行。
enum Str : int {
    kAppName = 0,   // 看看 / SeeSee
    kAppTitle,      // 看看 SeeSee / SeeSee
    kEmptyHint,
    kLoading,
    kCannotOpen,

    // 设置面板
    kSettingsTitle,
    kTabGeneral,
    kTabKeys,
    kTabAbout,
    kSectionView,
    kSectionWindow,
    kLanguage,
    kLangSystem,
    kLangZh,
    kLangEn,
    kUiScale,
    kReset,
    kFilmstrip,
    kHideChrome,
    kWheelNav,
    kFitUpscale,
    kSetDefaultApp,
    kSetDefaultDone,
    kSetDefaultFailed,

    // 快捷键页
    kKeysHint,
    kKeysCapturing,
    kKeysEmpty,
    kKeysResetAll,
    kKeysEscHint,
    kKeysConflict,

    // 关于页
    kAboutTagline,
    kAboutTech,
    kAboutPerf,
    kAboutRepo,
    kAboutLicense,
    kVersion,

    // 命令名（快捷键列表左侧）
    kCmdPrev,
    kCmdNext,
    kCmdFirst,
    kCmdLast,
    kCmdZoomIn,
    kCmdZoomOut,
    kCmdFit,
    kCmdActual,
    kCmdFullscreen,
    kCmdOpen,
    kCmdDefaultApp,
    kCmdStrip,
    kCmdSettings,
    kCmdUiScaleUp,
    kCmdUiScaleDown,

    // 图片操作（右键菜单 + 快捷键）
    kCmdCopyImage,
    kCmdCopyFile,
    kCmdCutFile,
    kCmdCopyPath,
    kCmdReveal,
    kCmdEdit,
    kCmdDelete,
    kCmdPlayPause,

    // 关联引导 / 操作结果
    kRegBlockedHint,
    kImportRegFile,
    kOpenSystemSettings,
    kCopiedImage,
    kCopiedFile,
    kCutToClipboard,
    kCopiedPath,
    kOpFailed,
    kDeletedFile,
    kMenuSettings,
    kMenuExit,
    kRawPreview,
    kAnimTag,      // 信息胶囊里的「动图」标记

    kCount
};

// v: 0 = 跟随系统，1 = 简体中文，2 = English
void SetLanguageSetting(int v);
int LanguageSetting();
bool IsChinese();

const wchar_t* T(Str s);

}  // namespace i18n
