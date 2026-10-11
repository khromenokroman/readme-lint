#include "rule_options.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <fstream>

#include "cparse.hpp"

namespace fs = std::filesystem;

namespace {

constexpr std::string_view RULE = "options";
/// Меньше описанных ключей: README не описывает CLI, о неописанных ключах не сообщается.
constexpr std::size_t MIN_DOCUMENTED = 2;

bool is_alnum(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); }

bool is_ident_char(char c) { return is_alnum(c) || c == '_'; }

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

std::string read_file(fs::path const &path) {
    std::ifstream in{path, std::ios::binary};
    std::string data;
    std::array<char, 65536> buf{};
    while (in.read(buf.data(), buf.size()) || in.gcount() > 0) {
        data.append(buf.data(), static_cast<std::size_t>(in.gcount()));
    }
    return data;
}

/// Ключ: -x (буква, цифра) или --name (буквы, цифры, '-', '_').
bool is_option(std::string_view s) {
    if (s.size() == 2 && s[0] == '-' && is_alnum(s[1])) {
        return true;
    }
    return s.size() > 3 && s.starts_with("--") && ((s[2] >= 'a' && s[2] <= 'z') || (s[2] >= '0' && s[2] <= '9')) &&
           std::all_of(s.begin() + 2, s.end(), [](char c) { return is_alnum(c) || c == '-' || c == '_'; });
}

/// Снимает значение ("--file=x" -> "--file") и знаки препинания по краям.
std::string_view clean_token(std::string_view t) {
    while (!t.empty() && (t.front() == '(' || t.front() == '[' || t.front() == '"' || t.front() == '\'')) {
        t.remove_prefix(1);
    }
    while (!t.empty() && (t.back() == ',' || t.back() == '.' || t.back() == ':' || t.back() == ';' || t.back() == ')' || t.back() == ']' ||
                          t.back() == '"' || t.back() == '\'')) {
        t.remove_suffix(1);
    }
    if (auto const eq = t.find('='); eq != std::string_view::npos) {
        t = t.substr(0, eq);
    }
    return t;
}

/// Плейсхолдер значения ключа: ФАЙЛ, SIZE, <file>, [N], _size_, {a,b}, а также "|" и "," между ключами.
bool is_placeholder(std::string_view t) {
    if (t.empty() || t == "|" || t == "," || t == "или" || t == "or") {
        return true;
    }
    if ((t.front() == '<' && t.back() == '>') || (t.front() == '[' && t.back() == ']') || (t.front() == '{' && t.back() == '}') ||
        (t.size() > 2 && t.front() == '_' && t.back() == '_')) {
        return true;
    }
    // Все буквы заглавные (латиница или кириллица в UTF-8: заглавные А-Я это D0 90..D0 AF и Ё D0 81).
    bool letter = false;
    for (std::size_t i = 0; i < t.size(); ++i) {
        auto const c = static_cast<unsigned char>(t[i]);
        if (c >= 'a' && c <= 'z') {
            return false;
        }
        if (c >= 'A' && c <= 'Z') {
            letter = true;
        } else if (c == 0xD0 && i + 1 < t.size()) {
            auto const n = static_cast<unsigned char>(t[++i]);
            if (!((n >= 0x90 && n <= 0xAF) || n == 0x81)) {
                return false;
            }
            letter = true;
        } else if (c >= 0x80) {
            return false;
        } else if (!(c == '_' || c == '-' || c == '.' || (c >= '0' && c <= '9'))) {
            return false;
        }
    }
    return letter;
}

std::vector<std::string_view> split_ws(std::string_view s) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && is_space(s[i])) {
            ++i;
        }
        auto const b = i;
        while (i < s.size() && !is_space(s[i])) {
            ++i;
        }
        if (i > b) {
            out.push_back(s.substr(b, i - b));
        }
    }
    return out;
}

/// Убирает разметку markdown, которая окружает ключи: обратные кавычки и звёздочки.
std::string strip_markup(std::string_view s) {
    std::string out;
    for (auto c : s) {
        if (c != '`' && c != '*') {
            out.push_back(c);
        }
    }
    return out;
}

/// Ключи в начале фрагмента: ключи и плейсхолдеры до первого обычного слова.
std::vector<std::string> leading_options(std::string_view text) {
    std::vector<std::string> out;
    for (auto tok : split_ws(text)) {
        auto const c = clean_token(tok);
        if (is_option(c)) {
            out.emplace_back(c);
            continue;
        }
        // "-f," и "-f|--file" уже разобраны clean_token; составной "-f|--file" делится здесь.
        if (tok.find('|') != std::string_view::npos && tok.starts_with('-')) {
            bool all = true;
            std::vector<std::string> parts;
            std::size_t start = 0;
            while (start <= tok.size()) {
                auto end = tok.find('|', start);
                if (end == std::string_view::npos) {
                    end = tok.size();
                }
                auto const p = clean_token(tok.substr(start, end - start));
                all = all && is_option(p);
                parts.emplace_back(p);
                start = end + 1;
            }
            if (all) {
                out.insert(out.end(), parts.begin(), parts.end());
                continue;
            }
        }
        if (is_placeholder(clean_token(tok)) || is_placeholder(tok)) {
            continue;
        }
        break;
    }
    return out;
}

/// Строка кода похожа на разбор или регистрацию ключей.
bool declares_options(std::string_view line) {
    static constexpr std::array<std::string_view, 9> MARKERS{"==",           "strcmp",   "streq",        "add_option", "add_flag",
                                                             "add_argument", "compare(", "starts_with(", "case "};
    return std::any_of(MARKERS.begin(), MARKERS.end(), [&](std::string_view m) { return line.find(m) != std::string_view::npos; });
}

void add_pair(CliOptions &out, std::string const &a, std::string const &b) {
    out.pairs[a] = b;
    out.pairs[b] = a;
}

/// Позиции вызовов функции name( с границей идентификатора слева.
std::vector<std::size_t> find_calls(std::string_view code, std::string_view name) {
    std::vector<std::size_t> out;
    for (auto i = code.find(name); i != std::string_view::npos; i = code.find(name, i + 1)) {
        auto j = i + name.size();
        if ((i > 0 && is_ident_char(code[i - 1])) || j >= code.size()) {
            continue;
        }
        while (j < code.size() && (code[j] == ' ' || code[j] == '\t')) {
            ++j;
        }
        if (j < code.size() && code[j] == '(') {
            out.push_back(j);
        }
    }
    return out;
}

void parse_getopt(std::string_view code, CliOptions &out) {
    for (auto const name : {std::string_view{"getopt"}, std::string_view{"getopt_long"}, std::string_view{"getopt_long_only"}}) {
        for (auto const open : find_calls(code, name)) {
            auto const args = split_args(bracket_content(code, open));
            if (args.size() < 3) {
                continue;
            }
            auto const lits = string_literals(args[2]);
            if (lits.empty()) {
                continue;
            }
            std::string_view spec = lits.front();
            while (!spec.empty() && (spec.front() == '+' || spec.front() == '-' || spec.front() == ':')) {
                spec.remove_prefix(1);
            }
            for (auto const c : spec) {
                if (c != ':' && is_alnum(c)) {
                    auto const opt = std::string{"-"} + c;
                    out.known.insert(opt);
                    out.declared.insert(opt);
                }
            }
        }
    }
}

void parse_struct_option(std::string_view code, CliOptions &out) {
    for (auto i = code.find("struct"); i != std::string_view::npos; i = code.find("struct", i + 1)) {
        auto j = i + std::string_view{"struct"}.size();
        while (j < code.size() && (is_space(code[j]) || code[j] == ':')) {
            ++j;
        }
        if (code.substr(j, 6) != "option" || (j + 6 < code.size() && is_ident_char(code[j + 6]))) {
            continue;
        }
        auto const brace = code.find_first_of("{;", i);
        if (brace == std::string_view::npos || code[brace] != '{') {
            continue;
        }
        for (auto const entry : split_args(bracket_content(code, brace))) {
            auto const open = entry.find('{');
            if (open == std::string_view::npos) {
                continue;
            }
            auto const fields = split_args(bracket_content(entry, open));
            if (fields.empty()) {
                continue;
            }
            auto const name = string_literals(fields[0]);
            if (name.empty() || name.front().empty()) {
                continue;
            }
            auto const long_opt = "--" + name.front();
            if (!is_option(long_opt)) {
                continue;
            }
            out.known.insert(long_opt);
            out.declared.insert(long_opt);
            if (fields.size() >= 4) {
                auto const val = fields[3];
                auto const q = val.find('\'');
                if (q != std::string_view::npos && q + 2 < val.size() && val[q + 2] == '\'' && is_alnum(val[q + 1])) {
                    add_pair(out, std::string{"-"} + val[q + 1], long_opt);
                }
            }
        }
    }
}

/// boost::program_options и cxxopts: add_options()("help,h", ...)("v,verbose", ...);
void parse_add_options(std::string_view code, CliOptions &out) {
    constexpr std::string_view KEY = "add_options";
    for (auto i = code.find(KEY); i != std::string_view::npos; i = code.find(KEY, i + 1)) {
        auto const end = code.find(';', i);
        auto const chain = code.substr(i, end == std::string_view::npos ? std::string_view::npos : end - i);
        for (std::size_t p = chain.find('('); p != std::string_view::npos; p = chain.find('(', p + 1)) {
            auto const args = split_args(bracket_content(chain, p));
            auto const lits = string_literals(args.empty() ? std::string_view{} : args[0]);
            if (lits.size() != 1 || lits.front().empty() || lits.front().front() == '-') {
                continue;
            }
            std::string short_opt;
            std::string long_opt;
            std::size_t start = 0;
            auto const &spec = lits.front();
            while (start <= spec.size()) {
                auto e = spec.find(',', start);
                if (e == std::string::npos) {
                    e = spec.size();
                }
                auto const part = spec.substr(start, e - start);
                if (part.size() == 1 && is_alnum(part[0])) {
                    short_opt = "-" + part;
                } else if (part.size() > 1 && std::all_of(part.begin(), part.end(), [](char c) { return is_alnum(c) || c == '-' || c == '_'; })) {
                    long_opt = "--" + part;
                }
                start = e + 1;
            }
            for (auto *o : {&short_opt, &long_opt}) {
                if (!o->empty() && !is_option(*o)) {
                    o->clear();
                }
                if (!o->empty()) {
                    out.known.insert(*o);
                    out.declared.insert(*o);
                }
            }
            if (!short_opt.empty() && !long_opt.empty()) {
                add_pair(out, short_opt, long_opt);
            }
        }
    }
}

/// Файл разбирает аргументы командной строки: только в таких файлах сравнения с ключами объявляют ключи.
bool parses_arguments(std::string_view code) {
    static constexpr std::array<std::string_view, 9> MARKERS{"argv",     "argc",           "getopt",        "add_argument",   "add_option",
                                                             "add_flag", "ArgumentParser", "parse_options", "program_options"};
    return std::any_of(MARKERS.begin(), MARKERS.end(), [&](std::string_view m) { return code.find(m) != std::string_view::npos; });
}

/// Литералы "-x", "--name" и "-f,--file": ручной разбор, CLI11, argparse.
void parse_literals(std::string_view code, bool python, CliOptions &out) {
    bool const cli_file = parses_arguments(code);
    std::size_t pos = 0;
    while (pos < code.size()) {
        auto eol = code.find('\n', pos);
        if (eol == std::string_view::npos) {
            eol = code.size();
        }
        auto const line = code.substr(pos, eol - pos);
        pos = eol + 1;
        if (line.find('-') == std::string_view::npos) {
            continue;
        }
        bool const strong = cli_file && declares_options(line);
        std::string short_opt;
        std::string long_opt;
        std::size_t count = 0;
        for (auto const &lit : string_literals(line, python)) {
            std::size_t start = 0;
            while (start <= lit.size()) {
                auto e = lit.find(',', start);
                if (e == std::string::npos) {
                    e = lit.size();
                }
                auto part = std::string_view{lit}.substr(start, e - start);
                while (!part.empty() && part.front() == ' ') {
                    part.remove_prefix(1);
                }
                if (is_option(part)) {
                    out.known.emplace(part);
                    if (strong) {
                        out.declared.emplace(part);
                    }
                    (part.starts_with("--") ? long_opt : short_opt) = part;
                    ++count;
                }
                start = e + 1;
            }
        }
        if (count == 2 && !short_opt.empty() && !long_opt.empty()) {
            add_pair(out, short_opt, long_opt);
        }
    }
}

/// Путь внутри каталога тестов, бенчмарков, примеров или стороннего кода: там другие программы со своими ключами.
bool auxiliary_path(std::string_view path) {
    static constexpr std::array<std::string_view, 10> PREFIXES{"test", "bench",       "example", "sample",   "demo",
                                                               "fuzz", "third_party", "vendor",  "external", "3rdparty"};
    std::size_t start = 0;
    while (true) {
        auto const slash = path.find('/', start);
        if (slash == std::string_view::npos) {
            return false;
        }
        auto const dir = path.substr(start, slash - start);
        if (std::any_of(PREFIXES.begin(), PREFIXES.end(), [&](std::string_view p) { return dir.starts_with(p); })) {
            return true;
        }
        start = slash + 1;
    }
}

/// Обрывает команду оболочки.
bool is_shell_operator(std::string_view t) { return t == "|" || t == "||" || t == "&&" || t == ";" || t == ">" || t == "<" || t.starts_with('#'); }

/// Токен запускает программу проекта: имя или путь, оканчивающийся на /имя.
bool is_program(std::string_view t, std::set<std::string, std::less<>> const &programs) {
    auto const slash = t.rfind('/');
    auto const base = slash == std::string_view::npos ? t : t.substr(slash + 1);
    return programs.contains(base);
}

/// Ключи программы проекта в командах строки: имя программы стоит в начале (после $, sudo, VAR=...) или после &&, ;, |.
std::vector<std::string> command_options(std::string_view line, std::set<std::string, std::less<>> const &programs) {
    std::vector<std::string> out;
    auto const tokens = split_ws(line);
    bool command_pos = true;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        auto const t = tokens[i];
        if (is_shell_operator(t)) {
            command_pos = true;
            continue;
        }
        if (!command_pos) {
            continue;
        }
        if (t == "$" || t == "sudo" || t == "env" || (t.find('=') != std::string_view::npos && !t.starts_with('-'))) {
            continue;
        }
        command_pos = false;
        if (!is_program(t, programs)) {
            continue;
        }
        for (auto j = i + 1; j < tokens.size() && !is_shell_operator(tokens[j]); ++j) {
            if (auto const c = clean_token(tokens[j]); is_option(c)) {
                out.emplace_back(c);
            }
        }
    }
    return out;
}

std::string_view trim_left_markup(std::string_view s) {
    // Пробелы, маркеры списка, ячейка таблицы, цитата.
    while (!s.empty()) {
        if (s.front() == ' ' || s.front() == '\t' || s.front() == '|' || s.front() == '>') {
            s.remove_prefix(1);
        } else if ((s.starts_with("- ") || s.starts_with("* ") || s.starts_with("+ "))) {
            s.remove_prefix(2);
        } else {
            break;
        }
    }
    return s;
}

} // namespace

void parse_cli_options(std::string_view source, bool python, CliOptions &out) {
    auto const stripped = strip_comments(source, python);
    std::string_view const code = stripped;
    if (!python) {
        parse_getopt(code, out);
        parse_struct_option(code, out);
        parse_add_options(code, out);
    }
    parse_literals(code, python, out);
}

CliOptions load_cli_options(fs::path const &root, Repo const &repo) {
    CliOptions out;
    for (auto const &f : repo.files()) {
        if (auxiliary_path(f)) {
            continue;
        }
        auto const ext = fs::path{f}.extension().string();
        bool const python = ext == ".py";
        if (python || ext == ".c" || ext == ".cc" || ext == ".cpp" || ext == ".cxx" || ext == ".h" || ext == ".hpp") {
            parse_cli_options(read_file(root / f), python, out);
        }
    }
    return out;
}

std::set<std::string, std::less<>> program_names(fs::path const &root, Repo const &repo) {
    std::set<std::string, std::less<>> out;
    if (auto const dir = fs::absolute(root).lexically_normal().filename().string(); !dir.empty()) {
        out.insert(dir);
    }
    for (auto const &f : repo.files()) {
        fs::path const p{f};
        auto const parent = p.parent_path().filename().string();
        if (parent == "bin" && !p.has_extension()) {
            out.insert(p.filename().string());
        }
        if (p.filename() != "CMakeLists.txt") {
            continue;
        }
        auto const text = read_file(root / f);
        std::string project;
        if (auto const open = find_calls(text, "project"); !open.empty()) {
            auto const args = split_ws(bracket_content(text, open.front()));
            if (!args.empty()) {
                project = std::string{clean_token(args.front())};
                out.insert(project);
            }
        }
        for (auto const open : find_calls(text, "add_executable")) {
            auto const args = split_ws(bracket_content(text, open));
            if (args.empty()) {
                continue;
            }
            std::string name{args.front()};
            if (auto const v = name.find("${PROJECT_NAME}"); v != std::string::npos && !project.empty()) {
                name.replace(v, std::string_view{"${PROJECT_NAME}"}.size(), project);
            }
            if (name.find('$') == std::string::npos) {
                out.insert(name);
            }
        }
    }
    return out;
}

std::vector<OptionMention> find_readme_options(std::string_view readme, Markdown const &md, std::set<std::string, std::less<>> const &programs) {
    std::vector<OptionMention> out;
    auto add = [&](std::vector<std::string> const &opts, std::size_t line) {
        for (auto const &o : opts) {
            out.push_back({o, line});
        }
    };
    std::size_t line_no = 0;
    std::size_t pos = 0;
    while (pos < readme.size()) {
        auto eol = readme.find('\n', pos);
        if (eol == std::string_view::npos) {
            eol = readme.size();
        }
        auto const raw = readme.substr(pos, eol - pos);
        pos = eol + 1;
        ++line_no;
        auto const line = strip_markup(raw);
        // (б) ключи в начале строки.
        add(leading_options(trim_left_markup(line)), line_no);
        // (а) команды программы проекта.
        add(command_options(line, programs), line_no);
    }
    // (в) inline-код только из ключей и плейсхолдеров.
    for (auto const &s : md.spans) {
        add(command_options(s.text, programs), s.line);
        auto const opts = leading_options(s.text);
        bool only_options = !opts.empty();
        for (auto tok : split_ws(s.text)) {
            auto const c = clean_token(tok);
            only_options = only_options && (is_option(c) || is_placeholder(c) || is_placeholder(tok) || tok.find('|') != std::string_view::npos);
        }
        if (only_options) {
            add(opts, s.line);
        }
    }
    std::stable_sort(out.begin(), out.end(), [](OptionMention const &a, OptionMention const &b) { return a.line < b.line; });
    // Ключ мог найтись и в начале строки, и в inline-коде той же строки.
    std::set<std::pair<std::size_t, std::string>> seen;
    std::erase_if(out, [&](OptionMention const &m) { return !seen.emplace(m.line, m.option).second; });
    return out;
}

std::vector<Diagnostic> check_options(std::vector<OptionMention> const &mentions, CliOptions const &code,
                                      std::set<std::string, std::less<>> const &ignore, std::string const &file) {
    std::vector<Diagnostic> out;
    if (code.known.empty() || mentions.empty()) {
        return out;
    }
    std::set<std::string, std::less<>> documented;
    for (auto const &m : mentions) {
        if (!documented.insert(m.option).second || ignore.contains(m.option)) {
            continue;
        }
        if (!code.known.contains(m.option)) {
            out.push_back({.file = file,
                           .rule = std::string{RULE},
                           .message = fmt::format("ключ '{}' описан в README, но не найден в коде", m.option),
                           .line = m.line});
        }
    }
    // Справку и версию описывать не обязательно, вместе с их короткими парами.
    auto const trivial = [&](std::string const &opt) {
        auto const p = code.pairs.find(opt);
        auto const is_trivial = [](std::string_view o) { return o == "--help" || o == "--version"; };
        return is_trivial(opt) || (p != code.pairs.end() && is_trivial(p->second));
    };
    // README описывает ключи программы: хотя бы 2 объявленных ключа и не меньше трети объявленных.
    auto const covered = static_cast<std::size_t>(std::count_if(code.declared.begin(), code.declared.end(), [&](std::string const &o) {
        auto const p = code.pairs.find(o);
        return documented.contains(o) || (p != code.pairs.end() && documented.contains(p->second));
    }));
    if (covered < MIN_DOCUMENTED || covered * 3 < code.declared.size()) {
        return out;
    }
    std::set<std::string, std::less<>> reported;
    for (auto const &opt : code.declared) {
        if (documented.contains(opt) || ignore.contains(opt) || trivial(opt) || reported.contains(opt)) {
            continue;
        }
        auto const p = code.pairs.find(opt);
        if (p != code.pairs.end() && (documented.contains(p->second) || ignore.contains(p->second))) {
            continue;
        }
        // Пара короткий/длинный сообщается одной строкой: "-c, --check".
        auto shown = opt;
        if (p != code.pairs.end()) {
            reported.insert(p->second);
            shown = opt.starts_with("--") ? p->second + ", " + opt : opt + ", " + p->second;
        }
        out.push_back({.file = file,
                       .rule = std::string{RULE},
                       .message = fmt::format("ключ '{}' есть в коде, но не описан в README", shown),
                       .line = mentions.front().line,
                       .severity = Severity::WARNING});
    }
    return out;
}
