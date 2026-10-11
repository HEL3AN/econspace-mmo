#pragma once

#include <chrono>
#include <filesystem>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>

// A folder of the test's own under the system temp directory, removed with everything in it
// when it goes out of scope -- also when a REQUIRE has thrown out of the test half way. A test
// that writes files writes them here: not into the working directory, where they were left
// beside whatever ran the tests, and never into data/, where the game would read them.
class TempDir
{
public:
    TempDir()
    {
        namespace fs = std::filesystem;
        // Unique per test and per run, so two runs at once never share a file. The clock
        // is mixed in because random_device has been deterministic on some MinGW builds.
        std::random_device rd;
        const auto         stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const fs::path     base = fs::temp_directory_path();
        for (int attempt = 0; attempt < 100; attempt++)
        {
            const fs::path  p = base / ("econspace_test_" + std::to_string(stamp) + "_" +
                                        std::to_string(rd()) + "_" + std::to_string(attempt));
            std::error_code ec;
            if (fs::create_directory(p, ec))
            {
                path_ = p;
                return;
            }
        }
        throw std::runtime_error("TempDir: cannot create a folder under " + base.string());
    }

    ~TempDir()
    {
        std::error_code ec;  // never throws: this runs while an assertion unwinds the test
        std::filesystem::remove_all(path_, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    // A file (or "sub/file") inside the folder. Forward slashes, as the loaders are given.
    std::string Path(const std::string& name) const { return (path_ / name).generic_string(); }

    // A folder inside it, made now.
    std::string MakeDir(const std::string& name) const
    {
        std::filesystem::create_directories(path_ / name);
        return Path(name);
    }

private:
    std::filesystem::path path_;
};
