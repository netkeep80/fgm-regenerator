#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace fs = std::filesystem;
namespace {
constexpr std::uintmax_t kMaxFile = 16ULL * 1024 * 1024;
constexpr std::size_t kMaxFiles = 10000;
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
std::string upper(std::string text) {
    for (auto &c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return text;
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
fs::path childDir(const fs::path& root, const std::string& target) {
    std::error_code ec;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (upper(it->path().filename().string()) != target) continue;
        if (it->is_symlink(ec) || ec) return {};
        if (it->is_directory(ec) && !ec) return it->path();
        return {};
    }
    return {};
}
void validateFile(Report& report, const std::string& category, const fs::path& path,
                  const std::string& type) {
    Result result;
    result.category = category;
    result.name = path.filename().string();
    result.type = type;
    std::error_code ec;
    if (fs::is_symlink(path, ec) || ec || !fs::is_regular_file(path, ec) || ec) {
        result.diagnosis = "Недопустимый тип файла";
    } else {
        result.bytes = fs::file_size(path, ec);
        if (ec || result.bytes > kMaxFile) {
            result.diagnosis = "Ошибка размера или превышен лимит 16 МиБ";
        } else if (type == "MAP") {
            if (result.bytes < 3) result.diagnosis = "MAP: отсутствует заголовок";
            else {
                std::ifstream stream(path, std::ios::binary);
                std::array<unsigned char, 3> dims{};
                if (!stream.read(reinterpret_cast<char*>(dims.data()), 3))
                    result.diagnosis = "MAP: ошибка чтения заголовка";
                else if (dims[0] == 0 || dims[1] == 0 || dims[2] == 0)
                    result.diagnosis = "MAP: нулевая размерность";
                else {
                    const std::uintmax_t expected = 3 + 4ULL * dims[0] * dims[1] * dims[2];
                    if (expected != result.bytes)
                        result.diagnosis = "MAP: длина не соответствует размерностям";
                    else result.valid = true;
                }
            }
        } else if (type == "MCD") {
            result.valid = result.bytes != 0 && result.bytes % 62 == 0;
            if (!result.valid) result.diagnosis = "MCD: размер не кратен 62 байтам";
        } else if (type == "LOFTEMPS") {
            result.valid = result.bytes != 0 && result.bytes % 32 == 0;
            if (!result.valid) result.diagnosis = "LOFTEMPS: размер не кратен 32 байтам";
        }
    }
    if (result.valid) ++report.passed;
    else ++report.failed;
    report.items.push_back(std::move(result));
}
void scan(Report& report, const fs::path& root) {
    for (const auto& category : {"MAPS", "TERRAIN", "GEODATA"}) {
        fs::path folder = childDir(root, category);
        if (folder.empty()) {
            invalid(report, std::string("Не найден обязательный каталог: ") + category);
            continue;
        }
        std::error_code ec;
        for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
            if (report.items.size() >= kMaxFiles) {
                invalid(report, "Превышен лимит 10000 файлов");
                return;
            }
            const auto path = it->path();
            const auto ext = upper(path.extension().string());
            std::string type;
            if (std::string(category) == "MAPS" && ext == ".MAP") type = "MAP";
            else if (std::string(category) == "TERRAIN" && ext == ".MCD") type = "MCD";
            else if (std::string(category) == "GEODATA" && upper(path.filename().string()) == "LOFTEMPS.DAT") type = "LOFTEMPS";
            if (!type.empty()) validateFile(report, category, path, type);
        }
        if (ec) invalid(report, std::string("Ошибка чтения каталога: ") + category);
    }
    if (report.items.empty()) invalid(report, "Не найдены поддерживаемые игровые ресурсы");
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
} // namespace

int main(int argc, char** argv) {
    if (argc < 4) { usage(); return 64; }
    const std::string mode = argv[1];
    if (mode != "inspect" && mode != "validate") { usage(); return 64; }
    fs::path source, destination;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if ((arg == "--source" || arg == "--report") && i + 1 < argc) {
            fs::path path = argv[++i];
            if (arg == "--source" && source.empty()) source = std::move(path);
            else if (arg == "--report" && destination.empty()) destination = std::move(path);
            else { usage(); return 64; }
        } else { usage(); return 64; }
    }
    std::error_code ec;
    if (source.empty() || fs::is_symlink(source, ec) || ec || !fs::is_directory(source, ec) || ec) {
        std::cerr << "Ошибка: не найден разрешённый исходный каталог\n";
        return 64;
    }
    const auto normalizedSource = fs::weakly_canonical(source, ec);
    if (ec) { std::cerr << "Ошибка: не удалось проверить исходный каталог\n"; return 64; }
    if (!destination.empty()) {
        const auto outPath = fs::weakly_canonical(destination, ec);
        if (ec) { std::cerr << "Ошибка: неверный путь отчёта\n"; return 64; }
        const auto src = normalizedSource.generic_string();
        const auto out = outPath.generic_string();
        if (out == src || (out.size() > src.size() && out.substr(0, src.size()) == src && out[src.size()] == '/')) {
            std::cerr << "Ошибка: отчёт нельзя сохранять внутри исходной игры\n";
            return 64;
        }
        if (fs::is_symlink(destination, ec) && !ec) {
            std::cerr << "Ошибка: отчёт не может быть символической ссылкой\n";
            return 64;
        }
    }
    Report report;
    scan(report, normalizedSource);
    const std::string json = toJson(report, mode);
    if (destination.empty()) std::cout << json;
    else {
        std::ofstream stream(destination, std::ios::binary | std::ios::trunc);
        if (!stream || !(stream << json)) { std::cerr << "Ошибка записи отчёта\n"; return 73; }
    }
    return mode == "validate" && report.failed ? 2 : 0;
}
