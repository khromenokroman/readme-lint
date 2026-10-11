#include "markdown.hpp"

#include <gtest/gtest.h>

#include <string>

namespace {

/// Печать inline-кода в сообщениях gtest.
std::string describe(InlineCode const &s) { return "`" + s.text + "` в " + std::to_string(s.line) + ":" + std::to_string(s.column); }

} // namespace

/// Блок ``` с языком: язык, содержимое и номера строк ограждения и первой строки кода.
TEST(Markdown, FencedBlock) {
    auto const md = parse_markdown("# T\n\n```bash\napt install x\nmake\n```\nтекст\n");
    ASSERT_EQ(md.blocks.size(), 1U) << "ожидался один блок";
    auto const &b = md.blocks[0];
    EXPECT_EQ(b.lang, "bash");
    EXPECT_EQ(b.text, "apt install x\nmake\n");
    EXPECT_EQ(b.line, 3U) << "строка открывающего ограждения";
    EXPECT_EQ(b.first_line, 4U) << "строка первой строки кода";
    EXPECT_TRUE(b.closed);
}

/// Ограждения ~~~ и из 4 кавычек, как в README рабочих проектов (````bash).
TEST(Markdown, TildeAndLongFences) {
    auto const md = parse_markdown("~~~\nа\n~~~\n````bash\nб\n```\nв\n````\n");
    ASSERT_EQ(md.blocks.size(), 2U);
    EXPECT_EQ(md.blocks[0].lang, "");
    EXPECT_EQ(md.blocks[0].text, "а\n");
    EXPECT_EQ(md.blocks[1].lang, "bash");
    EXPECT_EQ(md.blocks[1].text, "б\n```\nв\n") << "``` короче открывающего ```` и не закрывает блок";
}

/// Блок внутри пункта списка с отступом закрывается ограждением с тем же отступом.
TEST(Markdown, IndentedFenceInList) {
    auto const md = parse_markdown("- пункт\n  ````bash\n  monitor -f net\n  ````\n- `после`\n");
    ASSERT_EQ(md.blocks.size(), 1U);
    EXPECT_EQ(md.blocks[0].text, "  monitor -f net\n") << "отступ содержимого сохраняется как есть";
    ASSERT_EQ(md.spans.size(), 1U) << "блок должен закрыться, иначе `после` попадёт внутрь";
    EXPECT_EQ(md.spans[0].text, "после");
}

/// Язык берётся до пробела или '{', атрибуты отбрасываются.
TEST(Markdown, LangFromInfoString) {
    EXPECT_EQ(parse_markdown("```cpp title=\"a\"\n```\n").blocks.at(0).lang, "cpp");
    EXPECT_EQ(parse_markdown("```c++{.line-numbers}\n```\n").blocks.at(0).lang, "c++");
}

/// Ограждение с текстом после кавычек не закрывает блок.
TEST(Markdown, CloseFenceWithInfoDoesNotClose) {
    auto const md = parse_markdown("```\nа\n```cpp\nб\n```\n");
    ASSERT_EQ(md.blocks.size(), 1U);
    EXPECT_EQ(md.blocks[0].text, "а\n```cpp\nб\n");
}

/// Незакрытый блок идёт до конца файла и даёт ошибку markdown на строке открытия.
TEST(Markdown, UnclosedBlock) {
    auto const md = parse_markdown("текст\n```cpp\nint x;\n`не inline`\n");
    ASSERT_EQ(md.blocks.size(), 1U);
    EXPECT_FALSE(md.blocks[0].closed);
    EXPECT_TRUE(md.spans.empty()) << "кавычки внутри незакрытого блока не являются inline-кодом";
    auto const diags = check_markdown(md, "README.md");
    ASSERT_EQ(diags.size(), 1U);
    EXPECT_EQ(format_diagnostic(diags[0]), "README.md:2: ошибка: блок кода не закрыт, всё до конца файла отображается как код [markdown]");
}

/// Закрытые блоки ошибок не дают.
TEST(Markdown, ClosedBlocksNoDiagnostics) { EXPECT_TRUE(check_markdown(parse_markdown("```\nx\n```\n"), "R.md").empty()); }

/// Inline-код: текст, строка и колонка открывающей кавычки (в байтах, с 1).
TEST(Markdown, InlineSpans) {
    auto const md = parse_markdown("Файл `src/a.cpp` и `Foo::bar()`.\nещё `x`\n");
    ASSERT_EQ(md.spans.size(), 3U);
    EXPECT_EQ(md.spans[0].text, "src/a.cpp") << describe(md.spans[0]);
    EXPECT_EQ(md.spans[0].line, 1U);
    EXPECT_EQ(md.spans[0].column, 10U) << "\"Файл \" занимает 9 байт в UTF-8";
    EXPECT_EQ(md.spans[1].text, "Foo::bar()");
    EXPECT_EQ(md.spans[2].line, 2U);
}

/// Двойные кавычки позволяют кавычку внутри, пробелы по краям снимаются.
TEST(Markdown, DoubleBacktickSpan) {
    auto const md = parse_markdown("пример `` `a` `` и ``x`y``\n");
    ASSERT_EQ(md.spans.size(), 2U);
    EXPECT_EQ(md.spans[0].text, "`a`");
    EXPECT_EQ(md.spans[1].text, "x`y");
}

/// Экранированная и незакрытая кавычки не открывают inline-код.
TEST(Markdown, EscapedAndUnmatched) {
    auto const md = parse_markdown("\\`не код\\` и ``одна и `да`\n");
    ASSERT_EQ(md.spans.size(), 1U) << (md.spans.empty() ? "" : describe(md.spans[0]));
    EXPECT_EQ(md.spans[0].text, "да");
}

/// Inline-код внутри блока кода не выделяется.
TEST(Markdown, NoSpansInsideBlock) { EXPECT_TRUE(parse_markdown("```\n`x`\n```\n").spans.empty()); }

/// Переводы строк \r\n не попадают в текст и не сбивают нумерацию.
TEST(Markdown, CrLf) {
    auto const md = parse_markdown("```sh\r\nls\r\n```\r\n`a`\r\n");
    ASSERT_EQ(md.blocks.size(), 1U);
    EXPECT_EQ(md.blocks[0].text, "ls\n");
    ASSERT_EQ(md.spans.size(), 1U);
    EXPECT_EQ(md.spans[0].line, 4U);
}

/// Пустой текст и текст без перевода строки в конце.
TEST(Markdown, EdgeInputs) {
    EXPECT_TRUE(parse_markdown("").blocks.empty());
    auto const md = parse_markdown("`a`");
    ASSERT_EQ(md.spans.size(), 1U);
    EXPECT_EQ(md.spans[0].text, "a");
}

/// Span переходит на следующую строку абзаца, как в README с ручным переносом.
TEST(Markdown, SpanAcrossLines) {
    auto const md = parse_markdown("функции (`<name>_load(path, options,\nresult_items)`, `<name>_commit(...)`)\n");
    ASSERT_EQ(md.spans.size(), 2U) << "ожидались 2 span, первый на двух строках";
    EXPECT_EQ(md.spans[0].text, "<name>_load(path, options, result_items)") << "перевод строки внутри span становится пробелом";
    EXPECT_EQ(md.spans[0].line, 1U) << "строка span: строка открывающей кавычки";
    EXPECT_EQ(md.spans[1].text, "<name>_commit(...)");
    EXPECT_EQ(md.spans[1].line, 2U);
    EXPECT_EQ(md.spans[1].column, 17U);
}

/// Отступ строки-продолжения не попадает в текст span.
TEST(Markdown, SpanContinuationIndent) {
    auto const md = parse_markdown("- пункт `a\n  b` конец\n");
    ASSERT_EQ(md.spans.size(), 1U);
    EXPECT_EQ(md.spans[0].text, "a b");
}

/// Span не переходит через пустую строку, пункт списка, заголовок и строку таблицы.
TEST(Markdown, SpanStopsAtBlockBoundary) {
    for (std::string const sep : {"\n", "- пункт\n", "# заголовок\n", "| a | b |\n", "1. пункт\n"}) {
        SCOPED_TRACE("граница: " + sep);
        auto const md = parse_markdown("начало `a\n" + sep + "конец` b\n");
        EXPECT_TRUE(md.spans.empty()) << "кавычки по разные стороны границы не должны образовать span, найден `"
                                      << (md.spans.empty() ? "" : md.spans[0].text) << "`";
    }
}
