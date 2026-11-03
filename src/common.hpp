// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace asset::detail {

inline uint16_t rd16(const uint8_t* p) noexcept { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint32_t rd32(const uint8_t* p) noexcept {
    return uint32_t{p[0]} | (uint32_t{p[1]} << 8) | (uint32_t{p[2]} << 16) | (uint32_t{p[3]} << 24);
}
inline bool tag4(const uint8_t* p, const char* t) noexcept { return std::memcmp(p, t, 4) == 0; }

}  // namespace asset::detail
