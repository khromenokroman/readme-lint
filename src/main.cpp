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

namespace {

constexpr int EXIT_FINDINGS = 1;
constexpr int EXIT_USAGE = 2;

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
        auto const md = parse_markdown(read_file(readme_path));
        auto const file = opt.readme.string();
        if (opt.dump) {
            dump(md, file);
            return EXIT_SUCCESS;
        }

        auto const found = check_markdown(md, file);
        for (auto const &d : found) {
            std::cout << format_diagnostic(d) << "\n";
        }
        return found.empty() ? EXIT_SUCCESS : EXIT_FINDINGS;
    } catch (std::exception const &ex) {
        std::cerr << "Ошибка: " << ex.what() << std::endl;
        return EXIT_USAGE;
    }
}
