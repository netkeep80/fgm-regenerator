#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

namespace local {

enum class Status { ok, invalidType, ioError, limit };

struct Listing {
    Status status = Status::ok;
    std::vector<std::filesystem::path> names;
};

struct Resource {
    Status status = Status::invalidType;
    std::uintmax_t size = 0;
    std::array<unsigned char, 3> header{};
};

class Directory {
public:
    Directory();
    ~Directory();
    Directory(Directory&&) noexcept;
    Directory& operator=(Directory&&) noexcept;

    static std::vector<Directory> openPath(const std::filesystem::path& path);
    explicit operator bool() const;
    Directory child(const std::filesystem::path& name) const;
    bool sameAs(const Directory& other) const;
    Listing list(std::size_t maxEntries) const;
    Resource resource(const std::filesystem::path& name, bool readHeader,
                      std::uintmax_t maxBytes) const;
    bool writeNewReport(const std::filesystem::path& name, std::string_view json) const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace local
