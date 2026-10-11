#pragma once
#include <string>
#include <string_view>
#include <vector>

/**
 * @brief Делит список аргументов по запятым верхнего уровня с учётом скобок и строк.
 */
[[nodiscard]] std::vector<std::string_view> split_args(std::string_view s);

/**
 * @brief Содержимое скобок (), {} или [], открытых в позиции open; пустое, если скобка не закрыта.
 */
[[nodiscard]] std::string_view bracket_content(std::string_view s, std::size_t open);

/**
 * @brief Строковые литералы в тексте подряд, с раскрытыми escape-последовательностями \x.
 * @param s Текст.
 * @param single_quotes Считать литералами и строки в одинарных кавычках (Python).
 */
[[nodiscard]] std::vector<std::string> string_literals(std::string_view s, bool single_quotes = false);

/**
 * @brief Заменяет комментарии пробелами, сохраняя переводы строк и строковые литералы.
 * @param code Текст файла.
 * @param python Комментарии Python (#), иначе C/C++ (строчные и блочные).
 */
[[nodiscard]] std::string strip_comments(std::string_view code, bool python = false);
