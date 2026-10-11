#pragma once
#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "diagnostic.hpp"
#include "markdown.hpp"
#include "repo.hpp"

/**
 * @brief Чем является inline-код README для правила names.
 */
enum class SpanKind : std::uint8_t {
    NONE,       ///< не проверяется: слово, команда, значение, URL
    PATH,       ///< путь к файлу или каталогу проекта
    FILE_NAME,  ///< имя файла без каталога
    IDENTIFIER, ///< имя из кода: функция, переменная, тип
};

/**
 * @brief Результат классификации inline-кода.
 */
struct SpanClass {
    std::string name; ///< что искать: нормализованный путь или последний компонент имени
    SpanKind kind = SpanKind::NONE; ///< вид
};

/**
 * @brief Определяет, что проверять в inline-коде.
 *
 * Путь: есть '/' и (известное расширение или первый сегмент есть в корне проекта),
 * без пробелов, URL, абсолютных путей и плейсхолдеров. Имя файла: без '/', с известным расширением.
 * Идентификатор: C++-имя с '_', '::', '()' или camelCase; foo(...) сокращается до foo, A::b до b,
 * имена std:: не проверяются.
 * @param text Содержимое inline-кода.
 * @param repo Проект, нужен для проверки первого сегмента пути.
 */
[[nodiscard]] SpanClass classify_span(std::string_view text, Repo const &repo);

/**
 * @brief Читает файл исключений: одно имя на строку, '#' начинает комментарий.
 * @return Пустое множество, если файла нет.
 */
[[nodiscard]] std::set<std::string, std::less<>> load_ignore(std::filesystem::path const &path);

/**
 * @brief Правило names: пути, имена файлов и идентификаторы из inline-кода существуют в проекте.
 *
 * Каждое имя проверяется один раз, расхождение выдаётся на первом упоминании. Не найденный путь и имя
 * из кода: ошибка; голое имя файла и ЗАГЛАВНОЕ имя (переменная окружения, CMake): предупреждение.
 * @param md Разобранный README.
 * @param repo Проект.
 * @param headers Системные include-каталоги для внешних имён.
 * @param ignore Имена, которые не проверяются (сравниваются с текстом span и с найденным именем).
 * @param file Имя README для сообщений.
 * @param readme_dir Каталог README относительно корня проекта ("" для корня).
 */
[[nodiscard]] std::vector<Diagnostic> check_names(Markdown const &md, Repo const &repo, SystemHeaders &headers,
                                                  std::set<std::string, std::less<>> const &ignore, std::string const &file,
                                                  std::string const &readme_dir);
