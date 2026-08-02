#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Inserts one compact RT_ICON/RT_GROUP_ICON tree into 1KB.exe's altered
// Crinkler tiny-header core. The source must be the builder's conventional
// fixed i386 template after icon serialization.
bool PackCrinklerIconPe(const std::vector<uint8_t>& source,
                        const std::vector<uint8_t>& crinklerCore,
                        std::vector<uint8_t>& packed,
                        std::wstring* error=nullptr);
