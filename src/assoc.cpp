// seesee / 看看 — 文件关联（设为默认看图工具）
// 全部写在 HKCU 下，不需要管理员权限。
// 若注册表写入被安全软件/沙箱拒绝，则退化为生成一个 .reg 文件供用户双击导入。
#include "assoc.h"

#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <winreg.h>

#include <iterator>
#include <vector>

namespace assoc {
namespace {

const std::wstring kClasses = L"Software\\Classes\\";
const std::wstring kCaps = L"Software\\SeeSee\\Capabilities";

struct Entry {
    std::wstring key;    // HKEY_CURRENT_USER 下的相对路径
    std::wstring name;   // 值名，空 = 默认值
    std::wstring value;  // 值数据；空 = 空值（如 OpenWithProgids）
};

bool MakeKey(const std::wstring& sub, HKEY* out) {
    return RegCreateKeyExW(HKEY_CURRENT_USER, sub.c_str(), 0, nullptr, 0, KEY_WRITE | KEY_SET_VALUE,
                           nullptr, out, nullptr) == ERROR_SUCCESS;
}

bool Apply(const Entry& e) {
    HKEY key = nullptr;
    if (!MakeKey(e.key, &key)) return false;
    const wchar_t* name = e.name.empty() ? nullptr : e.name.c_str();
    LONG r;
    if (e.value.empty()) {
        r = RegSetValueExW(key, name, 0, REG_NONE, nullptr, 0);
    } else {
        r = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(e.value.c_str()),
                           static_cast<DWORD>((e.value.size() + 1) * sizeof(wchar_t)));
    }
    RegCloseKey(key);
    return r == ERROR_SUCCESS;
}

std::wstring Escape(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size() + 8);
    for (wchar_t c : s) {
        if (c == L'\\' || c == L'"') out.push_back(L'\\');
        out.push_back(c);
    }
    return out;
}

bool WriteRegFile(const std::vector<Entry>& entries, const std::wstring& path) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return false;
    const wchar_t bom = 0xFEFF;
    fwrite(&bom, sizeof(bom), 1, f);
    const std::wstring header = L"Windows Registry Editor Version 5.00\r\n\r\n";
    fwrite(header.c_str(), sizeof(wchar_t), header.size(), f);

    std::wstring lastKey;
    for (const auto& e : entries) {
        if (e.key != lastKey) {
            lastKey = e.key;
            std::wstring line = L"\r\n[HKEY_CURRENT_USER\\" + e.key + L"]\r\n";
            fwrite(line.c_str(), sizeof(wchar_t), line.size(), f);
        }
        std::wstring line = e.name.empty() ? L"@" : (L"\"" + Escape(e.name) + L"\"");
        line += L"=\"" + Escape(e.value) + L"\"\r\n";
        fwrite(line.c_str(), sizeof(wchar_t), line.size(), f);
    }
    fclose(f);
    return true;
}

std::vector<Entry> BuildEntries() {
    const std::wstring exe = ExePath();
    const std::wstring icon = exe + L",0";
    const std::wstring command = L"\"" + exe + L"\" \"%1\"";
    const std::wstring progId = kClasses + kProgId;

    std::vector<Entry> v;
    v.push_back({progId, L"", L"图片文件 (看看 SeeSee)"});
    v.push_back({progId + L"\\DefaultIcon", L"", icon});
    v.push_back({progId + L"\\shell\\open", L"FriendlyAppName", L"看看 SeeSee"});
    v.push_back({progId + L"\\shell\\open\\command", L"", command});
    // 「打开方式」列表里的身份
    v.push_back({progId + L"\\Application", L"ApplicationName", L"SeeSee"});
    v.push_back({progId + L"\\Application", L"ApplicationIcon", icon});
    v.push_back({progId + L"\\Application", L"ApplicationDescription",
                 L"极速看图 — 毫秒级打开，键盘翻页"});

    for (const auto& ext : Extensions())
        v.push_back({kClasses + ext + L"\\OpenWithProgids", kProgId, L""});

    // 「默认应用」页面登记
    v.push_back({kCaps, L"ApplicationName", L"看看 SeeSee"});
    v.push_back({kCaps, L"ApplicationDescription",
                 L"极速看图 — 毫秒级打开，键盘翻页，自动加载同目录图片"});
    v.push_back({kCaps, L"ApplicationIcon", icon});
    for (const auto& ext : Extensions())
        v.push_back({kCaps + L"\\FileAssociations", ext, kProgId});
    v.push_back({L"Software\\RegisteredApplications", L"SeeSee", kCaps});
    return v;
}

}  // namespace

std::wstring ExePath() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    if (n == 0) return L"";
    return std::wstring(buf, n);
}

const std::vector<std::wstring>& Extensions() {
    static const std::vector<std::wstring> exts = {
        L".jpg", L".jpeg", L".jpe", L".jfif",  L".png",  L".bmp",  L".gif", L".tif",
        L".tiff", L".ico", L".webp", L".avif", L".heic", L".heif", L".jxl",
        // 单反 / 微单 RAW
        L".cr2", L".cr3", L".crw", L".nef", L".nrw", L".arw", L".srf", L".sr2",
        L".dng", L".orf", L".rw2", L".raf", L".pef", L".srw", L".rwl", L".3fr",
        L".fff", L".iiq", L".mos", L".mrw", L".x3f", L".dcr", L".kdc", L".mef",
        L".raw", L".erf", L".rwz", L".gpr", L".braw",
    };
    return exts;
}

bool Register(std::wstring* fallbackRegFile) {
    if (fallbackRegFile) fallbackRegFile->clear();
    if (ExePath().empty()) return false;

    const std::vector<Entry> entries = BuildEntries();
    bool ok = true;
    for (const auto& e : entries) ok &= Apply(e);

    if (ok) {
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
        return true;
    }

    // 退路：生成 .reg 文件让用户双击导入
    std::vector<std::wstring> candidates;
    wchar_t dir[MAX_PATH];
    HRESULT shr = SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, dir);
    if (SUCCEEDED(shr)) {
        std::wstring folder = std::wstring(dir) + L"\\SeeSee";
        CreateDirectoryW(folder.c_str(), nullptr);
        candidates.push_back(folder + L"\\seesee-assoc.reg");
    }
    wchar_t tmp[MAX_PATH];
    DWORD tn = GetTempPathW(MAX_PATH, tmp);
    if (tn > 0 && tn < MAX_PATH) candidates.push_back(std::wstring(tmp) + L"seesee-assoc.reg");
    {
        const std::wstring exe = ExePath();
        size_t slash = exe.find_last_of(L'\\');
        if (slash != std::wstring::npos) candidates.push_back(exe.substr(0, slash) + L"\\seesee-assoc.reg");
    }

    for (const auto& path : candidates) {
        bool wrote = WriteRegFile(entries, path);
        if (wrote) {
            if (fallbackRegFile) *fallbackRegFile = path;
            break;
        }
    }
    return false;
}

bool Unregister() {
    bool ok = true;
    ok &= RegDeleteTreeW(HKEY_CURRENT_USER, (kClasses + kProgId).c_str()) == ERROR_SUCCESS;
    for (const auto& ext : Extensions()) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, (kClasses + ext + L"\\OpenWithProgids").c_str(), 0,
                          KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
            RegDeleteValueW(key, kProgId);
            RegCloseKey(key);
        }
    }
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\SeeSee");
    HKEY ra = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\RegisteredApplications", 0, KEY_SET_VALUE, &ra) ==
        ERROR_SUCCESS) {
        RegDeleteValueW(ra, L"SeeSee");
        RegCloseKey(ra);
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return ok;
}

bool IsRegistered() {
    const std::wstring command =
        [] {
            HKEY key = nullptr;
            std::wstring out;
            const std::wstring p = kClasses + kProgId + L"\\shell\\open\\command";
            if (RegOpenKeyExW(HKEY_CURRENT_USER, p.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS)
                return out;
            wchar_t buf[2048];
            DWORD cb = sizeof(buf) - sizeof(wchar_t);
            DWORD type = 0;
            if (RegQueryValueExW(key, nullptr, nullptr, &type, reinterpret_cast<BYTE*>(buf), &cb) ==
                    ERROR_SUCCESS &&
                type == REG_SZ) {
                buf[cb / sizeof(wchar_t)] = 0;
                out = buf;
            }
            RegCloseKey(key);
            return out;
        }();
    return command.find(ExePath()) != std::wstring::npos;
}

bool OpenDefaultAppsSettings() {
    // Win11：落到本程序的默认应用页面；Win10 只支持列表页
    const wchar_t* uris[] = {L"ms-settings:defaultapps?registeredAppUser=SeeSee",
                             L"ms-settings:defaultapps"};
    for (const wchar_t* uri : uris) {
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", uri, nullptr, nullptr,
                                                    SW_SHOWNORMAL)) > 32)
            return true;
    }
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", L"control.exe",
                                                   L"/name Microsoft.DefaultPrograms", nullptr,
                                                   SW_SHOWNORMAL)) > 32;
}

int RunRegisterCommand(bool unregisterInstead, bool openSettings) {
    if (unregisterInstead) return Unregister() ? 0 : 1;

    std::wstring regFile;
    bool ok = Register(&regFile);
    if (ok && openSettings) OpenDefaultAppsSettings();

    // 有 console（被 cmd 调用）时也能看到结果
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f = nullptr;
        if (_wfopen_s(&f, L"CONOUT$", L"w") == 0 && f) {
            fwprintf(f, ok ? L"[seesee] 已注册为候选看图工具。\n"
                           : L"[seesee] 注册表写入被拒绝，已生成: %s\n",
                     regFile.c_str());
            fclose(f);
        }
        FreeConsole();
    }
    return ok ? 0 : 1;
}

}  // namespace assoc
