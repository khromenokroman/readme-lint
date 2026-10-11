#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "diagnostic.hpp"
#include "markdown.hpp"
#include "options.hpp"
#include "repo.hpp"
#include "rule_dbus.hpp"
#include "rule_deps.hpp"
#include "rule_names.hpp"

namespace {

constexpr int EXIT_FINDINGS = 1;
constexpr int EXIT_USAGE = 2;

/// Файл исключений в корне проверяемого проекта.
constexpr char const IGNORE_FILE[] = ".readme-lint-ignore";

std::string read_file(std::filesystem::path const &path) {
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        throw std::runtime_error("не удалось открыть " + path.string());
    }
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

void dump(Markdown const &md, std::string const &file) {
    for (auto const &b : md.blocks) {
        auto const lines = static_cast<std::size_t>(std::count(b.text.begin(), b.text.end(), '\n'));
        std::cout << file << ":" << b.line << ": блок [" << b.lang << "], строк " << lines << (b.closed ? "" : ", не закрыт") << "\n";
    }
    for (auto const &s : md.spans) {
        std::cout << file << ":" << s.line << ":" << s.column << ": `" << s.text << "`\n";
    }
}

} // namespace

int main(int argc, char *argv[]) {
    try {
        std::vector<std::string_view> const args(argv + 1, argv + argc);
        auto const opt = parse_options(args);
        if (opt.help) {
            std::cout << usage();
            return EXIT_SUCCESS;
        }
        if (opt.version) {
            std::cout << "readme-lint " << README_LINT_VERSION << "\n";
            return EXIT_SUCCESS;
        }
        if (!std::filesystem::is_directory(opt.repo)) {
            throw std::runtime_error("каталог проекта " + opt.repo.string() + " не найден");
        }
        auto const readme_path = opt.readme.is_absolute() ? opt.readme : opt.repo / opt.readme;
        auto const readme_text = read_file(readme_path);
        auto const md = parse_markdown(readme_text);
        auto const file = opt.readme.string();
        if (opt.dump) {
            dump(md, file);
            return EXIT_SUCCESS;
        }

        auto found = check_markdown(md, file);
        Repo const repo{opt.repo};
        SystemHeaders headers{opt.include_dirs.empty() ? std::vector<std::filesystem::path>{"/usr/include", "/usr/local/include"} : opt.include_dirs};
        auto const ignore = load_ignore(opt.repo / IGNORE_FILE);
        auto const readme_dir =
            std::filesystem::absolute(readme_path).lexically_relative(std::filesystem::absolute(opt.repo)).parent_path().generic_string();
        auto names = check_names(md, repo, headers, ignore, file, readme_dir);
        found.insert(found.end(), names.begin(), names.end());
        auto deps = check_deps(md, load_dep_sources(opt.repo), file);
        found.insert(found.end(), deps.begin(), deps.end());
        auto dbus = check_dbus(readme_text, load_dbus_model(opt.repo, repo), repo, {"/usr/share/dbus-1/interfaces"}, ignore, file);
        found.insert(found.end(), dbus.begin(), dbus.end());
        std::stable_sort(found.begin(), found.end(), [](Diagnostic const &a, Diagnostic const &b) { return a.line < b.line; });
        for (auto const &d : found) {
            std::cout << format_diagnostic(d) << "\n";
        }
        bool const errors = std::any_of(found.begin(), found.end(), [](Diagnostic const &d) { return d.severity == Severity::ERROR; });
        return errors ? EXIT_FINDINGS : EXIT_SUCCESS;
    } catch (std::exception const &ex) {
        std::cerr << "Ошибка: " << ex.what() << std::endl;
        return EXIT_USAGE;
    }
}
