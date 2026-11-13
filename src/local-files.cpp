#include "local-files.hpp"

#include <cerrno>
#include <string>
#include <utility>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <cstdio>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
namespace local {
namespace {

#ifdef _WIN32
using NativeHandle = HANDLE;
const NativeHandle invalidHandle = INVALID_HANDLE_VALUE;
bool closeHandle(NativeHandle handle) { return CloseHandle(handle) != 0; }
#else
using NativeHandle = int;
constexpr NativeHandle invalidHandle = -1;
bool closeHandle(NativeHandle handle) { return ::close(handle) == 0; }
#endif

class FileHandle {
public:
    explicit FileHandle(NativeHandle handle) : handle_(handle) {}
    ~FileHandle() { if (*this) close(); }
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    explicit operator bool() const { return handle_ != invalidHandle; }
    NativeHandle get() const { return handle_; }
    NativeHandle release() { return std::exchange(handle_, invalidHandle); }
    bool close() { return closeHandle(release()); }

private:
    NativeHandle handle_;
};

#ifdef _WIN32
bool plainName(const fs::path& name) {
    const auto& text = name.native();
    return !text.empty() && text.find(L':') == text.npos &&
           text.back() != L'.' && text.back() != L' ';
}

HANDLE openDirectory(const fs::path& path) {
    return CreateFileW(path.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                       FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
}

bool directoryInfo(HANDLE handle, BY_HANDLE_FILE_INFORMATION& info) {
    return GetFileInformationByHandle(handle, &info) &&
           (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
               == FILE_ATTRIBUTE_DIRECTORY;
}

struct CloseSearch {
    void operator()(void* handle) const { FindClose(handle); }
};
#else
constexpr int directoryFlags = O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;

bool plainName(const fs::path& name) {
    return !name.empty() && name == name.filename() && name != "." && name != "..";
}

struct CloseListing {
    void operator()(DIR* directory) const { closedir(directory); }
};
#endif

} // namespace

struct Directory::State {
    explicit State(NativeHandle handle) : file(handle) {}
    FileHandle file;
#ifdef _WIN32
    fs::path path;
    BY_HANDLE_FILE_INFORMATION info{};
#else
    struct stat info{};
#endif
};

Directory::Directory() = default;
Directory::~Directory() = default;
Directory::Directory(Directory&&) noexcept = default;
Directory& Directory::operator=(Directory&&) noexcept = default;
Directory::operator bool() const { return state_ != nullptr; }

std::vector<Directory> Directory::openPath(const fs::path& path) {
    std::error_code error;
    const auto absolute = fs::absolute(path, error);
    if (error || !absolute.is_absolute()) return {};

    const auto rootPath = absolute.root_path();
    Directory root;
#ifdef _WIN32
    const auto drive = absolute.root_name().native();
    if (drive.size() != 2 || drive[1] != L':') return {};
    root.state_ = std::make_unique<State>(openDirectory(rootPath));
    if (!root.state_->file || !directoryInfo(root.state_->file.get(), root.state_->info)) return {};
    root.state_->path = rootPath;
#else
    root.state_ = std::make_unique<State>(::open(rootPath.c_str(), O_PATH | directoryFlags));
    if (!root.state_->file || fstat(root.state_->file.get(), &root.state_->info) != 0) return {};
#endif
    std::vector<Directory> chain;
    chain.push_back(std::move(root));
    for (const auto& part : absolute.relative_path()) {
        if (part.empty() || part == ".") continue;
        if (part == "..") {
            if (chain.size() > 1) chain.pop_back();
            continue;
        }
        auto next = chain.back().child(part);
        if (!next) return {};
        chain.push_back(std::move(next));
    }
    return chain;
}

Directory Directory::child(const fs::path& name) const {
    Directory result;
    if (!plainName(name)) return result;
#ifdef _WIN32
    const auto path = state_->path / name;
    auto opened = std::make_unique<State>(openDirectory(path));
    if (!opened->file || !directoryInfo(opened->file.get(), opened->info)) return result;
    opened->path = path;
#else
    auto opened = std::make_unique<State>(openat(state_->file.get(), name.c_str(), O_PATH | directoryFlags));
    if (!opened->file || fstat(opened->file.get(), &opened->info) != 0) return result;
#endif
    result.state_ = std::move(opened);
    return result;
}

bool Directory::sameAs(const Directory& other) const {
    const auto& left = state_->info;
    const auto& right = other.state_->info;
#ifdef _WIN32
    return left.dwVolumeSerialNumber == right.dwVolumeSerialNumber &&
           left.nFileIndexHigh == right.nFileIndexHigh && left.nFileIndexLow == right.nFileIndexLow;
#else
    return left.st_dev == right.st_dev && left.st_ino == right.st_ino;
#endif
}

Listing Directory::list(std::size_t maxEntries) const {
    Listing result;
    std::size_t visited = 0;
#ifdef _WIN32
    WIN32_FIND_DATAW entry{};
    const auto pattern = state_->path / L"*";
    auto search = FindFirstFileW(pattern.c_str(), &entry);
    if (search == INVALID_HANDLE_VALUE) {
        return {GetLastError() == ERROR_FILE_NOT_FOUND ? Status::ok : Status::ioError, {}};
    }
    std::unique_ptr<void, CloseSearch> searchHandle(search);
    do {
        const std::wstring_view name(entry.cFileName);
        if (name == L"." || name == L"..") continue;
        if (++visited > maxEntries) return {Status::limit, {}};
        if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) && !name.starts_with(L'.')) {
            result.names.emplace_back(name);
        }
    } while (FindNextFileW(searchHandle.get(), &entry));
    if (GetLastError() != ERROR_NO_MORE_FILES) return {Status::ioError, {}};
#else
    FileHandle descriptor(openat(state_->file.get(), ".", O_RDONLY | directoryFlags));
    if (!descriptor) return {Status::ioError, {}};
    std::unique_ptr<DIR, CloseListing> directory(fdopendir(descriptor.get()));
    if (!directory) return {Status::ioError, {}};
    descriptor.release();
    for (;;) {
        errno = 0;
        const auto entry = readdir(directory.get());
        if (!entry) {
            if (errno) return {Status::ioError, {}};
            break;
        }
        const std::string_view name(entry->d_name);
        if (name == "." || name == "..") continue;
        if (++visited > maxEntries) return {Status::limit, {}};
        if (!name.starts_with('.')) result.names.emplace_back(name);
    }
#endif
    return result;
}

Resource Directory::resource(const fs::path& name, bool readHeader, std::uintmax_t maxBytes) const {
    Resource result;
    if (!plainName(name)) return result;
#ifdef _WIN32
    const auto path = state_->path / name;
    const FileHandle file(CreateFileW(path.c_str(), readHeader ? GENERIC_READ : FILE_READ_ATTRIBUTES,
                                     FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                     FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    BY_HANDLE_FILE_INFORMATION info{};
    if (!file || GetFileType(file.get()) != FILE_TYPE_DISK ||
        !GetFileInformationByHandle(file.get(), &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
        return result;
    }
    result.size = (static_cast<std::uintmax_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
#else
    // Размер и заголовок относятся к одному открытому файлу.
    const int access = readHeader ? O_RDONLY | O_NONBLOCK : O_PATH;
    const FileHandle file(openat(state_->file.get(), name.c_str(), access | O_NOFOLLOW | O_CLOEXEC));
    struct stat info{};
    if (!file || fstat(file.get(), &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0) {
        return result;
    }
    result.size = static_cast<std::uintmax_t>(info.st_size);
#endif
    if (result.size > maxBytes) {
        result.status = Status::limit;
        return result;
    }
    result.status = Status::ok;
    if (readHeader && result.size >= result.header.size()) {
#ifdef _WIN32
        DWORD count = 0;
        const auto expected = static_cast<DWORD>(result.header.size());
        if (!ReadFile(file.get(), result.header.data(), expected, &count, nullptr) || count != expected) {
            result.status = Status::ioError;
        }
#else
        ssize_t count;
        do {
            count = ::read(file.get(), result.header.data(), result.header.size());
        } while (count < 0 && errno == EINTR);
        if (count != static_cast<ssize_t>(result.header.size())) result.status = Status::ioError;
#endif
    }
    return result;
}

bool Directory::writeNewReport(const fs::path& name, std::string_view json) const {
    if (!plainName(name)) return false;
    for (int attempt = 0; attempt < 16; ++attempt) {
#ifdef _WIN32
        const auto process = GetCurrentProcessId();
#else
        const auto process = getpid();
#endif
        const fs::path temporary = ".fgmr-report-" + std::to_string(process) + "-" +
                                   std::to_string(attempt) + ".tmp";
        if (temporary == name) continue;
#ifdef _WIN32
        const auto stagedPath = state_->path / temporary;
        const auto finalPath = state_->path / name;
        FileHandle file(CreateFileW(stagedPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!file) {
            if (GetLastError() == ERROR_FILE_EXISTS) continue;
            return false;
        }
        DWORD count = 0;
        const auto length = static_cast<DWORD>(json.size());
        const bool written = WriteFile(file.get(), json.data(), length, &count, nullptr) && count == length;
        const bool closed = file.close();
        if (written && closed && MoveFileW(stagedPath.c_str(), finalPath.c_str())) return true;
        DeleteFileW(stagedPath.c_str());
        return false;
#else
        const int dir = state_->file.get();
        FileHandle file(openat(dir, temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
        if (!file) {
            if (errno == EEXIST) continue;
            return false;
        }
        auto remaining = json;
        while (!remaining.empty()) {
            const auto count = ::write(file.get(), remaining.data(), remaining.size());
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) break;
            remaining.remove_prefix(static_cast<std::size_t>(count));
        }
        const bool closed = file.close();
        if (remaining.empty() && closed) {
#ifdef RENAME_NOREPLACE
            if (renameat2(dir, temporary.c_str(), dir, name.c_str(), RENAME_NOREPLACE) == 0) return true;
            if (errno != ENOSYS && errno != EINVAL && errno != EOPNOTSUPP) {
                unlinkat(dir, temporary.c_str(), 0);
                return false;
            }
#endif
            if (linkat(dir, temporary.c_str(), dir, name.c_str(), 0) == 0) {
                unlinkat(dir, temporary.c_str(), 0);
                return true;
            }
        }
        unlinkat(dir, temporary.c_str(), 0);
        return false;
#endif
    }
    return false;
}

} // namespace local
