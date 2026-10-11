#include "rule_deps.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <set>

namespace fs = std::filesystem;

namespace {

constexpr std::string_view RULE = "deps";

std::string_view trim(std::string_view s) {
    auto const b = s.find_first_not_of(" \t\r");
    if (b == std::string_view::npos) {
        return {};
    }
    return s.substr(b, s.find_last_not_of(" \t\r") - b + 1);
}

std::vector<std::string_view> split_ws(std::string_view s) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) {
            ++i;
        }
        auto const b = i;
        while (i < s.size() && s[i] != ' ' && s[i] != '\t') {
            ++i;
        }
        if (i > b) {
            out.push_back(s.substr(b, i - b));
        }
    }
    return out;
}

/// Имя пакета Debian: строчные буквы, цифры, + - . (политика Debian 5.6.1).
bool valid_name(std::string_view n) {
    return n.size() >= 2 &&
           std::all_of(n.begin(), n.end(),
                       [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.'; }) &&
           ((n[0] >= 'a' && n[0] <= 'z') || (n[0] >= '0' && n[0] <= '9'));
}

/// Разбирает имя[:арх][=версия]; пустое имя, если токен не пакет.
/// Версия Debian или шаблон версии apt (3.72.*): цифры, буквы и . + ~ : - *.
bool valid_version(std::string_view v) {
    return !v.empty() && std::all_of(v.begin(), v.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '+' || c == '~' || c == ':' ||
               c == '-' || c == '*';
    });
}

Package parse_package(std::string_view token) {
    if (token.empty() || token.front() == '-') {
        return {};
    }
    std::string_view version;
    if (auto const eq = token.find('='); eq != std::string_view::npos) {
        version = token.substr(eq + 1);
        token = token.substr(0, eq);
        if (!valid_version(version)) {
            return {};
        }
    }
    if (auto const colon = token.find(':'); colon != std::string_view::npos) {
        token = token.substr(0, colon);
    }
    if (!valid_name(token)) {
        return {};
    }
    return {std::string{token}, std::string{version}};
}

bool is_terminator(std::string_view t) {
    static constexpr std::array<std::string_view, 6> TERMINATORS{"&&", "||", ";", "|", "&", "2>&1"};
    return std::find(TERMINATORS.begin(), TERMINATORS.end(), t) != TERMINATORS.end() || t.starts_with('#') || t.starts_with('<') ||
           t.starts_with('>') || t.starts_with("2>");
}

/// Ищет команды установки в одной логической строке.
void scan_logical_line(std::string_view line, std::size_t line_no, std::vector<AptCommand> &out) {
    auto const tokens = split_ws(line);
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        auto const t = tokens[i];
        // Остаток строки после # комментарий оболочки.
        if (t.starts_with('#')) {
            return;
        }
        if (t != "apt" && t != "apt-get" && t != "aptitude") {
            continue;
        }
        auto j = i + 1;
        while (j < tokens.size() && tokens[j].starts_with('-')) {
            ++j;
        }
        if (j >= tokens.size() || tokens[j] != "install") {
            continue;
        }
        AptCommand cmd{{}, line_no};
        for (++j; j < tokens.size(); ++j) {
            auto tok = tokens[j];
            bool stop = is_terminator(tok);
            if (!stop && tok.ends_with(';')) {
                tok.remove_suffix(1);
                stop = true;
            }
            if (is_terminator(tok)) {
                break;
            }
            if (auto p = parse_package(tok); !p.name.empty()) {
                cmd.packages.push_back(std::move(p));
            }
            if (stop) {
                break;
            }
        }
        if (!cmd.packages.empty()) {
            out.push_back(std::move(cmd));
        }
        // Токен-разделитель обработает внешний цикл: после # строка заканчивается.
        i = j - 1;
    }
}

std::string read_file(fs::path const &path) {
    std::ifstream in{path, std::ios::binary};
    std::string data;
    std::array<char, 65536> buf{};
    while (in.read(buf.data(), buf.size()) || in.gcount() > 0) {
        data.append(buf.data(), static_cast<std::size_t>(in.gcount()));
    }
    return data;
}

/// Язык блока, в котором ищутся команды оболочки.
bool is_shell_block(std::string_view lang) {
    static constexpr std::array<std::string_view, 7> LANGS{"", "bash", "sh", "shell", "console", "zsh", "shell-session"};
    return std::find(LANGS.begin(), LANGS.end(), lang) != LANGS.end();
}

std::string join_files(std::vector<DepSource const *> const &sources) {
    std::string out;
    for (auto const *s : sources) {
        out += (out.empty() ? "" : ", ") + s->file;
    }
    return out;
}

} // namespace

std::vector<AptCommand> find_apt_commands(std::string_view text, std::size_t first_line) {
    std::vector<AptCommand> out;
    std::string logical;
    std::size_t logical_line = 0;
    std::size_t line_no = first_line;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        auto eol = text.find('\n', pos);
        if (eol == std::string_view::npos) {
            eol = text.size();
        }
        auto line = text.substr(pos, eol - pos);
        if (line.ends_with('\r')) {
            line.remove_suffix(1);
        }
        if (logical.empty()) {
            logical_line = line_no;
        }
        auto const t = trim(line);
        if (t.ends_with('\\')) {
            logical.append(t.substr(0, t.size() - 1)).push_back(' ');
        } else {
            logical.append(t);
            scan_logical_line(logical, logical_line, out);
            logical.clear();
        }
        ++line_no;
        if (eol == text.size()) {
            break;
        }
        pos = eol + 1;
    }
    if (!logical.empty()) {
        scan_logical_line(logical, logical_line, out);
    }
    return out;
}

DepSource parse_build_deps_lst(std::string_view text, std::string file) {
    DepSource src{std::move(file), {}, true};
    std::size_t pos = 0;
    while (pos < text.size()) {
        auto eol = text.find('\n', pos);
        if (eol == std::string_view::npos) {
            eol = text.size();
        }
        auto line = text.substr(pos, eol - pos);
        pos = eol + 1;
        if (auto const hash = line.find('#'); hash != std::string_view::npos) {
            line = line.substr(0, hash);
        }
        if (line.find('$') != std::string_view::npos) {
            continue;
        }
        for (auto const tok : split_ws(trim(line))) {
            if (auto p = parse_package(tok); !p.name.empty()) {
                src.packages.emplace(std::move(p.name), std::move(p.version));
            }
        }
    }
    return src;
}

DepSource parse_debian_control(std::string_view text, std::string file) {
    DepSource src{std::move(file), {}, true};
    std::string value;
    bool in_field = false;
    auto flush = [&] {
        std::size_t start = 0;
        while (start <= value.size()) {
            auto end = value.find(',', start);
            if (end == std::string::npos) {
                end = value.size();
            }
            auto const item = std::string_view{value}.substr(start, end - start);
            start = end + 1;
            std::size_t alt_start = 0;
            while (alt_start <= item.size()) {
                auto alt_end = item.find('|', alt_start);
                if (alt_end == std::string_view::npos) {
                    alt_end = item.size();
                }
                auto const alt = trim(item.substr(alt_start, alt_end - alt_start));
                alt_start = alt_end + 1;
                if (alt.empty() || alt.starts_with("${")) {
                    continue;
                }
                auto const name_end = alt.find_first_of(" \t([<:");
                auto const name = alt.substr(0, name_end);
                if (!valid_name(name)) {
                    continue;
                }
                std::string version;
                if (auto const open = alt.find('('); open != std::string_view::npos) {
                    auto const close = alt.find(')', open);
                    auto const rel = trim(alt.substr(open + 1, close == std::string_view::npos ? std::string_view::npos : close - open - 1));
                    if (rel.starts_with('=')) {
                        version = trim(rel.substr(1));
                    }
                }
                src.packages.emplace(std::string{name}, std::move(version));
            }
            if (end == value.size()) {
                break;
            }
        }
        value.clear();
    };
    std::size_t pos = 0;
    while (pos < text.size()) {
        auto eol = text.find('\n', pos);
        if (eol == std::string_view::npos) {
            eol = text.size();
        }
        auto const line = text.substr(pos, eol - pos);
        pos = eol + 1;
        if (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
            if (in_field) {
                value.append(" ").append(trim(line));
            }
            continue;
        }
        if (in_field) {
            flush();
            in_field = false;
        }
        auto const colon = line.find(':');
        if (colon == std::string_view::npos) {
            continue;
        }
        std::string field{line.substr(0, colon)};
        std::transform(field.begin(), field.end(), field.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (field == "build-depends" || field == "build-depends-indep" || field == "build-depends-arch") {
            in_field = true;
            value = trim(line.substr(colon + 1));
        }
    }
    if (in_field) {
        flush();
    }
    return src;
}

DepSource parse_ci_file(std::string_view text, std::string file) {
    DepSource src{std::move(file), {}, false};
    for (auto const &cmd : find_apt_commands(text, 1)) {
        for (auto const &p : cmd.packages) {
            src.packages.emplace(p.name, p.version);
        }
    }
    return src;
}

std::vector<DepSource> load_dep_sources(fs::path const &root) {
    std::vector<DepSource> out;
    std::error_code ec;
    if (fs::is_regular_file(root / "build-deps.lst", ec)) {
        out.push_back(parse_build_deps_lst(read_file(root / "build-deps.lst"), "build-deps.lst"));
    }
    if (fs::is_regular_file(root / "debian/control", ec)) {
        if (auto src = parse_debian_control(read_file(root / "debian/control"), "debian/control"); !src.packages.empty()) {
            out.push_back(std::move(src));
        }
    }
    std::vector<fs::path> ci;
    for (fs::directory_iterator it{root / ".github/workflows", ec}, end; !ec && it != end; it.increment(ec)) {
        auto const ext = it->path().extension();
        if (ext == ".yml" || ext == ".yaml") {
            ci.push_back(it->path());
        }
    }
    std::sort(ci.begin(), ci.end());
    for (auto const &p : ci) {
        if (auto src = parse_ci_file(read_file(p), p.lexically_relative(root).generic_string()); !src.packages.empty()) {
            out.push_back(std::move(src));
        }
    }
    return out;
}

std::vector<Diagnostic> check_deps(Markdown const &md, std::vector<DepSource> const &sources, std::string const &file) {
    std::vector<Diagnostic> out;
    if (sources.empty()) {
        return out;
    }
    std::vector<AptCommand> commands;
    for (auto const &b : md.blocks) {
        if (is_shell_block(b.lang)) {
            auto c = find_apt_commands(b.text, b.first_line);
            commands.insert(commands.end(), c.begin(), c.end());
        }
    }
    for (auto const &s : md.spans) {
        auto c = find_apt_commands(s.text, s.line);
        commands.insert(commands.end(), c.begin(), c.end());
    }

    std::vector<DepSource const *> all;
    for (auto const &s : sources) {
        all.push_back(&s);
    }
    for (auto const &cmd : commands) {
        bool const build = std::any_of(cmd.packages.begin(), cmd.packages.end(), [&](Package const &p) {
            return std::any_of(sources.begin(), sources.end(), [&](DepSource const &s) { return s.packages.contains(p.name); });
        });
        if (!build) {
            continue;
        }
        std::set<std::string> in_cmd;
        for (auto const &p : cmd.packages) {
            in_cmd.insert(p.name);
            bool known = false;
            for (auto const &s : sources) {
                auto const it = s.packages.find(p.name);
                if (it == s.packages.end()) {
                    continue;
                }
                known = true;
                if (!p.version.empty() && !it->second.empty() && p.version != it->second) {
                    out.push_back({.file = file,
                                   .rule = std::string{RULE},
                                   .message = fmt::format("версия пакета '{}': в README '={}', в {} '={}'", p.name, p.version, s.file, it->second),
                                   .line = cmd.line});
                }
            }
            if (!known) {
                out.push_back({.file = file,
                               .rule = std::string{RULE},
                               .message = fmt::format("пакет '{}' не найден в {}", p.name, join_files(all)),
                               .line = cmd.line,
                               .severity = Severity::WARNING});
            }
        }
        for (auto const &s : sources) {
            if (!s.complete || s.packages.empty()) {
                continue;
            }
            auto const covered = static_cast<std::size_t>(
                std::count_if(s.packages.begin(), s.packages.end(), [&](auto const &kv) { return in_cmd.contains(kv.first); }));
            if (covered * 2 < s.packages.size()) {
                continue;
            }
            for (auto const &[name, version] : s.packages) {
                if (!in_cmd.contains(name)) {
                    out.push_back({.file = file,
                                   .rule = std::string{RULE},
                                   .message = fmt::format("пакет '{}' из {} не указан в команде установки", name, s.file),
                                   .line = cmd.line,
                                   .severity = Severity::WARNING});
                }
            }
        }
    }
    return out;
}
