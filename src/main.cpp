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
        auto const text = read_file(readme_path);
        (void)text;

        std::vector<Diagnostic> const found;
        for (auto const &d : found) {
            std::cout << format_diagnostic(d) << "\n";
        }
        return found.empty() ? EXIT_SUCCESS : EXIT_FINDINGS;
    } catch (std::exception const &ex) {
        std::cerr << "Ошибка: " << ex.what() << std::endl;
        return EXIT_USAGE;
    }
}
