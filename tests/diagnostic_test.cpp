#include "diagnostic.hpp"

#include <gtest/gtest.h>

/// Ошибка печатается в формате компилятора, чтобы IDE и редакторы делали строку кликабельной.
TEST(Diagnostic, FormatError) {
    Diagnostic const d{.file = "README.md", .rule = "names", .message = "файл 'src/a.cpp' не существует", .line = 42};
    EXPECT_EQ(format_diagnostic(d), "README.md:42: ошибка: файл 'src/a.cpp' не существует [names]");
}

/// Предупреждение отличается только уровнем.
TEST(Diagnostic, FormatWarning) {
    Diagnostic const d{.file = "doc/R.md", .rule = "deps", .message = "x", .line = 1, .severity = Severity::WARNING};
    EXPECT_EQ(format_diagnostic(d), "doc/R.md:1: предупреждение: x [deps]");
}
