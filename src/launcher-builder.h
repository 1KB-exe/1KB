#pragma once

#include <cstdint>
#include <string>
#include "payload-crypto.h"

struct LauncherOverlay {
    std::wstring appId;
    PayloadEncryption encryption = PayloadEncryption::None;
    uint8_t secret[PayloadSecretBytes]{};
};

bool NormalizeLauncherAppId(const std::wstring& input, std::wstring& appId, bool& github, std::wstring& repository, std::wstring& appKey);
bool ReadDeploymentLauncherOverlay(const std::wstring& path, LauncherOverlay& overlay);
bool GenerateDeploymentLauncher(const std::wstring& application, const std::wstring& output, const std::wstring& appId,
                                bool removeIcon, PayloadEncryption encryption,
                                const uint8_t secret[PayloadSecretBytes], std::wstring& error);
