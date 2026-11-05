// seesee / 看看 — 文件关联（设为默认看图工具）
// 全部写在 HKCU 下，无需管理员权限。
#pragma once

#include <string>
#include <vector>

namespace assoc {

// 本程序的 ProgID
constexpr const wchar_t* kProgId = L"SeeSee.Image";

// 当前 exe 的完整路径
std::wstring ExePath();

// 支持关联的扩展名（带点）
const std::vector<std::wstring>& Extensions();

// 注册为候选看图工具（ProgID + OpenWithProgids + Capabilities + RegisteredApplications）
// 失败时（例如被安全软件拦截）会生成一个 .reg 落到 fallbackRegFile 供用户双击导入。
bool Register(std::wstring* fallbackRegFile = nullptr);

// 反向清理
bool Unregister();

// 是否已注册（且指向当前 exe）
bool IsRegistered();

// 打开 Windows「默认应用」页面，落点为本程序（Win11 支持 registeredAppUser）
bool OpenDefaultAppsSettings();

// 命令行 `--register` 的实现：注册 + 打开设置页；返回进程退出码
int RunRegisterCommand(bool unregisterInstead, bool openSettings = true);

}  // namespace assoc
