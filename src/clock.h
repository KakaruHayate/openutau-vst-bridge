#pragma once

#include <chrono>
#include <cstdint>

namespace bridge {

/// Monotonic milliseconds. Used for every protocol timing, so a wall-clock adjustment
/// cannot make a connection look dead or a pull look stalled.
inline int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace bridge
