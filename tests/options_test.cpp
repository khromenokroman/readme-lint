#include "options.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <vector>

namespace {

Options parse(std::vector<std::string_view> const &args) { return parse_options(args); }

/// Проверяет, что разбор бросает std::invalid_argument с текстом, содержащим expected.
void expect_error(std::vector<std::string_view> const &args, std::string const &expected) {
    std::string joined;
    for (auto a : args) {
        joined += std::string{a} + ' ';
    }
    SCOPED_TRACE("аргументы: " + joined);
    try {
        (void)parse(args);
        FAIL() << "ожидалось исключение с текстом '" << expected << "', разбор прошёл успешно";
    } catch (std::invalid_argument const &ex) {
        EXPECT_THAT(ex.what(), testing::HasSubstr(expected)) << "исключение брошено, но по другой причине";
    }
}

} // namespace

/// Без аргументов: текущий каталог и README.md.
TEST(Options, Defaults) {
    auto const opt = parse({});
    EXPECT_EQ(opt.repo, ".") << "каталог проекта по умолчанию должен быть текущим";
    EXPECT_EQ(opt.readme, "README.md") << "README по умолчанию должен быть README.md";
    EXPECT_FALSE(opt.dump) << "по умолчанию README проверяется, а не выводится";
    EXPECT_FALSE(opt.help);
    EXPECT_FALSE(opt.version);
}

/// --dump включает вывод разобранного README.
TEST(Options, Dump) { EXPECT_TRUE(parse({"--dump"}).dump); }

/// Короткие и длинные формы ключей дают одинаковый результат.
TEST(Options, ShortAndLongForms) {
    for (auto const &args : {std::vector<std::string_view>{"-f", "doc/README.md", "-h", "-V"},
                             std::vector<std::string_view>{"--file", "doc/README.md", "--help", "--version"}}) {
        SCOPED_TRACE("первый ключ: " + std::string{args[0]});
        auto const opt = parse(args);
        EXPECT_EQ(opt.readme, "doc/README.md");
        EXPECT_TRUE(opt.help);
        EXPECT_TRUE(opt.version);
    }
}

/// Каталог проекта задаётся позиционным аргументом в любом месте строки.
TEST(Options, RepoArgument) {
    EXPECT_EQ(parse({"/src/proj"}).repo, "/src/proj");
    EXPECT_EQ(parse({"-f", "R.md", "../proj"}).repo, "../proj");
}

/// Второй позиционный аргумент отклоняется.
TEST(Options, ExtraArgument) { expect_error({"a", "b"}, "лишний аргумент 'b'"); }

/// Ключ без значения в конце строки отклоняется.
TEST(Options, MissingValue) {
    expect_error({"-f"}, "для ключа -f не указано значение");
    expect_error({"--file"}, "для ключа --file не указано значение");
}

/// Пустые имена отклоняются.
TEST(Options, EmptyValues) {
    expect_error({"-f", ""}, "имя файла README не может быть пустым");
    expect_error({""}, "каталог проекта не может быть пустым");
}

/// Неизвестный ключ отклоняется с его именем в тексте ошибки.
TEST(Options, UnknownKey) { expect_error({"--bogus"}, "неизвестный ключ '--bogus'"); }
