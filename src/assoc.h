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

// 支持关联的扩展名数量
int ExtensionCount();

// 注册为候选看图工具（ProgID + OpenWithProgids + Capabilities + RegisteredApplications）
// 失败时（例如被安全软件拦截）会生成一个 .reg 落到 fallbackRegFile 供用户双击导入。
bool Register(std::wstring* fallbackRegFile = nullptr);

// 一键关联所有图片类型：
// 除 Register() 的内容外，还把 HKCU\Software\Classes\<ext> 的默认值直接指向本程序，
// 这样尚未被其它程序占用的扩展名**立刻**用本程序打开（已被 UserChoice 占用的类型
// 系统不允许程序修改，需要在「默认应用」里点一下）。
// outTypes 返回写完的扩展名个数。
bool RegisterAll(std::wstring* fallbackRegFile, int* outTypes = nullptr);

// 当前有多少个支持的类型实际指向本程序（UserChoice 或 Classes\<ext> 默认值）
int AssociatedCount();

// 反向清理
bool Unregister();

// 是否已注册（且指向当前 exe）
bool IsRegistered();

// 打开 Windows「默认应用」页面，落点为本程序（Win11 支持 registeredAppUser）
bool OpenDefaultAppsSettings();

// 命令行 `--register` 的实现：注册 + 打开设置页；返回进程退出码
int RunRegisterCommand(bool unregisterInstead, bool openSettings = true);

}  // namespace assoc
