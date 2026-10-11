#include "suppress.hpp"

#include <gtest/gtest.h>

#include <string>

namespace {

Suppressions make(std::string const &readme) { return Suppressions{readme, parse_markdown(readme)}; }

} // namespace

/// Участок без правил отключает всё между маркерами, сами маркеры и строки вне участка не затронуты.
TEST(Suppress, AllRules) {
    auto const s = make("a\n<!-- readme-lint-disable -->\nb\nc\n<!-- readme-lint-enable -->\nd\n");
    for (std::size_t line : {3U, 4U}) {
        EXPECT_TRUE(s.suppressed(line, "names")) << "строка " << line;
        EXPECT_TRUE(s.suppressed(line, "options")) << "строка " << line;
    }
    for (std::size_t line : {1U, 6U}) {
        EXPECT_FALSE(s.suppressed(line, "names")) << "строка " << line;
    }
}

/// Список правил: отключены только они; маркер без пробелов внутри комментария тоже работает.
TEST(Suppress, SelectedRules) {
    auto const s = make("<!--readme-lint-disable names, options-->\nb\n<!--readme-lint-enable names, options-->\nc\n");
    EXPECT_TRUE(s.suppressed(2, "names"));
    EXPECT_TRUE(s.suppressed(2, "options"));
    EXPECT_FALSE(s.suppressed(2, "deps"));
    EXPECT_FALSE(s.suppressed(4, "names"));
}

/// enable без правил закрывает все участки; незакрытый disable действует до конца файла.
TEST(Suppress, EnableAllAndUnclosed) {
    auto const s = make("<!-- readme-lint-disable names -->\na\n<!-- readme-lint-enable -->\nb\n<!-- readme-lint-disable -->\nc\nd\n");
    EXPECT_TRUE(s.suppressed(2, "names"));
    EXPECT_FALSE(s.suppressed(4, "names"));
    EXPECT_TRUE(s.suppressed(6, "deps"));
    EXPECT_TRUE(s.suppressed(1000, "deps")) << "до конца файла";
}

/// Маркер в блоке кода или посреди строки только пример, он ничего не отключает.
TEST(Suppress, MarkersInExamples) {
    auto const s = make("```\n<!-- readme-lint-disable -->\n```\nтекст `<!-- readme-lint-disable -->` пример\nx\n");
    EXPECT_FALSE(s.suppressed(5, "names"));
}

/// visible убирает блоки и inline-код участка для правила, filter убирает расхождения.
TEST(Suppress, VisibleAndFilter) {
    auto const readme = std::string{"`a_b`\n<!-- readme-lint-disable names -->\n`c_d`\n```cpp\nx\n```\n<!-- readme-lint-enable names -->\n`e_f`\n"};
    auto const md = parse_markdown(readme);
    Suppressions const s{readme, md};
    auto const v = s.visible(md, "names");
    ASSERT_EQ(v.spans.size(), 2U);
    EXPECT_EQ(v.spans[0].text, "a_b");
    EXPECT_EQ(v.spans[1].text, "e_f");
    EXPECT_TRUE(v.blocks.empty());
    EXPECT_EQ(s.visible(md, "deps").spans.size(), 3U) << "для других правил всё видно";
    std::vector<Diagnostic> d{{.file = "R", .rule = "names", .message = "x", .line = 3}, {.file = "R", .rule = "deps", .message = "y", .line = 3}};
    s.filter(d);
    ASSERT_EQ(d.size(), 1U);
    EXPECT_EQ(d[0].rule, "deps");
}
