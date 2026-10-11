#include "repo.hpp"

#include <fnmatch.h>
#include <sys/wait.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <stdexcept>

namespace fs = std::filesystem;

namespace {

/// Файлы больше этого размера не читаются при поиске слов (сгенерированные дампы, бинарные данные).
constexpr std::uintmax_t MAX_CODE_FILE = 4U * 1024U * 1024U;

bool is_word_start(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }

bool is_word_char(char c) { return is_word_start(c) || (c >= '0' && c <= '9'); }

std::string read_file(fs::path const &path) {
    std::ifstream in{path, std::ios::binary | std::ios::ate};
    if (!in) {
        return {};
    }
    std::string data(static_cast<std::size_t>(in.tellg()), '\0');
    in.seekg(0);
    in.read(data.data(), static_cast<std::streamsize>(data.size()));
    data.resize(static_cast<std::size_t>(in.gcount()));
    return data;
}

/// Слово встречается в тексте целиком, не как часть более длинного идентификатора.
bool contains_word(std::string_view text, std::string_view word) {
    auto const *const begin = text.data();
    auto const *const end = begin + text.size();
    auto const *p = begin;
    while (p < end) {
        auto const *hit = static_cast<char const *>(memmem(p, static_cast<std::size_t>(end - p), word.data(), word.size()));
        if (hit == nullptr) {
            return false;
        }
        auto const *const after = hit + word.size();
        if ((hit == begin || !is_word_char(hit[-1])) && (after == end || !is_word_char(*after))) {
            return true;
        }
        p = hit + 1;
    }
    return false;
}

std::string shell_quote(std::string const &s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out.push_back(c);
        }
    }
    return out + "'";
}

/// Список файлов из git ls-files; пустой, если каталог не в git или git недоступен.
std::vector<std::string> git_files(fs::path const &root) {
    // safe.directory: в контейнере CI владелец каталога другой, а список файлов только читается.
    auto const cmd = "git -c safe.directory='*' -C " + shell_quote(root.string()) + " ls-files -z 2>/dev/null";
    std::unique_ptr<FILE, int (*)(FILE *)> pipe{popen(cmd.c_str(), "r"), pclose};
    if (!pipe) {
        return {};
    }
    std::string out;
    std::array<char, 65536> buf{};
    std::size_t n = 0;
    while ((n = std::fread(buf.data(), 1, buf.size(), pipe.get())) > 0) {
        out.append(buf.data(), n);
    }
    auto const status = pclose(pipe.release());
    if (status == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return {};
    }
    std::vector<std::string> files;
    std::size_t pos = 0;
    while (pos < out.size()) {
        auto const end = out.find('\0', pos);
        files.push_back(out.substr(pos, end - pos));
        pos = end == std::string::npos ? out.size() : end + 1;
    }
    return files;
}

/// Каталоги сборки, VCS и IDE не считаются частью проекта; остальные скрытые (.github) считаются.
bool skip_dir(std::string const &name) {
    return name == ".git" || name == ".idea" || name == ".vscode" || name == ".cache" || name.starts_with("build") ||
           name.starts_with("cmake-build-");
}

std::vector<std::string> walk_files(fs::path const &root) {
    std::vector<std::string> files;
    std::error_code ec;
    fs::recursive_directory_iterator it{root, fs::directory_options::skip_permission_denied, ec};
    if (ec) {
        throw std::runtime_error("не удалось прочитать каталог " + root.string() + ": " + ec.message());
    }
    for (; it != fs::recursive_directory_iterator{}; it.increment(ec)) {
        if (ec) {
            break;
        }
        auto const name = it->path().filename().string();
        if (it->is_directory(ec)) {
            if (skip_dir(name)) {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (it->is_regular_file(ec)) {
            files.push_back(it->path().lexically_relative(root).generic_string());
        }
    }
    return files;
}

std::string_view basename_of(std::string_view path) {
    auto const slash = path.rfind('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

std::string normalize(std::string_view path) {
    while (path.starts_with("./")) {
        path.remove_prefix(2);
    }
    while (path.size() > 1 && path.ends_with('/')) {
        path.remove_suffix(1);
    }
    return std::string{path};
}

} // namespace

bool is_code_file(std::string_view path) {
    auto const name = basename_of(path);
    if (name == "CMakeLists.txt" || name == "Makefile" || name == "meson.build" || name == "conanfile.py") {
        return true;
    }
    static constexpr std::array<std::string_view, 26> EXTENSIONS{".c",   ".cc",    ".cpp", ".cxx",  ".h",       ".hh",   ".hpp",  ".hxx",  ".ipp",
                                                                 ".tpp", ".inl",   ".py",  ".sh",   ".cmake",   ".xml",  ".yang", ".json", ".in",
                                                                 ".lst", ".proto", ".yml", ".yaml", ".service", ".conf", ".toml", ".ini"};
    return std::any_of(EXTENSIONS.begin(), EXTENSIONS.end(), [&](std::string_view ext) { return name.ends_with(ext); });
}

namespace {

/// Вызывает fn для каждого идентификатора [A-Za-z_][A-Za-z0-9_]* в тексте.
template <typename Fn>
void for_each_word(std::string_view text, Fn &&fn) {
    std::size_t i = 0;
    while (i < text.size()) {
        if (!is_word_start(text[i]) || (i > 0 && is_word_char(text[i - 1]))) {
            ++i;
            continue;
        }
        auto j = i + 1;
        while (j < text.size() && is_word_char(text[j])) {
            ++j;
        }
        fn(text.substr(i, j - i));
        i = j;
    }
}

} // namespace

void collect_words(std::string_view text, std::unordered_set<std::string> &out) {
    for_each_word(text, [&](std::string_view w) { out.emplace(w); });
}

Repo::Repo(fs::path root) {
    m_files = git_files(root);
    if (m_files.empty()) {
        m_files = walk_files(root);
    }
    std::sort(m_files.begin(), m_files.end());
    for (auto const &f : m_files) {
        bool const no_ext = basename_of(f).find('.') == std::string_view::npos;
        if (!is_code_file(f) && !no_ext) {
            continue;
        }
        std::error_code ec;
        auto const path = root / f;
        if (auto const size = fs::file_size(path, ec); !ec && size <= MAX_CODE_FILE) {
            auto const data = read_file(path);
            // Файл без расширения считается кодом, только если это скрипт.
            if (!no_ext || is_code_file(f) || data.starts_with("#!")) {
                collect_words(data, m_words);
            }
        }
    }
    index({});
}

Repo::Repo(std::vector<std::string> files, std::string_view code) : m_files(std::move(files)) {
    std::sort(m_files.begin(), m_files.end());
    index(code);
}

void Repo::index(std::string_view code) {
    collect_words(code, m_words);
    for (auto const &f : m_files) {
        auto const base = basename_of(f);
        m_basenames.emplace(base);
        // Имя программы или скрипта без расширения (bin/deploy_tool, tools/gen.py) тоже считается словом проекта.
        m_words.emplace(base.substr(0, base.find('.')));
        for (auto slash = f.find('/'); slash != std::string::npos; slash = f.find('/', slash + 1)) {
            m_dirs.emplace(f.substr(0, slash));
        }
    }
}

bool Repo::has_path(std::string_view path) const {
    auto const p = normalize(path);
    return std::binary_search(m_files.begin(), m_files.end(), p) || m_dirs.contains(p);
}

bool Repo::has_path_suffix(std::string_view path) const {
    auto const suffix = "/" + normalize(path);
    auto const ends = [&](std::string const &s) { return s.ends_with(suffix); };
    return std::any_of(m_files.begin(), m_files.end(), ends) || std::any_of(m_dirs.begin(), m_dirs.end(), ends);
}

bool Repo::matches_glob(std::string_view pattern) const {
    auto const p = normalize(pattern);
    return std::any_of(m_files.begin(), m_files.end(), [&](std::string const &f) { return fnmatch(p.c_str(), f.c_str(), FNM_PATHNAME) == 0; });
}

bool Repo::matches_glob_suffix(std::string_view pattern) const {
    auto const p = normalize(pattern);
    return std::any_of(m_files.begin(), m_files.end(), [&](std::string const &f) {
        for (auto slash = f.find('/'); slash != std::string::npos; slash = f.find('/', slash + 1)) {
            if (fnmatch(p.c_str(), f.c_str() + slash + 1, FNM_PATHNAME) == 0) {
                return true;
            }
        }
        return false;
    });
}

bool Repo::has_basename(std::string_view name) const { return m_basenames.contains(name); }

bool Repo::has_top_level(std::string_view name) const {
    auto const n = std::string{name};
    return m_dirs.contains(n) || std::binary_search(m_files.begin(), m_files.end(), n);
}

bool Repo::has_word(std::string_view word) const { return m_words.contains(std::string{word}); }

SystemHeaders::SystemHeaders(std::vector<fs::path> dirs) : m_dirs(std::move(dirs)) {}

bool SystemHeaders::has_path(std::string_view path) const {
    std::error_code ec;
    return std::any_of(m_dirs.begin(), m_dirs.end(), [&](fs::path const &d) { return fs::exists(d / normalize(path), ec); });
}

bool SystemHeaders::has_basename(std::string_view name) {
    if (!m_basenames_loaded) {
        m_basenames_loaded = true;
        for (auto const &d : m_dirs) {
            std::error_code ec;
            for (fs::recursive_directory_iterator it{d, fs::directory_options::skip_permission_denied, ec}, end; !ec && it != end; it.increment(ec)) {
                m_basenames.emplace(it->path().filename().string());
            }
        }
    }
    return m_basenames.contains(name);
}

std::set<std::string> SystemHeaders::find_words(std::set<std::string, std::less<>> const &wanted) const {
    std::set<std::string> found;
    if (wanted.empty()) {
        return found;
    }
    for (auto const &d : m_dirs) {
        std::error_code ec;
        for (fs::recursive_directory_iterator it{d, fs::directory_options::skip_permission_denied, ec}, end; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec)) {
                continue;
            }
            auto const data = read_file(it->path());
            for (auto const &w : wanted) {
                if (!found.contains(w) && contains_word(data, w)) {
                    found.insert(w);
                }
            }
            if (found.size() == wanted.size()) {
                return found;
            }
        }
    }
    return found;
}
