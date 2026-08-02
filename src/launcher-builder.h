#pragma once

#include <cstdint>
#include <string>
#include "payload-crypto.h"

struct LauncherOverlay {
    std::wstring appId;
    PayloadEncryption encryption = PayloadEncryption::None;
    uint8_t secret[PayloadSecretBytes]{};
};

inline std::wstring CanonicalGithubManifestUrl(const std::wstring& repository, const std::wstring& appKey) {
    return L"https://github.com/" + repository +
           (appKey.empty() ? L"/releases/latest/download/1KB.ini"
                           : L"/releases/download/" + appKey + L"/1KB.ini");
}

bool NormalizeLauncherAppId(const std::wstring& input, std::wstring& appId, bool& github, std::wstring& repository, std::wstring& appKey);
bool ReadDeploymentLauncherOverlay(const std::wstring& path, LauncherOverlay& overlay);
bool IsConsoleApplicationExecutable(const std::wstring& path);
bool GenerateDeploymentLauncher(const std::wstring& application, const std::wstring& output, const std::wstring& appId,
                                bool removeIcon, bool detachConsole, PayloadEncryption encryption,
                                const uint8_t secret[PayloadSecretBytes], std::wstring& error);
