#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

/**
 * @brief Серьёзность найденного расхождения.
 */
enum class Severity : std::uint8_t {
    ERROR,   ///< README точно не совпадает с кодом
    WARNING, ///< вероятное расхождение, нужна проверка человеком
};

/**
 * @brief Одно расхождение README с кодом.
 */
struct Diagnostic {
    std::string file;    ///< файл README, как его видит пользователь
    std::string rule;    ///< имя правила, нашедшего расхождение
    std::string message; ///< описание расхождения
    std::size_t line{};  ///< номер строки в README, с 1
    Severity severity = Severity::ERROR;
};

/**
 * @brief Форматирует расхождение в стиле компилятора: "файл:строка: ошибка: текст [правило]".
 */
[[nodiscard]] std::string format_diagnostic(Diagnostic const &d);
