#pragma once
#include <cstddef>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "diagnostic.hpp"
#include "markdown.hpp"

/**
 * @brief Участки README, где правила отключены маркерами.
 *
 * Маркер стоит отдельной строкой вне блока кода:
 * <!-- readme-lint-disable --> отключает все правила, <!-- readme-lint-disable names options -->
 * только перечисленные; <!-- readme-lint-enable --> (или с теми же правилами) включает обратно.
 * Незакрытый disable действует до конца файла.
 */
class Suppressions {
   public:
    /**
     * @brief Находит маркеры в README.
     * @param readme Текст README.
     * @param md Разобранный README: маркеры внутри блоков кода не действуют.
     */
    Suppressions(std::string_view readme, Markdown const &md);

    /// Правило rule отключено на строке line.
    [[nodiscard]] bool suppressed(std::size_t line, std::string_view rule) const;

    /// Копия README без блоков и inline-кода, на которые правило rule не распространяется.
    [[nodiscard]] Markdown visible(Markdown const &md, std::string_view rule) const;

    /// Убирает расхождения на отключённых строках.
    void filter(std::vector<Diagnostic> &diags) const;

   private:
    /// Состояние после строки-маркера: отключены все правила или набор правил.
    struct Range {
        std::set<std::string, std::less<>> rules; ///< пустое при all: отключено всё
        std::size_t first{};                      ///< первая отключённая строка
        std::size_t last{};                       ///< последняя отключённая строка
        bool all = false;                         ///< отключены все правила
    };

    std::vector<Range> m_ranges; ///< отключённые участки в порядке строк
};
