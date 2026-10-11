#pragma once
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "diagnostic.hpp"

/**
 * @brief Блок кода, огороженный ``` или ~~~.
 */
struct CodeBlock {
    std::string lang;         ///< первое слово info-строки ("cpp", "bash"), может быть пустым
    std::string text;         ///< содержимое без строк-ограждений, строки через '\n'
    std::size_t line{};       ///< номер строки с открывающим ограждением, с 1
    std::size_t first_line{}; ///< номер первой строки содержимого, с 1
    bool closed = true; ///< false: закрывающее ограждение не найдено, блок идёт до конца файла
};

/**
 * @brief Фрагмент inline-кода `...` вне блоков кода.
 */
struct InlineCode {
    std::string text;     ///< содержимое без обратных кавычек
    std::size_t line{};   ///< номер строки с открывающей кавычкой, с 1
    std::size_t column{}; ///< позиция открывающей кавычки в строке (в байтах), с 1
};

/**
 * @brief Элементы README, которые проверяют правила.
 */
struct Markdown {
    std::vector<CodeBlock> blocks; ///< блоки кода в порядке появления
    std::vector<InlineCode> spans; ///< inline-код в порядке появления
};

/**
 * @brief Разбирает markdown и выделяет блоки кода и inline-код с номерами строк.
 *
 * Поддерживается подмножество CommonMark: огороженные блоки (``` и ~~~ длиной от 3, с любым отступом,
 * закрываются тем же символом не короче открывающего), inline-код из N обратных кавычек, закрытый ровно
 * N кавычками в пределах абзаца (может переходить на следующую строку), экранирование \`.
 * Абзац обрывается пустой строкой, заголовком, строкой таблицы, цитатой и пунктом списка.
 * Блоки с отступом 4 пробела не выделяются.
 * @param text Текст README, переводы строк "\n" или "\r\n".
 */
[[nodiscard]] Markdown parse_markdown(std::string_view text);

/**
 * @brief Ошибки разметки самого README: незакрытые блоки кода (правило "markdown").
 * @param md Разобранный README.
 * @param file Имя README для сообщений.
 */
[[nodiscard]] std::vector<Diagnostic> check_markdown(Markdown const &md, std::string const &file);
