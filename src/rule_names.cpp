#include "rule_names.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <map>

namespace {

constexpr std::string_view RULE = "names";

bool is_ident_start(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }

bool is_ident_char(char c) { return is_ident_start(c) || (c >= '0' && c <= '9'); }

bool is_upper(char c) { return c >= 'A' && c <= 'Z'; }

bool is_lower(char c) { return c >= 'a' && c <= 'z'; }

bool has_known_extension(std::string_view name) {
    static constexpr std::array<std::string_view, 33> EXTENSIONS{".c",       ".cc",   ".cpp",  ".cxx",  ".h",     ".hh",  ".hpp", ".hxx",  ".ipp",
                                                                 ".tpp",     ".inl",  ".py",   ".sh",   ".cmake", ".txt", ".md",  ".json", ".yaml",
                                                                 ".yml",     ".xml",  ".yang", ".puml", ".png",   ".svg", ".in",  ".lst",  ".conf",
                                                                 ".service", ".toml", ".ini",  ".cfg",  ".proto", ".cli"};
    auto const dot = name.rfind('.');
    if (dot == std::string_view::npos || dot == 0) {
        return false;
    }
    auto const ext = name.substr(dot);
    // Глоб src/app/server.* тоже считается файлом.
    return ext == ".*" || std::find(EXTENSIONS.begin(), EXTENSIONS.end(), ext) != EXTENSIONS.end();
}

/// Символы, допустимые в пути из README; плейсхолдеры, переменные и пробелы делают путь непроверяемым.
bool is_path_char(char c) {
    return is_ident_char(c) || c == '/' || c == '.' || c == '-' || c == '+' || c == '@' || c == '*' || c == '?' ||
           static_cast<unsigned char>(c) >= 0x80;
}

/// Снимает суффикс ":123" (строка в файле).
std::string_view strip_line_suffix(std::string_view s) {
    auto const colon = s.rfind(':');
    if (colon == std::string_view::npos || colon + 1 == s.size()) {
        return s;
    }
    auto const tail = s.substr(colon + 1);
    return std::all_of(tail.begin(), tail.end(), [](char c) { return c >= '0' && c <= '9'; }) ? s.substr(0, colon) : s;
}

SpanClass classify_path(std::string_view text, Repo const &repo) {
    auto path = strip_line_suffix(text);
    if (path.empty() || path.starts_with('/') || path.starts_with('~') || path.starts_with('-') || path.find("://") != std::string_view::npos ||
        !std::all_of(path.begin(), path.end(), is_path_char)) {
        return {};
    }
    while (path.starts_with("./")) {
        path.remove_prefix(2);
    }
    auto const slash = path.find('/');
    if (slash == std::string_view::npos) {
        if (has_known_extension(path) && path.find_first_of("*?") == std::string_view::npos) {
            return {std::string{path}, SpanKind::FILE_NAME};
        }
        return {};
    }
    auto const last = path.substr(path.rfind('/') + 1);
    if (has_known_extension(last) || repo.has_top_level(path.substr(0, slash))) {
        return {std::string{path}, SpanKind::PATH};
    }
    return {};
}

SpanClass classify_identifier(std::string_view text) {
    auto name = text;
    bool call = false;
    // foo(), foo(args): проверяется имя до скобки.
    if (name.ends_with(')')) {
        auto const open = name.find('(');
        if (open == std::string_view::npos || open == 0) {
            return {};
        }
        name = name.substr(0, open);
        call = true;
    }
    if (name.starts_with("std::") || name.starts_with("::")) {
        return {};
    }
    // Квалифицированное имя A::B::c: каждый компонент идентификатор, проверяется последний.
    std::string_view last;
    std::size_t pos = 0;
    bool qualified = false;
    while (true) {
        auto const sep = name.find("::", pos);
        auto const part = name.substr(pos, sep == std::string_view::npos ? std::string_view::npos : sep - pos);
        if (part.empty() || !is_ident_start(part.front()) || !std::all_of(part.begin(), part.end(), is_ident_char)) {
            return {};
        }
        last = part;
        if (sep == std::string_view::npos) {
            break;
        }
        qualified = true;
        pos = sep + 2;
    }
    bool const underscore = last.find('_') != std::string_view::npos && last.find_first_not_of('_') != std::string_view::npos;
    bool camel = false;
    for (std::size_t i = 1; i < last.size(); ++i) {
        if (is_upper(last[i]) && is_lower(last[i - 1])) {
            camel = true;
            break;
        }
    }
    if (!(underscore || camel || qualified || call)) {
        return {};
    }
    return {std::string{last}, SpanKind::IDENTIFIER};
}

std::string join_dir(std::string const &dir, std::string const &path) { return dir.empty() ? path : dir + "/" + path; }

} // namespace

SpanClass classify_span(std::string_view text, Repo const &repo) {
    if (auto c = classify_path(text, repo); c.kind != SpanKind::NONE) {
        return c;
    }
    return classify_identifier(text);
}

std::set<std::string, std::less<>> load_ignore(std::filesystem::path const &path) {
    std::set<std::string, std::less<>> out;
    std::ifstream in{path};
    std::string line;
    while (std::getline(in, line)) {
        if (auto const hash = line.find('#'); hash != std::string::npos) {
            line.erase(hash);
        }
        auto const b = line.find_first_not_of(" \t\r");
        if (b == std::string::npos) {
            continue;
        }
        out.emplace(line.substr(b, line.find_last_not_of(" \t\r") - b + 1));
    }
    return out;
}

std::vector<Diagnostic> check_names(Markdown const &md, Repo const &repo, SystemHeaders &headers, std::set<std::string, std::less<>> const &ignore,
                                    std::string const &file, std::string const &readme_dir) {
    std::vector<Diagnostic> out;
    std::set<std::string, std::less<>> seen;
    // Идентификаторы, которых нет в проекте: ищутся в системных заголовках одним проходом.
    std::map<std::string, InlineCode const *> missing_idents;

    for (auto const &span : md.spans) {
        auto const cls = classify_span(span.text, repo);
        if (cls.kind == SpanKind::NONE || ignore.contains(span.text) || ignore.contains(cls.name) || !seen.insert(cls.name).second) {
            continue;
        }
        switch (cls.kind) {
            case SpanKind::PATH: {
                bool const glob = cls.name.find_first_of("*?") != std::string::npos;
                bool const found =
                    glob ? repo.matches_glob(cls.name) || repo.matches_glob(join_dir(readme_dir, cls.name)) || repo.matches_glob_suffix(cls.name)
                         : repo.has_path(cls.name) || repo.has_path(join_dir(readme_dir, cls.name)) || repo.has_path_suffix(cls.name) ||
                               headers.has_path(cls.name);
                if (!found) {
                    out.push_back({.file = file,
                                   .rule = std::string{RULE},
                                   .message = fmt::format("путь '{}' не найден в проекте", cls.name),
                                   .line = span.line});
                }
                break;
            }
            case SpanKind::FILE_NAME:
                if (!repo.has_basename(cls.name) && !headers.has_basename(cls.name)) {
                    out.push_back({.file = file,
                                   .rule = std::string{RULE},
                                   .message = fmt::format("файл '{}' не найден ни в проекте, ни в системных заголовках", cls.name),
                                   .line = span.line,
                                   .severity = Severity::WARNING});
                }
                break;
            case SpanKind::IDENTIFIER:
                if (!repo.has_word(cls.name)) {
                    missing_idents.emplace(cls.name, &span);
                }
                break;
            case SpanKind::NONE:
                break;
        }
    }

    std::set<std::string, std::less<>> wanted;
    for (auto const &[name, span] : missing_idents) {
        wanted.insert(name);
    }
    auto const external = headers.find_words(wanted);
    for (auto const &[name, span] : missing_idents) {
        if (external.contains(name)) {
            continue;
        }
        auto const shown = span->text == name ? fmt::format("'{}'", name) : fmt::format("'{}' (из '{}')", name, span->text);
        // ЗАГЛАВНЫЕ имена чаще всего переменные окружения или CMake, определённые вне проекта.
        bool const upper = std::none_of(name.begin(), name.end(), is_lower);
        out.push_back({.file = file,
                       .rule = std::string{RULE},
                       .message = upper ? fmt::format("имя {} не найдено в коде проекта (переменная окружения или CMake?)", shown)
                                        : fmt::format("имя {} не найдено в коде проекта", shown),
                       .line = span->line,
                       .severity = upper ? Severity::WARNING : Severity::ERROR});
    }
    std::stable_sort(out.begin(), out.end(), [](Diagnostic const &a, Diagnostic const &b) { return a.line < b.line; });
    return out;
}
