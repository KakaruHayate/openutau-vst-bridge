#include "hash.h"

// Header-only build of xxHash: one translation unit needs it, so there is no reason to add
// xxhash.c to the target. Only the (BSD-2-Clause) library is used; xxhsum is GPLv2.
#define XXH_INLINE_ALL
#include <xxhash.h>

#include <limits>

namespace bridge {

uint64_t Xxh64(const void *data, size_t size) {
    return XXH64(data, size, 0);
}

std::string FormatHash(uint64_t hash) {
    return std::to_string(hash);
}

bool TryParseHash(const std::string &text, uint64_t *hash) {
    if (text.empty()) {
        return false;
    }
    constexpr uint64_t kMax = std::numeric_limits<uint64_t>::max();
    uint64_t value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        uint64_t digit = static_cast<uint64_t>(c - '0');
        // Checked before multiplying, so 18446744073709551616 is rejected rather than wrapped.
        if (value > (kMax - digit) / 10) {
            return false;
        }
        value = value * 10 + digit;
    }
    if (hash != nullptr) {
        *hash = value;
    }
    return true;
}

}  // namespace bridge
