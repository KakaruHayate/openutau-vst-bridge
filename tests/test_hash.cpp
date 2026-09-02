#include <doctest.h>

#include "hash.h"

#include <cstdint>
#include <string>

using bridge::FormatHash;
using bridge::TryParseHash;
using bridge::Xxh64;

TEST_CASE("XXH64 matches the reference vector for empty input") {
    // Pins this build to standard XXH64 with seed 0. The other side hashes with
    // K4os.Hash.xxHash; if either ever stops being the reference algorithm, dedup silently
    // breaks and every part looks missing forever, so it is worth a constant.
    CHECK(Xxh64("", 0) == 0xEF46DB3751D8E999ULL);
}

TEST_CASE("The shared PCM vector hashes to the value the other side expects") {
    // Three frames of interleaved little-endian float32 — 1.0, -1.0, 0.5 — which is what
    // DawAudio.ToPcmBytes produces for the same samples. OpenUtau.Test asserts this constant
    // against K4os.Hash.xxHash, so the two suites together pin the whole data plane: the byte
    // layout as much as the digest. A divergence here is the failure that cannot be seen from
    // one repository alone, because both sides would still agree with themselves.
    const uint8_t pcm[] = {0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x80, 0xBF, 0x00, 0x00, 0x00, 0x3F};
    CHECK(Xxh64(pcm, sizeof(pcm)) == 12033956788804169010ULL);
}

TEST_CASE("XXH64 is sensitive to content and length") {
    const std::string a = "OpenUtau";
    const std::string b = "OpenUtav";
    CHECK(Xxh64(a.data(), a.size()) != Xxh64(b.data(), b.size()));
    CHECK(Xxh64(a.data(), a.size()) != Xxh64(a.data(), a.size() - 1));
    CHECK(Xxh64(a.data(), a.size()) == Xxh64(a.data(), a.size()));
}

TEST_CASE("hashes round-trip through their decimal wire form") {
    for (uint64_t value : {uint64_t{0}, uint64_t{1}, uint64_t{13507256038857166760ULL},
                           uint64_t{0xFFFFFFFFFFFFFFFFULL}}) {
        uint64_t parsed = 0;
        REQUIRE(TryParseHash(FormatHash(value), &parsed));
        CHECK(parsed == value);
    }
}

TEST_CASE("the documented example hash is representable") {
    // PROTOCOL.md §5.2 uses this value precisely because it is past 2^53: a receiver that
    // routed it through a JSON number would come back with something else.
    uint64_t parsed = 0;
    REQUIRE(TryParseHash("13507256038857166760", &parsed));
    CHECK(parsed == 13507256038857166760ULL);
    CHECK(parsed > (uint64_t{1} << 53));
}

TEST_CASE("hash parsing is as strict as NumberStyles.None") {
    uint64_t parsed = 0;
    CHECK_FALSE(TryParseHash("", &parsed));
    CHECK_FALSE(TryParseHash("-1", &parsed));
    CHECK_FALSE(TryParseHash("+1", &parsed));
    CHECK_FALSE(TryParseHash(" 1", &parsed));
    CHECK_FALSE(TryParseHash("1 ", &parsed));
    CHECK_FALSE(TryParseHash("1.0", &parsed));
    CHECK_FALSE(TryParseHash("0x10", &parsed));
    CHECK_FALSE(TryParseHash("1e3", &parsed));
    // One past 2^64-1.
    CHECK_FALSE(TryParseHash("18446744073709551616", &parsed));
    CHECK(TryParseHash("18446744073709551615", &parsed));
}
