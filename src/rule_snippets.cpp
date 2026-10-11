#include "rule_snippets.hpp"

#include <fmt/format.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <set>
#include <thread>

namespace fs = std::filesystem;

namespace {

constexpr std::string_view RULE = "snippets";
/// Предел времени компиляции одного примера, секунд.
constexpr int COMPILE_TIMEOUT = 60;
/// Больше каталогов -I не добавляется (огромные проекты).
constexpr std::size_t MAX_INCLUDE_DIRS = 500;
/// Код возврата оболочки, если команда не найдена.
constexpr int EXIT_NOT_FOUND = 127;

std::string_view trim(std::string_view s) {
    auto const b = s.find_first_not_of(" \t\r");
    if (b == std::string_view::npos) {
        return {};
    }
    return s.substr(b, s.find_last_not_of(" \t\r") - b + 1);
}

std::vector<std::string_view> lines_of(std::string_view text) {
    std::vector<std::string_view> out;
    std::size_t pos = 0;
    while (pos < text.size()) {
        auto eol = text.find('\n', pos);
        if (eol == std::string_view::npos) {
            eol = text.size();
        }
        out.push_back(text.substr(pos, eol - pos));
        pos = eol + 1;
    }
    return out;
}

bool is_c_family(std::string_view lang) {
    static constexpr std::array<std::string_view, 7> LANGS{"cpp", "c++", "cxx", "cc", "hpp", "h", "c"};
    return std::find(LANGS.begin(), LANGS.end(), lang) != LANGS.end();
}

std::string shell_quote(std::string const &s) {
    std::string out = "'";
    for (char c : s) {
        out += c == '\'' ? std::string{"'\\''"} : std::string(1, c);
    }
    return out + "'";
}

/// Выполняет команду, возвращает код возврата и объединённый вывод.
std::pair<int, std::string> run(std::string const &cmd) {
    std::unique_ptr<FILE, int (*)(FILE *)> pipe{popen((cmd + " 2>&1").c_str(), "r"), pclose};
    if (!pipe) {
        return {-1, {}};
    }
    std::string out;
    std::array<char, 4096> buf{};
    std::size_t n = 0;
    while ((n = std::fread(buf.data(), 1, buf.size(), pipe.get())) > 0) {
        out.append(buf.data(), n);
    }
    auto const status = pclose(pipe.release());
    return {status != -1 && WIFEXITED(status) ? WEXITSTATUS(status) : -1, out};
}

/// Строка-директива, которая при обёртке в main остаётся снаружи.
bool is_toplevel_directive(std::string_view line) {
    auto const t = trim(line);
    return t.starts_with('#') || t.starts_with("using namespace") || (t.starts_with("namespace ") && t.find('=') != std::string_view::npos) ||
           (t.starts_with("using ") && t.find('=') == std::string_view::npos && t.ends_with(';'));
}

/// Каталог -I для заголовка: путь до самого глубокого сегмента include, single_include или inc включительно,
/// иначе до самого глубокого src, иначе каталог самого заголовка.
std::string include_root(std::string const &header) {
    std::size_t include_end = std::string::npos;
    std::size_t src_end = std::string::npos;
    std::size_t start = 0;
    for (auto slash = header.find('/'); slash != std::string::npos; slash = header.find('/', start)) {
        auto const seg = std::string_view{header}.substr(start, slash - start);
        if (seg == "include" || seg == "single_include" || seg == "inc") {
            include_end = slash;
        } else if (seg == "src") {
            src_end = slash;
        }
        start = slash + 1;
    }
    if (include_end != std::string::npos) {
        return header.substr(0, include_end);
    }
    if (src_end != std::string::npos) {
        return header.substr(0, src_end);
    }
    auto const slash = header.rfind('/');
    return slash == std::string::npos ? std::string{} : header.substr(0, slash);
}

CompileResult compile(SnippetSource src, SnippetConfig const &cfg, fs::path const &dir, std::size_t id) {
    auto const path = dir / fmt::format("snippet{}.{}", id, src.c_language ? "c" : "cpp");
    src.file_name = path.string();
    std::ofstream{path} << src.text;
    // LC_ALL=C: сообщения компилятора на английском и с ASCII-кавычками на любой машине.
    auto cmd = fmt::format("LC_ALL=C timeout {} {} -fsyntax-only -w -x {} -std={}", COMPILE_TIMEOUT, cfg.compiler, src.c_language ? "c" : "c++",
                           src.c_language ? "c17" : cfg.cxx_std);
    for (auto const &d : cfg.include_dirs) {
        cmd += " -I" + shell_quote(d.string());
    }
    cmd += " " + shell_quote(path.string());
    auto const [code, output] = run(cmd);
    if (code == 0) {
        return {.ok = true};
    }
    if (code == EXIT_NOT_FOUND || code == -1) {
        return {.message = fmt::format("компилятор '{}' не запускается", cfg.compiler), .tool_failed = true};
    }
    return parse_compiler_output(output, src);
}

/// Строка README из места вывода компилятора "файл_примера:строка:", 0 если это не строка примера.
std::size_t snippet_line_at(std::string_view output, std::size_t p, SnippetSource const &src) {
    std::size_t n = 0;
    for (auto q = p + src.file_name.size() + 1; q < output.size() && output[q] >= '0' && output[q] <= '9'; ++q) {
        n = n * 10 + static_cast<std::size_t>(output[q] - '0');
    }
    return n > 0 && n <= src.line_map.size() ? src.line_map[n - 1] : 0;
}

/// Строка примера, откуда пришла ошибка в заголовке: сначала "required from here", затем любое упоминание
/// файла примера (In file included from).
std::size_t first_snippet_line(std::string_view output, SnippetSource const &src) {
    auto const key = src.file_name + ":";
    for (auto const line : lines_of(output)) {
        if (auto const p = line.find(key); p != std::string_view::npos && line.find("required from") != std::string_view::npos) {
            if (auto const n = snippet_line_at(line, p, src)) {
                return n;
            }
        }
    }
    for (auto p = output.find(key); p != std::string_view::npos; p = output.find(key, p + 1)) {
        if (auto const n = snippet_line_at(output, p, src)) {
            return n;
        }
    }
    return 0;
}

} // namespace

std::optional<std::string> skip_reason(CodeBlock const &block, std::string_view info) {
    if (!is_c_family(block.lang)) {
        return "не C/C++";
    }
    static constexpr std::array<std::string_view, 5> MARKS{"ignore", "skip", "no-compile", "nocompile", "fragment"};
    for (auto const m : MARKS) {
        if (info.find(m) != std::string_view::npos) {
            return fmt::format("помечен {}", m);
        }
    }
    if (block.text.find("#include") == std::string::npos && block.text.find("main(") == std::string::npos) {
        return "фрагмент без #include и main()";
    }
    for (auto const line : lines_of(block.text)) {
        auto const t = trim(line);
        if (t == "..." || t == "…" || t == "// ..." || t == "/* ... */" || t.find("{ ... }") != std::string_view::npos ||
            t.find("{...}") != std::string_view::npos || t.find("…") != std::string_view::npos) {
            return "есть пропуски ...";
        }
    }
    return std::nullopt;
}

SnippetSource as_is(CodeBlock const &block) {
    SnippetSource src{.text = block.text, .c_language = block.lang == "c"};
    auto const n = lines_of(block.text).size();
    for (std::size_t i = 0; i < n; ++i) {
        src.line_map.push_back(block.first_line + i);
    }
    return src;
}

SnippetSource wrapped(CodeBlock const &block) {
    SnippetSource src{.c_language = block.lang == "c"};
    auto const lines = lines_of(block.text);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (is_toplevel_directive(lines[i])) {
            src.text.append(lines[i]).push_back('\n');
            src.line_map.push_back(block.first_line + i);
        }
    }
    src.text += "int main() {\n";
    src.line_map.push_back(0);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (!is_toplevel_directive(lines[i])) {
            src.text.append(lines[i]).push_back('\n');
            src.line_map.push_back(block.first_line + i);
        }
    }
    src.text += "}\n";
    src.line_map.push_back(0);
    return src;
}

CompileResult parse_compiler_output(std::string_view output, SnippetSource const &src) {
    for (auto const line : lines_of(output)) {
        auto const err = line.find(": error: ");
        auto const fatal = line.find(": fatal error: ");
        auto const at = std::min(err, fatal);
        if (at == std::string_view::npos) {
            continue;
        }
        auto const text = trim(line.substr(at + (at == fatal ? 15 : 9)));
        // Номер строки: "путь:строка:колонка" перед ": error:".
        std::size_t readme_line = 0;
        auto const head = line.substr(0, at);
        bool const in_snippet = src.file_name.empty() || head.starts_with(src.file_name + ":");
        auto const c2 = head.rfind(':');
        auto const c1 = c2 == std::string_view::npos ? std::string_view::npos : head.rfind(':', c2 - 1);
        if (c1 != std::string_view::npos) {
            auto const num = head.substr(c1 + 1, c2 - c1 - 1);
            std::size_t n = 0;
            for (auto const ch : num) {
                n = ch >= '0' && ch <= '9' ? n * 10 + static_cast<std::size_t>(ch - '0') : 0;
            }
            if (in_snippet && n > 0 && n <= src.line_map.size()) {
                readme_line = src.line_map[n - 1];
            }
        }
        if (readme_line == 0 && !in_snippet && !src.file_name.empty()) {
            // Ошибка в заголовке: строка примера, откуда пришли (required from here, In file included from).
            readme_line = first_snippet_line(output, src);
        }
        bool const missing = text.find("No such file or directory") != std::string_view::npos ||
                             text.find("file not found") != std::string_view::npos;
        return {.message = std::string{text}, .line = readme_line, .missing_header = missing};
    }
    return {.message = "компилятор завершился с ошибкой без сообщения error:", .line = 0};
}

SnippetConfig snippet_config(fs::path const &root, Repo const &repo, std::string const &compiler) {
    SnippetConfig cfg;
    if (!compiler.empty()) {
        cfg.compiler = compiler;
    } else if (auto const *env = std::getenv("CXX"); env != nullptr && *env != '\0') {
        cfg.compiler = env;
    }
    cfg.jobs = std::max(1U, std::thread::hardware_concurrency());
    std::set<std::string> dirs{""};
    for (auto const &f : repo.files()) {
        auto const ext = fs::path{f}.extension().string();
        if ((ext == ".h" || ext == ".hpp" || ext == ".hh" || ext == ".hxx" || ext == ".ipp" || ext == ".inl") && dirs.size() < MAX_INCLUDE_DIRS) {
            dirs.insert(include_root(f));
        }
        if (fs::path{f}.filename() == "CMakeLists.txt" && f.find('/') == std::string::npos) {
            std::ifstream in{root / f};
            std::string const text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
            for (auto const *key : {"CMAKE_CXX_STANDARD ", "cxx_std_"}) {
                if (auto const p = text.find(key); p != std::string::npos) {
                    auto q = p + std::string_view{key}.size();
                    std::string num;
                    while (q < text.size() && text[q] >= '0' && text[q] <= '9') {
                        num.push_back(text[q++]);
                    }
                    if (!num.empty()) {
                        cfg.cxx_std = "c++" + num;
                        break;
                    }
                }
            }
        }
    }
    for (auto const &d : dirs) {
        auto const p = d.empty() ? root : root / d;
        std::error_code ec;
        if (fs::is_directory(p, ec)) {
            cfg.include_dirs.push_back(fs::absolute(p));
        }
    }
    return cfg;
}

std::vector<Diagnostic> check_snippets(Markdown const &md, std::string_view readme, SnippetConfig const &config, std::string const &file) {
    std::vector<Diagnostic> out;
    auto const readme_lines = lines_of(readme);
    std::vector<CodeBlock const *> todo;
    for (auto const &b : md.blocks) {
        auto const info = b.line - 1 < readme_lines.size() ? readme_lines[b.line - 1] : std::string_view{};
        if (b.closed && !skip_reason(b, info)) {
            todo.push_back(&b);
        }
    }
    if (todo.empty()) {
        return out;
    }
    auto const dir = fs::temp_directory_path() / fmt::format("readme-lint-{}", getpid());
    fs::create_directories(dir);
    std::vector<std::pair<CompileResult, CompileResult>> results(todo.size());
    std::atomic<std::size_t> next{0};
    auto worker = [&] {
        for (auto i = next++; i < todo.size(); i = next++) {
            auto const plain = compile(as_is(*todo[i]), config, dir, i * 2);
            results[i].first = plain;
            // Пример со своим main() не оборачивается: его объявления должны остаться снаружи.
            bool const has_main = todo[i]->text.find("main(") != std::string::npos;
            if (!plain.ok && !plain.missing_header && !plain.tool_failed && !has_main) {
                results[i].second = compile(wrapped(*todo[i]), config, dir, i * 2 + 1);
            } else {
                results[i].second = plain;
            }
        }
    };
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < std::min<std::size_t>(config.jobs, todo.size()); ++t) {
        threads.emplace_back(worker);
    }
    for (auto &t : threads) {
        t.join();
    }
    std::error_code ec;
    fs::remove_all(dir, ec);

    for (std::size_t i = 0; i < todo.size(); ++i) {
        auto const &[plain, wrap] = results[i];
        if (plain.ok || wrap.ok) {
            continue;
        }
        if (plain.tool_failed) {
            // Компилятор не запустился: одно сообщение на весь README.
            out.push_back({.file = file, .rule = std::string{RULE}, .message = plain.message, .line = todo[i]->line, .severity = Severity::WARNING});
            break;
        }
        // Вариант, где компилятор продвинулся дальше, ближе к настоящей ошибке.
        auto const &best = wrap.line > plain.line && !wrap.tool_failed ? wrap : plain;
        auto const line = best.line != 0 ? best.line : todo[i]->line;
        if (best.missing_header) {
            out.push_back({.file = file,
                           .rule = std::string{RULE},
                           .message = fmt::format("пример не проверен: {}", best.message),
                           .line = line,
                           .severity = Severity::WARNING});
        } else {
            out.push_back(
                {.file = file, .rule = std::string{RULE}, .message = fmt::format("пример не компилируется: {}", best.message), .line = line});
        }
    }
    return out;
}
