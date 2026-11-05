// seesee / 看看 — 入口
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <string>

#include "app.h"
#include "assoc.h"

#pragma comment(lib, "shell32.lib")

namespace {

struct Args {
    std::wstring file;
    std::wstring shot;
    bool bench = false;
    bool help = false;
    bool reg = false;
    bool unreg = false;
    bool quiet = false;
    int panel = -1;      // --panel：0 通用 1 快捷键 2 关于（配合 --shot 截图验收）
    bool capture = false;  // --capture：模拟"正在等新键位"的状态
    int lang = -1;       // --lang：临时覆盖界面语言（不写盘）
};

Args ParseArgs() {
    Args a;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return a;
    for (int i = 1; i < argc; ++i) {
        std::wstring s = argv[i];
        if (s == L"--bench" || s == L"-b") {
            a.bench = true;
        } else if (s == L"--shot" && i + 1 < argc) {
            a.shot = argv[++i];
        } else if (s == L"--panel" && i + 1 < argc) {
            std::wstring v = argv[++i];
            if (v == L"general" || v == L"0") a.panel = 0;
            else if (v == L"keys" || v == L"1") a.panel = 1;
            else if (v == L"about" || v == L"2") a.panel = 2;
        } else if (s == L"--capture") {
            a.capture = true;
        } else if (s == L"--lang" && i + 1 < argc) {
            std::wstring v = argv[++i];
            if (v == L"auto" || v == L"0") a.lang = 0;
            else if (v == L"zh" || v == L"1") a.lang = 1;
            else if (v == L"en" || v == L"2") a.lang = 2;
        } else if (s == L"--register" || s == L"-r") {
            a.reg = true;
        } else if (s == L"--unregister") {
            a.unreg = true;
        } else if (s == L"--quiet" || s == L"-q") {
            a.quiet = true;
        } else if (s == L"--help" || s == L"-h" || s == L"/?") {
            a.help = true;
        } else if (!s.empty() && s[0] != L'-') {
            a.file = s;
        }
    }
    LocalFree(argv);
    return a;
}

const wchar_t* kHelp =
    L"看看 SeeSee — 极速看图\n\n"
    L"用法: seesee.exe [图片路径] [参数]\n\n"
    L"看图（全部可在「设置 → 快捷键」里改）:\n"
    L"  ← →  ↑ ↓     上一张 / 下一张\n"
    L"  PgUp / PgDn   上一张 / 下一张\n"
    L"  Home / End    第一张 / 最后一张\n"
    L"  滚轮          翻页（Ctrl+滚轮 缩放）\n"
    L"  双击          适应窗口 / 100% 切换\n"
    L"  0 / 1         适应窗口 / 原始大小\n"
    L"  + / -         放大 / 缩小\n"
    L"  F11           全屏（Esc 退出全屏）\n"
    L"  Ctrl+O        打开文件\n"
    L"  Ctrl+,        打开设置\n"
    L"  拖放文件      打开\n\n"
    L"界面:\n"
    L"  设置面板里有语言（跟随系统 / 中文 / English）与快捷键自定义\n"
    L"  Ctrl+Shift+= / Ctrl+Shift+-  缩放整个界面\n\n"
    L"关联:\n"
    L"  Ctrl+D        设为默认看图工具\n"
    L"  --register    同上（命令行）\n"
    L"  --unregister  取消关联\n\n"
    L"其它:\n"
    L"  --bench       性能基准（写 seesee-bench.txt）\n"
    L"  --shot <png>  把首帧存成 PNG 截图\n"
    L"  --panel <n>   截图时打开设置面板（0/1/2 或 general/keys/about）\n";

}  // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    SetErrorMode(SEM_FAILCRITICALERRORS);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    SetCurrentDirectoryW(nullptr);

    Args args = ParseArgs();
    if (args.help) {
        MessageBoxW(nullptr, kHelp, L"看看 SeeSee", MB_OK | MB_ICONINFORMATION);
        CoUninitialize();
        return 0;
    }
    if (args.reg || args.unreg) {
        int rc = assoc::RunRegisterCommand(args.unreg, !args.quiet);
        CoUninitialize();
        return rc;
    }

    if (args.panel >= 0) App::Instance().RequestPanel(args.panel, args.capture);
    if (args.lang >= 0) App::Instance().RequestLanguage(args.lang);

    int rc = App::Instance().Run(hInst, args.file, args.bench, args.shot);
    CoUninitialize();
    return rc;
}
