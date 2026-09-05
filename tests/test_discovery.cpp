#include "discovery.h"

#include "temp_dir.h"

#include <doctest.h>

#include <filesystem>
#include <string>
#include <vector>

using bridge::Discovery;
using bridge::DiscoveryDirectory;
using bridge::DiscoveryFileStem;
using bridge::test::FilesIn;
using bridge::test::ReadAll;
using bridge::test::TempDir;

namespace fs = std::filesystem;

TEST_CASE("The discovery directory is the one OpenUtau scans") {
    fs::path directory = DiscoveryDirectory();
    CHECK(directory.is_absolute());
    CHECK(directory.filename() == "PluginServers");
    CHECK(directory.parent_path().filename() == "OpenUtau");
}

TEST_CASE("An advertisement carries the port, the name and the api version") {
    TempDir temp;
    Discovery discovery(temp.path);
    REQUIRE(discovery.Publish(52341, "OpenUtau Bridge (Track 1)"));
    CHECK(discovery.IsPublished());

    std::vector<fs::path> files = FilesIn(temp.path);
    REQUIRE(files.size() == 1);  // Nothing left staged.
    CHECK(files[0].extension() == ".json");
    // The port is in the file name so two instances cannot overwrite each other.
    CHECK(files[0].stem().string() == DiscoveryFileStem(52341));

    std::string body = ReadAll(files[0]);
    CHECK(body.find("\"port\":52341") != std::string::npos);
    CHECK(body.find("\"name\":\"OpenUtau Bridge (Track 1)\"") != std::string::npos);
    CHECK(body.find("\"apiVersion\":\"1.2\"") != std::string::npos);
    // A BOM would be one more thing for a JSON reader to trip over.
    CHECK(body.substr(0, 1) == "{");
}

TEST_CASE("A rebind replaces the advertisement instead of leaving two") {
    TempDir temp;
    Discovery discovery(temp.path);
    REQUIRE(discovery.Publish(50001, "OpenUtau Bridge"));
    REQUIRE(discovery.Publish(50002, "OpenUtau Bridge"));

    std::vector<fs::path> files = FilesIn(temp.path);
    REQUIRE(files.size() == 1);
    CHECK(files[0].stem().string() == DiscoveryFileStem(50002));
}

TEST_CASE("The advertisement goes away with the instance") {
    TempDir temp;
    {
        Discovery discovery(temp.path);
        REQUIRE(discovery.Publish(50003, "OpenUtau Bridge"));
        REQUIRE(FilesIn(temp.path).size() == 1);
    }
    // §4: a file whose instance is gone would be probed and deleted by OpenUtau, but leaving
    // it there means the next scan offers a connection that cannot be made.
    CHECK(FilesIn(temp.path).empty());
}

TEST_CASE("Removing is idempotent") {
    TempDir temp;
    Discovery discovery(temp.path);
    REQUIRE(discovery.Publish(50004, "OpenUtau Bridge"));
    discovery.Remove();
    CHECK(!discovery.IsPublished());
    CHECK(discovery.PathUtf8().empty());
    discovery.Remove();
    CHECK(FilesIn(temp.path).empty());
}

TEST_CASE("A port that cannot be bound is not advertised") {
    TempDir temp;
    Discovery discovery(temp.path);
    CHECK(!discovery.Publish(0, "OpenUtau Bridge"));
    CHECK(!discovery.Publish(-1, "OpenUtau Bridge"));
    CHECK(!discovery.Publish(65536, "OpenUtau Bridge"));
    CHECK(!discovery.IsPublished());
    CHECK(FilesIn(temp.path).empty());
}

TEST_CASE("A blank name falls back to something identifiable") {
    // OpenUtau shows the file stem when the name field is blank; sending a usable name means
    // the two never disagree.
    TempDir temp;
    Discovery discovery(temp.path);
    REQUIRE(discovery.Publish(50005, ""));
    std::string body = ReadAll(FilesIn(temp.path)[0]);
    CHECK(body.find("\"name\":\"" + DiscoveryFileStem(50005) + "\"") != std::string::npos);
}

TEST_CASE("A missing directory is created, not an error") {
    TempDir temp;
    fs::path nested = temp.path / "OpenUtau" / "PluginServers";
    Discovery discovery(nested);
    REQUIRE(discovery.Publish(50006, "OpenUtau Bridge"));
    CHECK(fs::exists(nested));
    CHECK(FilesIn(nested).size() == 1);
}
