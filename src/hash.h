#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace bridge {

/// The protocol version this build speaks, carried by the discovery file and echoed in the
/// init response (PROTOCOL.md §4).
inline constexpr const char *kApiVersion = "1.0";

/// Hard engine limits of OpenUtau's mixer; never negotiated (PROTOCOL.md §1).
inline constexpr int kWireSampleRate = 44100;
inline constexpr int kWireChannels = 2;

/// XXH64 with seed 0 of a payload — the same digest `DawAudio.Hash` produces.
uint64_t Xxh64(const void *data, size_t size);

/// Hashes travel as decimal strings everywhere: a 64-bit value exceeds the 2^53
/// safe-integer range of JSON number parsers (PROTOCOL.md §5.2).
std::string FormatHash(uint64_t hash);

/// Strict decimal parse, mirroring `DawAudio.TryParseHash` (`NumberStyles.None`): digits
/// only, no sign, no whitespace, no overflow past 2^64-1.
bool TryParseHash(const std::string &text, uint64_t *hash);

}  // namespace bridge
