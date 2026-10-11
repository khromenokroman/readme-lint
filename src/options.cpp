#include "options.hpp"

#include <fmt/format.h>

#include <stdexcept>

namespace {

std::string_view take_value(std::span<std::string_view const> args, std::size_t &i) {
    if (i + 1 >= args.size()) {
        throw std::invalid_argument(fmt::format("для ключа {} не указано значение", args[i]));
    }
    return args[++i];
}

} // namespace

Options parse_options(std::span<std::string_view const> args) {
    Options opt;
    bool repo_set = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        auto const arg = args[i];
        if (arg == "-h" || arg == "--help") {
            opt.help = true;
        } else if (arg == "-V" || arg == "--version") {
            opt.version = true;
        } else if (arg == "--dump") {
            opt.dump = true;
        } else if (arg == "-I" || arg == "--include") {
            auto const value = take_value(args, i);
            if (value.empty()) {
                throw std::invalid_argument("include-каталог не может быть пустым");
            }
            opt.include_dirs.emplace_back(value);
        } else if (arg == "-f" || arg == "--file") {
            auto const value = take_value(args, i);
            if (value.empty()) {
                throw std::invalid_argument("имя файла README не может быть пустым");
            }
            opt.readme = value;
        } else if (arg.starts_with('-')) {
            throw std::invalid_argument(fmt::format("неизвестный ключ '{}', справка: readme-lint --help", arg));
        } else {
            if (repo_set) {
                throw std::invalid_argument(fmt::format("лишний аргумент '{}', каталог проекта уже указан", arg));
            }
            if (arg.empty()) {
                throw std::invalid_argument("каталог проекта не может быть пустым");
            }
            opt.repo = arg;
            repo_set = true;
        }
    }
    return opt;
}

std::string usage() {
    return "Использование: readme-lint [ключи] [КАТАЛОГ]\n"
           "Сверяет README.md проекта с его кодом и выводит расхождения.\n"
           "КАТАЛОГ: корень проекта (по умолчанию текущий).\n"
           "\n"
           "  -f, --file ФАЙЛ   проверяемый README, путь от корня проекта (по умолчанию README.md)\n"
           "  -I, --include КАТ системный include-каталог для внешних имён, можно несколько\n"
           "                    (по умолчанию /usr/include и /usr/local/include)\n"
           "      --dump        вывести найденные в README блоки кода и inline-код, без проверки\n"
           "  -V, --version     версия программы\n"
           "  -h, --help        эта справка\n"
           "\n"
           "Код возврата: 0 ошибок нет (предупреждения возможны), 1 найдены ошибки, 2 ошибка запуска.\n";
}
