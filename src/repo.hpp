#pragma once
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

/**
 * @brief Файлы проверяемого проекта и слова из его кода.
 */
class Repo {
   public:
    /**
     * @brief Загружает список файлов через git ls-files, вне git обходит каталог (без скрытых и build*).
     * @param root Корень проекта.
     * @throw std::runtime_error Каталог не читается.
     */
    explicit Repo(std::filesystem::path root);

    /**
     * @brief Создаёт проект из готового списка файлов и текста кода (для тестов).
     * @param files Пути относительно корня через '/'.
     * @param code Текст, слова которого считаются кодом проекта.
     */
    Repo(std::vector<std::string> files, std::string_view code);

    /// Файл или каталог с таким относительным путём есть в проекте.
    [[nodiscard]] bool has_path(std::string_view path) const;

    /// Какой-то файл проекта заканчивается на "/" + path (путь от include-каталога внутри проекта).
    [[nodiscard]] bool has_path_suffix(std::string_view path) const;

    /// Какой-то файл проекта совпадает с glob-шаблоном ('*', '?', '[...]' в пределах сегмента).
    [[nodiscard]] bool matches_glob(std::string_view pattern) const;

    /// Конец пути какого-то файла проекта (после '/') совпадает с glob-шаблоном.
    [[nodiscard]] bool matches_glob_suffix(std::string_view pattern) const;

    /// Файл с таким именем есть в любом каталоге проекта.
    [[nodiscard]] bool has_basename(std::string_view name) const;

    /// Каталог верхнего уровня или файл в корне с таким именем существует.
    [[nodiscard]] bool has_top_level(std::string_view name) const;

    /// Слово встречается в коде проекта как идентификатор или совпадает с именем файла без расширения.
    [[nodiscard]] bool has_word(std::string_view word) const;

   private:
    void index(std::string_view code);

    std::vector<std::string> m_files;               ///< файлы относительно корня, отсортированы
    std::unordered_set<std::string> m_words;        ///< идентификаторы из кода проекта
    std::set<std::string, std::less<>> m_dirs;      ///< все каталоги, в которых есть файлы
    std::set<std::string, std::less<>> m_basenames; ///< имена файлов без каталогов
};

/**
 * @brief Файл с этим путём считается кодом: в нём ищутся идентификаторы из README.
 *
 * Файлы без расширения проверяются отдельно при загрузке: кодом считаются скрипты с "#!".
 * Документация (.md, .puml, .txt кроме CMakeLists.txt) кодом не считается: устаревшее описание
 * в ней не должно подтверждать устаревший README.
 */
[[nodiscard]] bool is_code_file(std::string_view path);

/**
 * @brief Добавляет в out все идентификаторы [A-Za-z_][A-Za-z0-9_]* из текста.
 */
void collect_words(std::string_view text, std::unordered_set<std::string> &out);

/**
 * @brief Системные include-каталоги: ищет в них файлы и слова, которых нет в проекте.
 *
 * Каталоги читаются лениво и только при первом запросе, поиск слов прекращается, когда найдены все.
 */
class SystemHeaders {
   public:
    /// @param dirs Каталоги поиска, например /usr/include.
    explicit SystemHeaders(std::vector<std::filesystem::path> dirs);

    /// Файл dir/path существует в одном из каталогов.
    [[nodiscard]] bool has_path(std::string_view path) const;

    /// Файл с таким именем есть в одном из каталогов на любой глубине.
    [[nodiscard]] bool has_basename(std::string_view name);

    /**
     * @brief Возвращает слова из wanted, найденные в файлах каталогов.
     */
    [[nodiscard]] std::set<std::string> find_words(std::set<std::string, std::less<>> const &wanted) const;

   private:
    std::vector<std::filesystem::path> m_dirs;      ///< каталоги поиска
    std::set<std::string, std::less<>> m_basenames; ///< имена файлов, заполняется при первом запросе
    bool m_basenames_loaded = false;                ///< m_basenames уже заполнен
};
