#pragma once
#include <cstddef>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "diagnostic.hpp"
#include "markdown.hpp"
#include "repo.hpp"

/**
 * @brief Ключи командной строки, найденные в коде.
 */
struct CliOptions {
    std::set<std::string, std::less<>> known; ///< все ключи ("-h", "--help"), включая найденные только как литерал
    std::set<std::string, std::less<>> declared; ///< ключи из строгих источников: getopt, struct option, add_options, сравнения
    std::map<std::string, std::string, std::less<>> pairs; ///< короткий <-> длинный ключ одного параметра, в обе стороны
};

/**
 * @brief Ключ, упомянутый в README.
 */
struct OptionMention {
    std::string option; ///< "-f" или "--file", без значения
    std::size_t line{}; ///< строка README, с 1
};

/**
 * @brief Добавляет ключи из кода C/C++ или Python.
 *
 * Строгие источники: строка getopt/getopt_long(_only), массив struct option, add_options()("name,n"),
 * литералы "-x"/"--xx" в строках со сравнением или регистрацией (==, strcmp, add_option, add_flag,
 * add_argument) в файлах, разбирающих аргументы (argv, argc, getopt, add_argument, parse_options...).
 * Остальные литералы-ключи попадают только в known. В Python разбираются только литералы.
 * Комментарии перед разбором вырезаются.
 * @param code Текст файла.
 * @param python Файл Python: литералы и в одинарных кавычках.
 * @param out Результат.
 */
void parse_cli_options(std::string_view code, bool python, CliOptions &out);

/**
 * @brief Загружает ключи из файлов кода проекта (.c, .cc, .cpp, .cxx, .h, .hpp, .py), кроме каталогов
 * test*, bench*, example*, sample*, demo*, fuzz*, third_party, vendor, external, 3rdparty.
 */
[[nodiscard]] CliOptions load_cli_options(std::filesystem::path const &root, Repo const &repo);

/**
 * @brief Имена программ проекта: add_executable и project в CMakeLists.txt, файлы без расширения в bin/, имя каталога.
 */
[[nodiscard]] std::set<std::string, std::less<>> program_names(std::filesystem::path const &root, Repo const &repo);

/**
 * @brief Находит ключи программы в README.
 *
 * (а) в командах программы: имя (или путь ./build/имя) в начале строки или inline-кода, после $, sudo,
 * VAR=..., или после &&, ;, |; (б) в начале строки:
 * строка таблицы, пункт списка, строка справки "-h, --help  текст", ключи и плейсхолдеры до первого
 * обычного слова; (в) в inline-коде, состоящем только из ключей и плейсхолдеров.
 * @param readme Текст README.
 * @param md Разобранный README (для inline-кода).
 * @param programs Имена программ проекта.
 */
[[nodiscard]] std::vector<OptionMention> find_readme_options(std::string_view readme, Markdown const &md,
                                                             std::set<std::string, std::less<>> const &programs);

/**
 * @brief Правило options: ключи README и кода совпадают.
 *
 * Ключ README, которого нет в коде: ошибка. Ключ из строгого источника, не описанный в README (с учётом
 * пары короткий/длинный, кроме --help и --version): предупреждение на строке первого ключа README; только
 * если README описывает не меньше 2 объявленных ключей и не меньше трети из них. Если в коде ключей нет,
 * правило ничего не проверяет.
 */
[[nodiscard]] std::vector<Diagnostic> check_options(std::vector<OptionMention> const &mentions, CliOptions const &code,
                                                    std::set<std::string, std::less<>> const &ignore, std::string const &file);
