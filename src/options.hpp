#pragma once
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/**
 * @brief Параметры запуска из командной строки.
 */
struct Options {
    std::filesystem::path repo{"."};                 ///< каталог проверяемого проекта
    std::filesystem::path readme{"README.md"};       ///< README, относительный путь считается от repo
    std::vector<std::filesystem::path> include_dirs; ///< системные include-каталоги, пустой: /usr/include и /usr/local/include
    bool dump = false;                               ///< вывести найденные блоки и inline-код вместо проверки
    bool help = false;                               ///< показать справку
    bool version = false;                            ///< показать версию
};

/**
 * @brief Разбирает аргументы командной строки.
 * @param args Аргументы без имени программы.
 * @return Разобранные параметры.
 * @throw std::invalid_argument Неизвестный ключ, нет значения или лишний аргумент.
 */
[[nodiscard]] Options parse_options(std::span<std::string_view const> args);

/**
 * @brief Текст справки по ключам.
 */
[[nodiscard]] std::string usage();
