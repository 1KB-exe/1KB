#pragma once

#include <cstdint>
#include <string>

enum class PayloadEncryption {
    None,
    Launcher
};

constexpr unsigned PayloadSecretBytes = 8;
constexpr unsigned PayloadNonceBytes = 12;
constexpr unsigned PayloadTagBytes = 16;
constexpr unsigned EncryptedPayloadHeaderBytes = 40;
constexpr unsigned EncryptedPayloadOverhead = EncryptedPayloadHeaderBytes + PayloadTagBytes;

struct PayloadCryptoError {
    std::wstring message;
};

struct EncryptedPayloadInfo {
    unsigned long long plaintextBytes = 0;
    uint8_t nonce[PayloadNonceBytes]{};
};

bool GeneratePayloadSecret(uint8_t secret[PayloadSecretBytes], PayloadCryptoError* error = nullptr);
bool EncryptPayloadZip(const std::wstring& inputZip, const std::wstring& outputLlp,
                       const uint8_t secret[PayloadSecretBytes], const std::wstring& canonicalAppId,
                       const std::wstring& releaseVersion, PayloadCryptoError* error = nullptr);
bool DecryptPayloadZip(const std::wstring& inputLlp, const std::wstring& outputZip,
                       const uint8_t secret[PayloadSecretBytes], const std::wstring& canonicalAppId,
                       const std::wstring& releaseVersion, PayloadCryptoError* error = nullptr);
bool ValidateEncryptedPayloadHeader(const std::wstring& inputLlp, EncryptedPayloadInfo* info,
                                    PayloadCryptoError* error = nullptr);
