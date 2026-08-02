#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

// Finds the smallest complete PNG among the indexed/grayscale representations,
// palette orders, row filters, and zlib settings enumerated by the implementation.
// Pixels are BGRA. RGB below alpha zero is intentionally insignificant.
bool OptimizeTinyPng(uint32_t width,uint32_t height,const uint8_t* bgra,size_t size,
                     std::vector<uint8_t>& png);
bool TinyPngPixelsEqual(const std::vector<uint8_t>& a,const std::vector<uint8_t>& b);
