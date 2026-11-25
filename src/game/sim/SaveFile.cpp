#include "sim/SaveFile.h"

#include <cstdio>
#include <filesystem>
#include <system_error>

// The platform headers stay in this file: windows.h and raylib.h both define Rectangle,
// CloseWindow and ShowCursor, so nothing that includes raylib may include it.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace
{

// Replaces `to` with `from` in one step. On POSIX rename() is that by definition; on Windows
// rename() refuses an existing target, and MoveFileEx with REPLACE_EXISTING is the call that
// does the same thing as POSIX.
bool ReplaceFile(const std::string& from, const std::string& to)
{
#ifdef _WIN32
    const std::wstring f = std::filesystem::path(from).wstring();
    const std::wstring t = std::filesystem::path(to).wstring();
    return MoveFileExW(f.c_str(), t.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) !=
           0;
#else
    return std::rename(from.c_str(), to.c_str()) == 0;
#endif
}

// Writes the whole file and makes sure it reached the disk before anything is renamed over
// the real one: a rename that lands before the data would be the same truncated file again,
// one step later.
bool WriteAndSync(const std::string& path, const std::string& contents)
{
#ifdef _WIN32
    FILE* f = _wfopen(std::filesystem::path(path).wstring().c_str(), L"wb");
#else
    FILE* f = std::fopen(path.c_str(), "wb");
#endif
    if (f == nullptr)
        return false;
    bool ok = std::fwrite(contents.data(), 1, contents.size(), f) == contents.size();
    ok = ok && std::fflush(f) == 0;
#ifdef _WIN32
    ok = ok && _commit(_fileno(f)) == 0;
#else
    ok = ok && fsync(fileno(f)) == 0;
#endif
    ok = (std::fclose(f) == 0) && ok;
    return ok;
}

}  // namespace

namespace Save
{

std::string BackupPath(const std::string& path, int n)
{
    return path + ".bak" + std::to_string(n);
}

bool WriteFileAtomic(const std::string& path, const std::string& contents, int backups)
{
    namespace fs = std::filesystem;
    const std::string tmp = path + ".tmp";
    if (!WriteAndSync(tmp, contents))
    {
        std::error_code ec;
        fs::remove(tmp, ec);
        return false;
    }

    // The copies shift down before the current file joins them. A failure here costs a
    // backup, not the save, so it does not stop the write.
    std::error_code ec;
    if (backups > 0 && fs::exists(path, ec))
    {
        for (int n = backups; n > 1; n--)
            if (fs::exists(BackupPath(path, n - 1), ec))
                ReplaceFile(BackupPath(path, n - 1), BackupPath(path, n));
        fs::copy_file(path, BackupPath(path, 1), fs::copy_options::overwrite_existing, ec);
    }

    if (!ReplaceFile(tmp, path))
    {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

BackgroundWriter::BackgroundWriter() : worker_([this] { Run(); })
{
}

BackgroundWriter::~BackgroundWriter()
{
    {
        std::lock_guard<std::mutex> lock(m_);
        stop_ = true;
    }
    wake_.notify_all();
    worker_.join();  // Run() leaves only when nothing is waiting
}

void BackgroundWriter::Submit(const std::string& path, std::function<std::string()> text,
                              int backups)
{
    {
        std::lock_guard<std::mutex> lock(m_);
        auto                        it = waiting_.find(path);
        if (it != waiting_.end())
        {
            coalesced_++;
            it->second = Job{ std::move(text), backups };
        }
        else
            waiting_.emplace(path, Job{ std::move(text), backups });
    }
    wake_.notify_one();
}

void BackgroundWriter::Wait()
{
    std::unique_lock<std::mutex> lock(m_);
    idle_.wait(lock, [this] { return waiting_.empty() && !busy_; });
}

int BackgroundWriter::TakeFailures(std::string* lastPath)
{
    std::lock_guard<std::mutex> lock(m_);
    const int                   n = failures_;
    if (lastPath != nullptr)
        *lastPath = failedPath_;
    failures_ = 0;
    return n;
}

int BackgroundWriter::Coalesced() const
{
    std::lock_guard<std::mutex> lock(m_);
    return coalesced_;
}

int BackgroundWriter::Written() const
{
    std::lock_guard<std::mutex> lock(m_);
    return written_;
}

void BackgroundWriter::Run()
{
    std::unique_lock<std::mutex> lock(m_);
    for (;;)
    {
        wake_.wait(lock, [this] { return stop_ || !waiting_.empty(); });
        if (waiting_.empty())
            return;  // stopped, and nothing left to write
        auto        first = waiting_.begin();
        std::string path = first->first;
        Job         job = std::move(first->second);
        waiting_.erase(first);
        busy_ = true;
        lock.unlock();
        bool ok = false;
        try
        {
            ok = WriteFileAtomic(path, job.text(), job.backups);
        }
        catch (...)
        {
            ok = false;  // a job that throws is a failed save, not a dead thread
        }
        lock.lock();
        busy_ = false;
        if (ok)
            written_++;
        else
        {
            failures_++;
            failedPath_ = path;
        }
        idle_.notify_all();
    }
}

}  // namespace Save
