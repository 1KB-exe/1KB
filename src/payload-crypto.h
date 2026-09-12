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

using PayloadBytes = bool (*)(void* context, const uint8_t* bytes, unsigned count);
using PayloadProducer = bool (*)(void* context, PayloadBytes write, void* writeContext);

bool GeneratePayloadSecret(uint8_t secret[PayloadSecretBytes], PayloadCryptoError* error = nullptr);
bool EncryptPayloadStream(const std::wstring& output, unsigned long long plaintextBytes,
                          const uint8_t secret[PayloadSecretBytes], const std::wstring& canonicalAppId,
                          const std::wstring& packageName, PayloadProducer produce, void* context,
                          PayloadCryptoError* error = nullptr);

struct PayloadDecryptStream {
    void* state = nullptr;
};
bool BeginPayloadDecrypt(PayloadDecryptStream& stream, const uint8_t secret[PayloadSecretBytes],
                         const std::wstring& canonicalAppId, const std::wstring& packageName,
                         PayloadBytes output, void* outputContext, PayloadCryptoError* error = nullptr);
bool DecryptPayloadBytes(PayloadDecryptStream& stream, const uint8_t* bytes, unsigned count);
bool FinishPayloadDecrypt(PayloadDecryptStream& stream);
void EndPayloadDecrypt(PayloadDecryptStream& stream);
