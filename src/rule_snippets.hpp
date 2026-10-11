#pragma once
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "diagnostic.hpp"
#include "markdown.hpp"
#include "repo.hpp"

/**
 * @brief Как компилировать примеры.
 */
struct SnippetConfig {
    std::string compiler{"c++"};                     ///< компилятор C++ (он же компилирует C через -x c)
    std::string cxx_std{"c++20"};                    ///< стандарт C++ для -std=
    std::vector<std::filesystem::path> include_dirs; ///< каталоги -I
    unsigned jobs = 1;                               ///< сколько примеров компилировать одновременно
};

/**
 * @brief Исходник для компилятора, построенный из блока README.
 */
struct SnippetSource {
    std::string text{};                  ///< текст файла
    std::vector<std::size_t> line_map{}; ///< строка файла (с 0) -> строка README, 0 для добавленных строк
    std::string file_name{};             ///< имя файла примера в выводе компилятора
    bool c_language = false;             ///< компилировать как C
};

/**
 * @brief Результат компиляции примера.
 */
struct CompileResult {
    std::string message{};       ///< текст первой ошибки компилятора
    std::size_t line{};          ///< строка README с ошибкой, 0 если неизвестна
    bool ok = false;             ///< компиляция без ошибок
    bool missing_header = false; ///< ошибка из-за отсутствующего заголовка
    bool tool_failed = false;    ///< компилятор не запустился
};

/**
 * @brief Причина не компилировать блок или nullopt, если блок нужно проверить.
 *
 * Проверяются блоки cpp, c++, cxx, cc, hpp, h, c с #include или main(, без пометок ignore, skip,
 * no-compile, nocompile, fragment в info-строке и без пропусков: строка "...", "…", "// ...", "{ ... }".
 */
[[nodiscard]] std::optional<std::string> skip_reason(CodeBlock const &block, std::string_view info);

/**
 * @brief Блок как есть.
 */
[[nodiscard]] SnippetSource as_is(CodeBlock const &block);

/**
 * @brief Блок, обёрнутый в int main(): строки #include, using namespace и namespace x = остаются снаружи.
 */
[[nodiscard]] SnippetSource wrapped(CodeBlock const &block);

/**
 * @brief Разбирает вывод компилятора: первая строка "файл:строка:колонка: error: текст" (или fatal error).
 *
 * Строка пересчитывается в строку README, только если ошибка в файле примера (src.file_name);
 * ошибка в заголовке проекта даёт line = 0.
 * @param output Вывод компилятора (LC_ALL=C).
 * @param src Исходник, для пересчёта строки в строку README.
 */
[[nodiscard]] CompileResult parse_compiler_output(std::string_view output, SnippetSource const &src);

/**
 * @brief Настройки компиляции для проекта: стандарт из CMake, -I: корень и для каждого заголовка самый глубокий
 * каталог include, single_include или inc в его пути, иначе самый глубокий src, иначе каталог заголовка.
 * @param root Корень проекта.
 * @param repo Файлы проекта.
 * @param compiler Компилятор; пустой: $CXX или c++.
 */
[[nodiscard]] SnippetConfig snippet_config(std::filesystem::path const &root, Repo const &repo, std::string const &compiler);

/**
 * @brief Правило snippets: примеры C/C++ из README компилируются (-fsyntax-only).
 *
 * Пример без main() компилируется как есть и обёрнутым в int main() { ... }; хватает одного успешного
 * варианта, иначе сообщается ошибка варианта, где компилятор продвинулся дальше.
 * Ошибка компиляции: ошибка на строке README. Нет заголовка (не установлена библиотека): предупреждение.
 * Компилятор не запускается: одно предупреждение на первом проверяемом блоке.
 * @param md Разобранный README.
 * @param readme Текст README, для info-строк блоков.
 * @param config Настройки компиляции.
 * @param file Имя README для сообщений.
 */
[[nodiscard]] std::vector<Diagnostic> check_snippets(Markdown const &md, std::string_view readme, SnippetConfig const &config,
                                                     std::string const &file);
