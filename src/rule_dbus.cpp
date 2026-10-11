#include "rule_dbus.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <optional>

#include "cparse.hpp"

namespace fs = std::filesystem;

namespace {

constexpr std::string_view RULE = "dbus";
/// Сколько методов интерфейса перечислять в сообщении о ненайденном методе.
constexpr std::size_t MAX_LISTED = 6;

std::string read_file(fs::path const &path) {
    std::ifstream in{path, std::ios::binary};
    std::string data;
    std::array<char, 65536> buf{};
    while (in.read(buf.data(), buf.size()) || in.gcount() > 0) {
        data.append(buf.data(), static_cast<std::size_t>(in.gcount()));
    }
    return data;
}

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

bool is_ident_char(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; }

// ---------------------------------------------------------------- XML

/// Атрибуты тега: имя -> значение.
using Attrs = std::map<std::string, std::string, std::less<>>;

Attrs parse_attrs(std::string_view s) {
    Attrs out;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && is_space(s[i])) {
            ++i;
        }
        auto const name_begin = i;
        while (i < s.size() && !is_space(s[i]) && s[i] != '=') {
            ++i;
        }
        auto const name = s.substr(name_begin, i - name_begin);
        while (i < s.size() && is_space(s[i])) {
            ++i;
        }
        if (i >= s.size() || s[i] != '=') {
            ++i;
            continue;
        }
        ++i;
        while (i < s.size() && is_space(s[i])) {
            ++i;
        }
        if (i >= s.size() || (s[i] != '"' && s[i] != '\'')) {
            continue;
        }
        auto const quote = s[i++];
        auto const end = s.find(quote, i);
        if (end == std::string_view::npos) {
            break;
        }
        out.emplace(std::string{name}, std::string{s.substr(i, end - i)});
        i = end + 1;
    }
    return out;
}

std::string attr(Attrs const &a, std::string_view name) {
    auto const it = a.find(name);
    return it == a.end() ? std::string{} : it->second;
}

// ---------------------------------------------------------------- sd-bus

/// Значение аргумента-строки: склеенные литералы или NULL/nullptr как пустая строка.
std::optional<std::string> literal_value(std::string_view arg) {
    auto const b = arg.find_first_not_of(" \t\r\n");
    if (b == std::string_view::npos) {
        return std::nullopt;
    }
    arg = arg.substr(b, arg.find_last_not_of(" \t\r\n") - b + 1);
    if (arg == "NULL" || arg == "nullptr") {
        return std::string{};
    }
    if (!arg.starts_with('"')) {
        return std::nullopt;
    }
    std::string out;
    for (auto const &l : string_literals(arg)) {
        out += l;
    }
    return out;
}

/// Типы из SD_BUS_ARGS(...) / SD_BUS_RESULT(...): литералы через один (тип, имя-идентификатор).
std::optional<std::string> args_macro_types(std::string_view arg) {
    if (arg.find("SD_BUS_NO_ARGS") != std::string_view::npos || arg.find("SD_BUS_NO_RESULT") != std::string_view::npos) {
        return std::string{};
    }
    auto const open = arg.find('(');
    if (open == std::string_view::npos) {
        return std::nullopt;
    }
    std::string out;
    for (auto const &l : string_literals(bracket_content(arg, open))) {
        out += l;
    }
    return out;
}

// ---------------------------------------------------------------- команды

/// Токены строки оболочки с учётом кавычек; на неэкранированных операторах оболочки разбор прекращается.
std::vector<std::string> shell_tokens(std::string_view line) {
    std::vector<std::string> out;
    std::string cur;
    bool in_token = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        auto const c = line[i];
        if (c == '\'' || c == '"') {
            in_token = true;
            for (++i; i < line.size() && line[i] != c; ++i) {
                if (c == '"' && line[i] == '\\' && i + 1 < line.size()) {
                    ++i;
                }
                cur.push_back(line[i]);
            }
            continue;
        }
        if (is_space(c)) {
            if (in_token) {
                out.push_back(std::move(cur));
                cur.clear();
                in_token = false;
            }
            continue;
        }
        if ((c == '|' || c == ';' || c == '&' || c == '<' || c == '>' || c == '#') && !in_token) {
            out.emplace_back(1, c);
            continue;
        }
        cur.push_back(c);
        in_token = true;
    }
    if (in_token) {
        out.push_back(std::move(cur));
    }
    return out;
}

bool is_operator(std::string const &t) { return t == "|" || t == ";" || t == "&" || t == "<" || t == ">" || t == "#"; }

/// Код базового типа dbus-send.
std::optional<char> basic_type(std::string_view name) {
    static constexpr std::array<std::pair<std::string_view, char>, 11> TYPES{{{"string", 's'},
                                                                              {"int16", 'n'},
                                                                              {"uint16", 'q'},
                                                                              {"int32", 'i'},
                                                                              {"uint32", 'u'},
                                                                              {"int64", 'x'},
                                                                              {"uint64", 't'},
                                                                              {"double", 'd'},
                                                                              {"byte", 'y'},
                                                                              {"boolean", 'b'},
                                                                              {"objpath", 'o'}}};
    for (auto const &[n, code] : TYPES) {
        if (n == name) {
            return code;
        }
    }
    return std::nullopt;
}

/// Сигнатура одного аргумента dbus-send: тип:значение, array:тип:..., dict:тип:тип:..., variant:тип:...
std::optional<std::string> dbus_send_arg(std::string_view arg) {
    auto const c1 = arg.find(':');
    if (c1 == std::string_view::npos) {
        return std::nullopt;
    }
    auto const kind = arg.substr(0, c1);
    auto const rest = arg.substr(c1 + 1);
    if (auto const t = basic_type(kind)) {
        return std::string(1, *t);
    }
    auto const c2 = rest.find(':');
    auto const first = rest.substr(0, c2);
    if (kind == "variant") {
        return basic_type(first) ? std::optional<std::string>{"v"} : std::nullopt;
    }
    if (kind == "array") {
        auto const t = basic_type(first);
        return t ? std::optional<std::string>{std::string{"a"} + *t} : std::nullopt;
    }
    if (kind == "dict" && c2 != std::string_view::npos) {
        auto const second = rest.substr(c2 + 1, rest.find(':', c2 + 1) - c2 - 1);
        auto const k = basic_type(first);
        auto const v = second == "variant" ? std::optional<char>{'v'} : basic_type(second);
        if (k && v) {
            return std::string{"a{"} + *k + *v + "}";
        }
    }
    return std::nullopt;
}

bool looks_literal(std::string_view s) { return !s.empty() && s.find_first_of("<>${}") == std::string_view::npos; }

void scan_busctl(std::vector<std::string> const &t, std::size_t i, std::size_t line, std::vector<DbusCall> &out) {
    auto j = i + 1;
    while (j < t.size() && t[j].starts_with('-')) {
        bool const takes_value = (t[j] == "-H" || t[j] == "--host" || t[j] == "-M" || t[j] == "--machine");
        j += takes_value ? 2 : 1;
    }
    if (j >= t.size() || t[j] != "call") {
        return;
    }
    // call СЕРВИС ОБЪЕКТ ИНТЕРФЕЙС МЕТОД [СИГНАТУРА [АРГУМЕНТЫ...]]
    std::vector<std::string> args;
    for (++j; j < t.size() && !is_operator(t[j]); ++j) {
        if (!t[j].starts_with('-')) {
            args.push_back(t[j]);
        }
    }
    if (args.size() < 4 || !looks_literal(args[2]) || !looks_literal(args[3])) {
        return;
    }
    DbusCall c{.tool = "busctl", .interface = args[2], .method = args[3], .line = line};
    if (args.size() > 4) {
        c.signature = args[4];
        c.has_signature = true;
    }
    out.push_back(std::move(c));
}

void scan_dbus_send(std::vector<std::string> const &t, std::size_t i, std::size_t line, std::vector<DbusCall> &out) {
    bool method_call = false;
    auto j = i + 1;
    for (; j < t.size() && t[j].starts_with("--"); ++j) {
        if (t[j] == "--type=method_call" || t[j].starts_with("--print-reply")) {
            method_call = true;
        }
    }
    // ОБЪЕКТ ИНТЕРФЕЙС.МЕТОД [АРГУМЕНТЫ...]
    if (!method_call || j + 1 >= t.size()) {
        return;
    }
    auto const &member = t[j + 1];
    auto const dot = member.rfind('.');
    if (dot == std::string::npos || dot == 0 || !looks_literal(member)) {
        return;
    }
    DbusCall c{.tool = "dbus-send", .interface = member.substr(0, dot), .method = member.substr(dot + 1), .line = line, .has_signature = true};
    for (j += 2; j < t.size() && !is_operator(t[j]); ++j) {
        auto const sig = dbus_send_arg(t[j]);
        if (!sig) {
            c.problem = fmt::format("аргумент dbus-send '{}' не разобран, ожидается тип:значение", t[j]);
            break;
        }
        c.signature += *sig;
    }
    out.push_back(std::move(c));
}

std::string join_names(std::map<std::string, DbusMethod> const &methods) {
    std::string out;
    std::size_t n = 0;
    for (auto const &[name, m] : methods) {
        if (n++ == MAX_LISTED) {
            return out + ", ...";
        }
        out += (out.empty() ? "" : ", ") + name;
    }
    return out;
}

/// Сравнивает сигнатуру вызова с методом, добавляет ошибку при расхождении.
void compare(DbusCall const &c, DbusMethod const &m, std::string const &where, std::string const &file, std::vector<Diagnostic> &out) {
    if (!c.problem.empty()) {
        out.push_back({.file = file, .rule = std::string{RULE}, .message = c.problem, .line = c.line, .severity = Severity::WARNING});
        return;
    }
    if (c.signature == m.in) {
        return;
    }
    auto const msg = !c.has_signature
                         ? fmt::format("{} {}.{} вызывается без аргументов, а метод принимает '{}' ({})", c.tool, c.interface, c.method, m.in, where)
                         : fmt::format("сигнатура {}.{}: в README '{}', в {} '{}'", c.interface, c.method, c.signature, where, m.in);
    out.push_back({.file = file, .rule = std::string{RULE}, .message = msg, .line = c.line});
}

} // namespace

void parse_introspection_xml(std::string_view xml, std::string const &file, DbusModel &model) {
    std::string iface;
    std::string method;
    std::size_t i = 0;
    while ((i = xml.find('<', i)) != std::string_view::npos) {
        if (xml.substr(i).starts_with("<!--")) {
            auto const end = xml.find("-->", i);
            i = end == std::string_view::npos ? xml.size() : end + 3;
            continue;
        }
        auto const end = xml.find('>', i);
        if (end == std::string_view::npos) {
            break;
        }
        auto tag = xml.substr(i + 1, end - i - 1);
        i = end + 1;
        if (tag.starts_with('?') || tag.starts_with('!')) {
            continue;
        }
        bool const closing = tag.starts_with('/');
        bool const self_closing = tag.ends_with('/');
        if (closing) {
            tag.remove_prefix(1);
        }
        if (self_closing) {
            tag.remove_suffix(1);
        }
        auto const name_end = std::min(tag.find_first_of(" \t\r\n"), tag.size());
        auto const name = tag.substr(0, name_end);
        if (closing) {
            if (name == "interface") {
                iface.clear();
            } else if (name == "method") {
                method.clear();
            }
            continue;
        }
        auto const attrs = parse_attrs(tag.substr(name_end));
        if (name == "interface") {
            iface = attr(attrs, "name");
            if (!iface.empty()) {
                model.interfaces[iface].file = file;
            }
            if (self_closing) {
                iface.clear();
            }
        } else if (name == "method" && !iface.empty()) {
            method = attr(attrs, "name");
            model.interfaces[iface].methods[method];
            if (self_closing) {
                method.clear();
            }
        } else if (name == "arg" && !iface.empty() && !method.empty()) {
            auto &m = model.interfaces[iface].methods[method];
            (attr(attrs, "direction") == "out" ? m.out : m.in) += attr(attrs, "type");
        } else if (name == "property" && !iface.empty()) {
            model.interfaces[iface].properties[attr(attrs, "name")] = attr(attrs, "type");
        }
    }
}

void parse_sdbus_vtables(std::string_view code, std::string const &file, DbusModel &model) {
    constexpr std::string_view MACRO = "SD_BUS_METHOD";
    std::size_t i = 0;
    while ((i = code.find(MACRO, i)) != std::string_view::npos) {
        if (i > 0 && is_ident_char(code[i - 1])) {
            i += MACRO.size();
            continue;
        }
        auto j = i + MACRO.size();
        while (j < code.size() && is_ident_char(code[j])) {
            ++j;
        }
        auto const variant = code.substr(i, j - i);
        i = j;
        while (j < code.size() && is_space(code[j])) {
            ++j;
        }
        if (j >= code.size() || code[j] != '(') {
            continue;
        }
        auto const args = split_args(bracket_content(code, j));
        if (args.size() < 3) {
            continue;
        }
        auto const name = literal_value(args[0]);
        std::optional<std::string> in;
        std::optional<std::string> out;
        if (variant.find("WITH_ARGS") != std::string_view::npos) {
            in = args_macro_types(args[1]);
            out = args_macro_types(args[2]);
        } else if (variant.find("WITH_NAMES") != std::string_view::npos) {
            in = literal_value(args[1]);
            out = args.size() > 3 ? literal_value(args[3]) : std::nullopt;
        } else {
            in = literal_value(args[1]);
            out = literal_value(args[2]);
        }
        if (name && !name->empty() && in && out) {
            model.vtable_methods.push_back({*name, file, {*in, *out}});
        }
    }
}

DbusModel load_dbus_model(fs::path const &root, Repo const &repo) {
    DbusModel model;
    for (auto const &f : repo.files()) {
        auto const ext = fs::path{f}.extension().string();
        bool const xml = ext == ".xml";
        bool const code = ext == ".c" || ext == ".cc" || ext == ".cpp" || ext == ".cxx" || ext == ".h" || ext == ".hpp";
        if (!xml && !code) {
            continue;
        }
        auto const text = read_file(root / f);
        if (xml && text.find("<interface") != std::string::npos) {
            parse_introspection_xml(text, f, model);
        } else if (code && text.find("SD_BUS_METHOD") != std::string::npos) {
            parse_sdbus_vtables(strip_comments(text), f, model);
        }
    }
    return model;
}

std::vector<DbusCall> find_dbus_calls(std::string_view text) {
    std::vector<DbusCall> out;
    std::string logical;
    std::size_t logical_line = 0;
    std::size_t line_no = 0;
    std::size_t pos = 0;
    auto scan = [&] {
        std::replace(logical.begin(), logical.end(), '`', ' ');
        auto const t = shell_tokens(logical);
        for (std::size_t i = 0; i < t.size(); ++i) {
            if (t[i] == "busctl") {
                scan_busctl(t, i, logical_line, out);
            } else if (t[i] == "dbus-send") {
                scan_dbus_send(t, i, logical_line, out);
            }
        }
        logical.clear();
    };
    while (pos < text.size()) {
        auto eol = text.find('\n', pos);
        if (eol == std::string_view::npos) {
            eol = text.size();
        }
        auto line = text.substr(pos, eol - pos);
        pos = eol + 1;
        ++line_no;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
            line.remove_suffix(1);
        }
        if (logical.empty()) {
            logical_line = line_no;
        }
        if (line.ends_with('\\')) {
            logical.append(line.substr(0, line.size() - 1)).push_back(' ');
            continue;
        }
        logical.append(line);
        scan();
    }
    if (!logical.empty()) {
        scan();
    }
    return out;
}

std::vector<Diagnostic> check_dbus(std::string_view readme, DbusModel const &model, Repo const &repo, std::vector<fs::path> const &system_dirs,
                                   std::set<std::string, std::less<>> const &ignore, std::string const &file) {
    std::vector<Diagnostic> out;
    DbusModel system;
    for (auto const &c : find_dbus_calls(readme)) {
        if (ignore.contains(c.method) || ignore.contains(c.interface + "." + c.method)) {
            continue;
        }
        DbusInterface const *iface = nullptr;
        if (auto const it = model.interfaces.find(c.interface); it != model.interfaces.end()) {
            iface = &it->second;
        } else if (auto const sit = system.interfaces.find(c.interface); sit != system.interfaces.end()) {
            iface = &sit->second;
        } else {
            for (auto const &d : system_dirs) {
                std::error_code ec;
                if (auto const p = d / (c.interface + ".xml"); fs::is_regular_file(p, ec)) {
                    parse_introspection_xml(read_file(p), p.string(), system);
                    if (auto const loaded = system.interfaces.find(c.interface); loaded != system.interfaces.end()) {
                        iface = &loaded->second;
                    }
                    break;
                }
            }
        }
        if (iface != nullptr) {
            auto const m = iface->methods.find(c.method);
            if (m == iface->methods.end()) {
                out.push_back({.file = file,
                               .rule = std::string{RULE},
                               .message = fmt::format("метод '{}' не найден в интерфейсе '{}' ({}; есть: {})", c.method, c.interface, iface->file,
                                                      join_names(iface->methods)),
                               .line = c.line});
            } else {
                compare(c, m->second, iface->file, file, out);
            }
            continue;
        }
        auto const vt =
            std::find_if(model.vtable_methods.begin(), model.vtable_methods.end(), [&](VtableMethod const &v) { return v.name == c.method; });
        if (vt != model.vtable_methods.end()) {
            compare(c, vt->sig, fmt::format("SD_BUS_METHOD в {}", vt->file), file, out);
            continue;
        }
        if (c.interface.starts_with("org.freedesktop.") || repo.has_word(c.method)) {
            continue;
        }
        out.push_back({.file = file,
                       .rule = std::string{RULE},
                       .message = fmt::format("метод '{}.{}' не найден ни в описаниях D-Bus, ни в коде проекта", c.interface, c.method),
                       .line = c.line,
                       .severity = Severity::WARNING});
    }
    return out;
}
