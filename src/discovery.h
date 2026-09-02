#pragma once

/*
 * The discovery file of PROTOCOL.md §4: one JSON advertisement per plugin instance under
 * `<temp>/OpenUtau/PluginServers`, written once the port is bound and deleted on shutdown.
 *
 * OpenUtau treats a file whose port it can bind itself as stale and deletes it, so the order
 * matters: bind first, publish second. Nothing here is called from the audio thread.
 */

#include <filesystem>
#include <string>

namespace bridge {

/// `<temp>/OpenUtau/PluginServers`, where `<temp>` is resolved the way .NET's
/// `Path.GetTempPath()` resolves it on this OS — the two sides have to agree on the directory
/// or discovery silently finds nothing.
std::filesystem::path DiscoveryDirectory();

/// The file stem. The port is what makes it unique: two live instances cannot share one, and a
/// display name can repeat across tracks.
std::string DiscoveryFileStem(int port);

class Discovery {
public:
    /// The directory is injectable so tests never write into the real discovery path, where a
    /// running OpenUtau would try to connect to them.
    explicit Discovery(std::filesystem::path directory = DiscoveryDirectory())
        : directory_(std::move(directory)) {}

    /// Deletes the advertisement: an instance that is gone must not be found.
    ~Discovery();

    Discovery(const Discovery &) = delete;
    Discovery &operator=(const Discovery &) = delete;

    /// Advertises a freshly bound port, replacing any file this instance published before.
    /// False (and logged) if the port is out of range or the file could not be written.
    bool Publish(int port, const std::string &displayName);

    void Remove();

    bool IsPublished() const { return !path_.empty(); }

    /// UTF-8, for logs and tests. Empty when nothing is published.
    std::string PathUtf8() const;

private:
    std::filesystem::path directory_;
    std::filesystem::path path_;
};

}  // namespace bridge
