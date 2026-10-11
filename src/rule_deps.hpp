#pragma once
#include <cstddef>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "diagnostic.hpp"
#include "markdown.hpp"

/**
 * @brief Пакет из команды установки.
 */
struct Package {
    std::string name;    ///< имя пакета без архитектуры
    std::string version; ///< версия после '=', пустая, если не указана
};

/**
 * @brief Команда apt install / apt-get install.
 */
struct AptCommand {
    std::vector<Package> packages; ///< пакеты в порядке указания
    std::size_t line{};            ///< строка начала команды, с 1
};

/**
 * @brief Источник списка сборочных зависимостей проекта.
 */
struct DepSource {
    std::string file;                            ///< путь относительно корня проекта, для сообщений
    std::map<std::string, std::string> packages; ///< имя -> версия (пустая, если не зафиксирована)
    bool complete = false; ///< полный список сборочных зависимостей (не CI с служебными пакетами)
};

/**
 * @brief Находит команды apt install в тексте.
 *
 * Строки, заканчивающиеся '\', склеиваются. Префиксы sudo и приглашения "$ " пропускаются.
 * Команда обрывается на &&, ||, ;, |, <, >, #. Пропускаются опции, пути (./x.deb) и аргументы
 * с подстановками ($, <, `, ().
 * @param text Текст: содержимое блока кода, inline-код или файл CI.
 * @param first_line Номер строки первой строки текста.
 */
[[nodiscard]] std::vector<AptCommand> find_apt_commands(std::string_view text, std::size_t first_line);

/**
 * @brief Разбирает build-deps.lst: имя[=версия] через пробелы, '#' комментарий, строки с $ пропускаются.
 */
[[nodiscard]] DepSource parse_build_deps_lst(std::string_view text, std::string file);

/**
 * @brief Разбирает Build-Depends* из debian/control; версия берётся только из ограничения (= V).
 */
[[nodiscard]] DepSource parse_debian_control(std::string_view text, std::string file);

/**
 * @brief Собирает пакеты из команд apt install в файле CI; такой источник неполный.
 */
[[nodiscard]] DepSource parse_ci_file(std::string_view text, std::string file);

/**
 * @brief Загружает источники зависимостей проекта: build-deps.lst, debian/control, файлы .yml и .yaml в .github/workflows.
 * @param root Корень проекта.
 */
[[nodiscard]] std::vector<DepSource> load_dep_sources(std::filesystem::path const &root);

/**
 * @brief Правило deps: команды apt install из README согласованы с источниками зависимостей.
 *
 * Ошибка: версия пакета в README отличается от версии в источнике. Предупреждения: пакета из сборочной
 * команды README нет ни в одном источнике; пакета из полного источника нет в команде README, которая
 * покрывает не меньше половины этого источника. Команды, ни один пакет которых не входит в источники,
 * считаются установкой зависимостей для запуска и не проверяются.
 * @param md Разобранный README.
 * @param sources Источники зависимостей; пустой список: правило ничего не проверяет.
 * @param file Имя README для сообщений.
 */
[[nodiscard]] std::vector<Diagnostic> check_deps(Markdown const &md, std::vector<DepSource> const &sources, std::string const &file);
