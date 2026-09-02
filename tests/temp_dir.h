#pragma once

/*
 * A scratch directory per test. The real discovery path is deliberately left alone: a running
 * OpenUtau scans it and would try to connect to whatever a test advertised there.
 */

#include "clock.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace bridge {
namespace test {

struct TempDir {
    std::filesystem::path path;

    TempDir() {
        static int counter = 0;
        std::error_code ignored;
        path = std::filesystem::temp_directory_path() /
               ("openutau-bridge-test-" + std::to_string(NowMs()) + "-" +
                std::to_string(++counter));
        std::filesystem::create_directories(path, ignored);
    }

    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }

    TempDir(const TempDir &) = delete;
    TempDir &operator=(const TempDir &) = delete;
};

inline std::vector<std::filesystem::path> FilesIn(const std::filesystem::path &directory) {
    std::vector<std::filesystem::path> found;
    std::error_code ignored;
    for (const std::filesystem::directory_entry &entry :
         std::filesystem::directory_iterator(directory, ignored)) {
        found.push_back(entry.path());
    }
    return found;
}

inline std::string ReadAll(const std::filesystem::path &file) {
    std::ifstream in(file, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

}  // namespace test
}  // namespace bridge
