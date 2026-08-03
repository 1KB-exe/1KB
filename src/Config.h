#pragma once
#include <windows.h>

// Authoritative recovery/update endpoint. The build generates the matching
// MASM definition used by the bootstrap.
constexpr wchar_t LauncherRuntimeDownloadUrl[] = L"https://r.2v2.me";

namespace Limits {
constexpr DWORD UpdateConfigurationTimeoutMs = 5000;
constexpr DWORD RuntimeUpdateCheckIntervalMs = 60 * 1000;
constexpr DWORD DownloadTimeoutMs = 30000;
constexpr DWORD GracefulRestartTimeoutMs = 5000;
constexpr DWORD ForcedRestartTimeoutMs = 2000;
constexpr unsigned long long MaxUpdateConfigurationSize = 4ull * 1024ull;
constexpr unsigned long long MaxAppIdBytes = 4ull * 1024ull;
constexpr unsigned long long MaxApplicationSize = 100ull * 1024ull * 1024ull;
}
