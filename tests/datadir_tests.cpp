#include <doctest/doctest.h>

#include "core/DataDir.h"
#include "temp_dir.h"

#include <cstdlib>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

// Where every binary reads data/ from (#399, #486). Only the parts that do not depend on where
// the test binary lives: --data and ECONSPACE_DATA come before the build tree and the folder
// beside the executable, so with either one set the answer is the test's own folder.

namespace
{
// A folder that counts as data/: it has a factions.json in it.
std::string DataFolder(const TempDir& t)
{
    const std::string dir = t.MakeDir("data");
    std::ofstream(dir + "/factions.json") << "{}";
    return dir;
}

// argv as main gets it: writable strings that outlive the call.
struct Args
{
    std::vector<std::string> text;
    std::vector<char*>       argv;
    int                      argc = 0;

    explicit Args(std::vector<std::string> words) : text(std::move(words))
    {
        for (std::string& s : text)
            argv.push_back(s.data());
        argv.push_back(nullptr);
        argc = (int)text.size();
    }
    std::string At(int i) const { return argv[i]; }
};

void SetEnv(const char* name, const char* value)
{
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value)
        setenv(name, value, 1);
    else
        unsetenv(name);
#endif
}

// ECONSPACE_DATA as the test wants it, and DataDir::Path() as it was, put back afterwards.
class Restore
{
public:
    Restore() : path_(DataDir::Path())
    {
        if (const char* v = std::getenv("ECONSPACE_DATA"))
            env_ = v;
    }
    ~Restore()
    {
        SetEnv("ECONSPACE_DATA", env_ ? env_->c_str() : nullptr);
        Args back({ "tests", "--data", path_ });
        DataDir::Resolve(back.argc, back.argv.data(), "tests");
    }

private:
    std::string                path_;
    std::optional<std::string> env_;
};
}  // namespace

TEST_CASE("--data names the folder and is taken out of argv")
{
    Restore           restore;
    TempDir           t;
    const std::string dir = DataFolder(t);
    SetEnv("ECONSPACE_DATA", nullptr);

    Args a({ "econserver", "host", "--data", dir, "50800" });
    CHECK(DataDir::Resolve(a.argc, a.argv.data(), "tests") == dir + "/");
    CHECK(a.argc == 3);
    CHECK(a.At(1) == "host");
    CHECK(a.At(2) == "50800");
    CHECK(DataDir::Path() == dir + "/");

    // A trailing slash given is not doubled.
    Args b({ "econserver", "--data", dir + "/" });
    CHECK(DataDir::Resolve(b.argc, b.argv.data(), "tests") == dir + "/");
    CHECK(b.argc == 1);
}

TEST_CASE("--data with no folder after it cuts argv there and goes on to the next source")
{
    Restore           restore;
    TempDir           t;
    const std::string dir = DataFolder(t);
    SetEnv("ECONSPACE_DATA", dir.c_str());

    Args a({ "econserver", "host", "--data" });
    CHECK(DataDir::Resolve(a.argc, a.argv.data(), "tests") == dir + "/");
    CHECK(a.argc == 2);
    CHECK(a.At(1) == "host");
}

TEST_CASE("--data wins over ECONSPACE_DATA, which wins over everything else")
{
    Restore           restore;
    TempDir           fromEnv, fromArg;
    const std::string envDir = DataFolder(fromEnv);
    const std::string argDir = DataFolder(fromArg);
    SetEnv("ECONSPACE_DATA", envDir.c_str());

    Args a({ "econspace", "--data", argDir, "connect" });
    CHECK(DataDir::Resolve(a.argc, a.argv.data(), "tests") == argDir + "/");
    CHECK(DataDir::Path() == argDir + "/");

    // No --data: the variable, and argv is left alone.
    Args b({ "econspace", "connect", "127.0.0.1" });
    CHECK(DataDir::Resolve(b.argc, b.argv.data(), "tests") == envDir + "/");
    CHECK(b.argc == 3);
    CHECK(b.At(2) == "127.0.0.1");
    CHECK(DataDir::Path() == envDir + "/");
}

TEST_CASE("a folder named on purpose is used even without a factions.json in it")
{
    // The binary says so on stderr and then fails on the first file it reads -- a clearer
    // failure than quietly reading another data/ than the one asked for.
    Restore restore;
    TempDir empty;
    SetEnv("ECONSPACE_DATA", nullptr);
    const std::string dir = empty.MakeDir("data");

    Args a({ "econserver", "--data", dir });
    CHECK(DataDir::Resolve(a.argc, a.argv.data(), "tests") == dir + "/");
    CHECK(DataDir::Path() == dir + "/");
}
