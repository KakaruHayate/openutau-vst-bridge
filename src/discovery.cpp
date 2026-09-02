#include "discovery.h"

#include "log.h"
#include "messages.h"

#include <fstream>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cstdlib>
#endif

namespace bridge {
namespace {

/// Matches `Path.GetTempPath()`: on Windows the Win32 temp path (TMP, then TEMP, then the
/// profile), elsewhere `$TMPDIR` if set and `/tmp` otherwise. A sandboxed host can hand its
/// plugins a different temp directory than OpenUtau sees, which would make discovery fail with
/// nothing written wrong — worth remembering if a report ever describes exactly that.
std::filesystem::path TempDirectory() {
#if defined(_WIN32)
    // Wide throughout: a path under a non-ASCII user name would not survive the active code page.
    // The two-call form because a temp path is not bounded by MAX_PATH.
    DWORD needed = ::GetTempPathW(0, nullptr);
    if (needed == 0) {
        BRIDGE_WARN("Could not resolve the temp directory; falling back to the working directory.");
        return std::filesystem::path(L".");
    }
    std::wstring buffer(needed, L'\0');
    DWORD length = ::GetTempPathW(needed, buffer.data());
    if (length == 0 || length >= needed) {
        BRIDGE_WARN("Could not resolve the temp directory; falling back to the working directory.");
        return std::filesystem::path(L".");
    }
    buffer.resize(length);
    return std::filesystem::path(buffer);
#else
    const char *configured = std::getenv("TMPDIR");
    if (configured != nullptr && configured[0] != '\0') {
        return std::filesystem::path(configured);
    }
    return std::filesystem::path("/tmp");
#endif
}

std::string ToUtf8(const std::filesystem::path &path) {
    std::u8string utf8 = path.u8string();
    return std::string(reinterpret_cast<const char *>(utf8.data()), utf8.size());
}

}  // namespace

std::filesystem::path DiscoveryDirectory() {
    return TempDirectory() / "OpenUtau" / "PluginServers";
}

std::string DiscoveryFileStem(int port) {
    return "OpenUtau Bridge " + std::to_string(port);
}

Discovery::~Discovery() {
    Remove();
}

bool Discovery::Publish(int port, const std::string &displayName) {
    if (port <= 0 || port > 65535) {
        BRIDGE_ERROR("Refusing to advertise port %d, which is not a usable port.", port);
        return false;
    }
    Remove();

    std::error_code error;
    std::filesystem::create_directories(directory_, error);
    if (error) {
        BRIDGE_ERROR("Could not create the discovery directory %s: %s",
                     ToUtf8(directory_).c_str(), error.message().c_str());
        return false;
    }

    std::filesystem::path target = directory_ / (DiscoveryFileStem(port) + ".json");
    std::filesystem::path staging = target;
    staging += ".tmp";
    std::string body = BuildDiscoveryJson(port, displayName.empty()
                                                    ? DiscoveryFileStem(port)
                                                    : displayName);
    {
        // No BOM: the other side reads with an explicit UTF-8 encoding, and a BOM is only ever
        // one more thing a JSON parser can trip over.
        std::ofstream out(staging, std::ios::binary | std::ios::trunc);
        if (!out) {
            BRIDGE_ERROR("Could not write %s.", ToUtf8(staging).c_str());
            return false;
        }
        out.write(body.data(), static_cast<std::streamsize>(body.size()));
        if (!out) {
            BRIDGE_ERROR("Could not write %s.", ToUtf8(staging).c_str());
            out.close();
            std::filesystem::remove(staging, error);
            return false;
        }
    }
    // Rename rather than write in place: OpenUtau may scan at any moment, and a half-written
    // advertisement is a warning on its side (§4).
    std::filesystem::rename(staging, target, error);
    if (error) {
        BRIDGE_ERROR("Could not publish %s: %s", ToUtf8(target).c_str(), error.message().c_str());
        std::error_code ignored;
        std::filesystem::remove(staging, ignored);
        return false;
    }
    path_ = target;
    BRIDGE_INFO("Advertised port %d as %s.", port, ToUtf8(path_).c_str());
    return true;
}

void Discovery::Remove() {
    if (path_.empty()) {
        return;
    }
    std::error_code error;
    std::filesystem::remove(path_, error);
    if (error) {
        // Not fatal: OpenUtau probes the port and deletes what no longer answers (§4).
        BRIDGE_WARN("Could not delete %s: %s", ToUtf8(path_).c_str(), error.message().c_str());
    }
    path_.clear();
}

std::string Discovery::PathUtf8() const {
    return path_.empty() ? std::string() : ToUtf8(path_);
}

}  // namespace bridge
