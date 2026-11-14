#include "local-files.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
namespace {
constexpr std::uintmax_t kMaxFileBytes = 16ULL * 1024 * 1024;
constexpr std::size_t kMaxDirectoryEntries = 10000;
struct Result {
    std::string category, name, type, diagnosis;
    std::uintmax_t bytes = 0;
    bool valid = false;
};
struct Report {
    std::vector<Result> items;
    std::vector<std::string> diagnostics;
    std::size_t passed = 0;
    std::size_t failed = 0;
};
bool sameFormatName(const fs::path& path, std::string_view expected) {
    const auto& name = path.native();
    return name.size() == expected.size() &&
           std::equal(name.begin(), name.end(), expected.begin(), [](auto actual, char wanted) {
               if (actual >= 'a' && actual <= 'z') actual -= 'a' - 'A';
               return actual == wanted;
           });
}

bool validUtf8(std::string_view text) {
    while (!text.empty()) {
        const auto lead = static_cast<unsigned char>(text.front());
        text.remove_prefix(1);
        if (lead < 0x80) continue;
        if (lead < 0xc2 || lead > 0xf4) return false;
        const std::size_t tailSize = lead < 0xe0 ? 1 : (lead < 0xf0 ? 2 : 3);
        if (text.size() < tailSize) return false;
        const auto first = static_cast<unsigned char>(text.front());
        if ((lead == 0xe0 && first < 0xa0) || (lead == 0xed && first > 0x9f) ||
            (lead == 0xf0 && first < 0x90) || (lead == 0xf4 && first > 0x8f)) return false;
        for (const unsigned char c : text.substr(0, tailSize)) {
            if (c < 0x80 || c > 0xbf) return false;
        }
        text.remove_prefix(tailSize);
    }
    return true;
}
std::string jsonString(std::string_view src) {
    std::ostringstream out;
    out << '"';
    for (unsigned char c : src) {
        if (c == '"' || c == '\\') out << '\\' << static_cast<char>(c);
        else if (c == '\n') out << "\\n";
        else if (c == '\r') out << "\\r";
        else if (c == '\t') out << "\\t";
        else if (c < 32) {
            constexpr char hex[] = "0123456789abcdef";
            out << "\\u00" << hex[c >> 4] << hex[c & 15];
        } else out << static_cast<char>(c);
    }
    out << '"';
    return out.str();
}
void invalid(Report& report, const std::string& diagnostic) {
    ++report.failed;
    report.diagnostics.push_back(diagnostic);
}
void validateFile(Report& report, const local::Directory& folder, std::string_view category,
                  const fs::path& name, std::string_view type) {
    Result result;
    result.category = category;
    result.type = type;
    try {
        const auto utf8 = name.u8string();
        result.name.assign(utf8.begin(), utf8.end());
    } catch (const std::system_error&) {
        invalid(report, "Имя ресурса нельзя представить в UTF-8");
        return;
    }
    if (!validUtf8(result.name)) {
        invalid(report, "Имя ресурса нельзя представить в UTF-8");
        return;
    }
    const auto file = folder.resource(name, type == "MAP", kMaxFileBytes);
    result.bytes = file.size;
    if (file.status == local::Status::invalidType) {
        result.diagnosis = "Недопустимый тип файла или ошибка открытия";
    } else if (file.status == local::Status::limit) {
        result.diagnosis = "Превышен лимит 16 МиБ";
    } else if (file.status == local::Status::ioError) {
        result.diagnosis = "MAP: ошибка чтения заголовка";
    } else if (type == "MAP") {
        const auto& dims = file.header;
        if (result.bytes < dims.size()) {
            result.diagnosis = "MAP: отсутствует заголовок";
        } else if (dims[0] == 0 || dims[1] == 0 || dims[2] == 0) {
            result.diagnosis = "MAP: нулевая размерность";
        } else {
            const std::uintmax_t expected = 3 + 4ULL * dims[0] * dims[1] * dims[2];
            result.valid = expected == result.bytes;
            if (!result.valid) result.diagnosis = "MAP: длина не соответствует размерностям";
        }
    } else {
        const auto recordBytes = type == "MCD" ? 62u : 32u;
        result.valid = result.bytes != 0 && result.bytes % recordBytes == 0;
        if (!result.valid) {
            result.diagnosis = result.type + ": размер должен быть положительным и кратным " +
                               std::to_string(recordBytes) + " байтам";
        }
    }
    if (result.valid) ++report.passed;
    else ++report.failed;
    report.items.push_back(std::move(result));
}
bool checkListing(Report& report, const local::Listing& listing, std::string_view category) {
    if (listing.status == local::Status::ok) return true;
    const auto message = listing.status == local::Status::limit ?
        "Превышен лимит 10000 записей каталога: " : "Ошибка чтения каталога: ";
    invalid(report, message + std::string(category));
    return false;
}

void scan(Report& report, const local::Directory& root) {
    const auto rootEntries = root.list(kMaxDirectoryEntries);
    if (!checkListing(report, rootEntries, "исходная игра")) return;

    struct Format {
        std::string_view category, type, selector;
        bool exactFilename;
    };
    constexpr std::array formats{
        Format{"MAPS", "MAP", ".MAP", false},
        Format{"TERRAIN", "MCD", ".MCD", false},
        Format{"GEODATA", "LOFTEMPS", "LOFTEMPS.DAT", true},
    };
    for (const auto& format : formats) {
        const std::string category(format.category);
        std::vector<fs::path> folders;
        for (const auto& name : rootEntries.names) {
            if (sameFormatName(name, format.category)) folders.push_back(name);
        }
        if (folders.size() != 1) {
            invalid(report, (folders.empty() ? "Не найден обязательный каталог: " :
                    "Неоднозначное имя обязательного каталога: ") + category);
            continue;
        }
        auto folder = root.child(folders.front());
        if (!folder) {
            invalid(report, "Недопустимый или недоступный каталог: " + category);
            continue;
        }
        const auto entries = folder.list(kMaxDirectoryEntries);
        if (!checkListing(report, entries, category)) continue;
        std::vector<fs::path> names;
        for (const auto& name : entries.names) {
            if (sameFormatName(format.exactFilename ? name : name.extension(), format.selector)) {
                names.push_back(name);
            }
        }
        if (names.empty()) {
            invalid(report, "Не найдены поддерживаемые файлы: " + category);
            continue;
        }
        if (format.exactFilename && names.size() != 1) {
            invalid(report, "Неоднозначное имя ресурса LOFTEMPS.DAT");
            continue;
        }
        std::sort(names.begin(), names.end());
        for (const auto& name : names) {
            validateFile(report, folder, format.category, name, format.type);
        }
    }
    std::sort(report.items.begin(), report.items.end(), [](const Result& a, const Result& b) {
        return std::tie(a.category, a.name) < std::tie(b.category, b.name);
    });
}
std::string toJson(const Report& report, std::string_view mode) {
    std::ostringstream out;
    out << "{\n  \"schema\": \"fgm-regenerator/inspection-v1\",\n"
        << "  \"mode\": " << jsonString(mode) << ",\n"
        << "  \"checks\": {\"files\": " << report.items.size()
        << ", \"passed\": " << report.passed << ", \"failed\": " << report.failed << "},\n"
        << "  \"resources\": [\n";
    for (std::size_t i = 0; i < report.items.size(); ++i) {
        const auto& r = report.items[i];
        if (i) out << ",\n";
        out << "    {\"category\": " << jsonString(r.category)
            << ", \"name\": " << jsonString(r.name)
            << ", \"type\": " << jsonString(r.type)
            << ", \"bytes\": " << r.bytes
            << ", \"valid\": " << (r.valid ? "true" : "false")
            << ", \"diagnosis\": " << jsonString(r.diagnosis) << "}";
    }
    out << "\n  ],\n  \"diagnostics\": [";
    for (std::size_t i = 0; i < report.diagnostics.size(); ++i) {
        if (i) out << ", ";
        out << jsonString(report.diagnostics[i]);
    }
    out << "]\n}\n";
    return out.str();
}
void usage() {
    std::cerr << "Использование: fgm-regenerator inspect|validate --source КАТАЛОГ [--report JSON_ФАЙЛ]\n";
}
template <typename Char>
int run(int argc, Char** argv) {
    if (argc < 4) {
        usage();
        return 64;
    }
    const fs::path command(argv[1]);
    if (command != "inspect" && command != "validate") {
        usage();
        return 64;
    }
    const std::string_view mode = command == "inspect" ? "inspect" : "validate";
    fs::path source, destination;
    for (int i = 2; i < argc; ++i) {
        const fs::path option(argv[i]);
        if (i + 1 == argc || argv[i + 1][0] == 0) {
            usage();
            return 64;
        }
        if (option == "--source" && source.empty()) {
            source = argv[++i];
        } else if (option == "--report" && destination.empty()) {
            destination = argv[++i];
        } else {
            usage();
            return 64;
        }
    }

    auto sourceChain = source.empty() ? std::vector<local::Directory>{} : local::Directory::openPath(source);
    if (sourceChain.empty()) {
        std::cerr << "Ошибка: не найден разрешённый исходный каталог\n";
        return 64;
    }
    const auto& root = sourceChain.back();
    std::vector<local::Directory> reportChain;
    if (!destination.empty()) {
        auto parent = destination.parent_path();
        if (parent.empty()) parent = ".";
        reportChain = local::Directory::openPath(parent);
        if (reportChain.empty()) {
            std::cerr << "Ошибка: неверный или недоступный каталог отчёта\n";
            return 64;
        }
        if (std::any_of(reportChain.begin(), reportChain.end(), [&root](const auto& dir) {
                return root.sameAs(dir);
            })) {
            std::cerr << "Ошибка: отчёт нельзя сохранять внутри исходной игры\n";
            return 64;
        }
    }

    Report report;
    scan(report, root);
    const std::string json = toJson(report, mode);
    if (destination.empty()) {
        std::cout << json << std::flush;
        if (!std::cout) {
            std::cerr << "Ошибка вывода отчёта\n";
            return 73;
        }
    } else if (!reportChain.back().writeNewReport(destination.filename(), json)) {
        std::cerr << "Ошибка создания нового файла отчёта: проверьте имя, права и отсутствие файла\n";
        return 73;
    }
    return mode == "validate" && report.failed ? 2 : 0;
}
} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
    try {
        return run(argc, argv);
    } catch (const fs::filesystem_error&) {
        std::cerr << "Ошибка обработки локального пути\n";
        return 64;
    }
}
