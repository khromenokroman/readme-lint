#include "rule_names.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

Repo sample() {
    return Repo{{"src/main.cpp", "src/plugin.cpp", "src/app/server.cpp", "src/app/server.hpp", "module/acme/os/clock.py", "lib/include/acme/x.hpp",
                 "build-deps.lst"},
                "int list_sessions(); struct Container { int m_cache; }; void on_save_done(); int OrderParser;"};
}

std::string kind_name(SpanKind k) {
    switch (k) {
        case SpanKind::NONE:
            return "NONE";
        case SpanKind::PATH:
            return "PATH";
        case SpanKind::FILE_NAME:
            return "FILE_NAME";
        case SpanKind::IDENTIFIER:
            return "IDENTIFIER";
    }
    return "?";
}

void expect_class(std::string_view text, SpanKind kind, std::string const &name = {}) {
    auto const c = classify_span(text, sample());
    EXPECT_EQ(kind_name(c.kind), kind_name(kind)) << "inline-код `" << text << "`";
    if (!name.empty()) {
        EXPECT_EQ(c.name, name) << "inline-код `" << text << "`";
    }
}

/// Проверяет README из текста с пустыми системными заголовками, возвращает строки диагностик.
std::vector<std::string> check(std::string const &readme, std::set<std::string, std::less<>> const &ignore = {}, std::string const &dir = {}) {
    SystemHeaders headers{{}};
    std::vector<std::string> out;
    for (auto const &d : check_names(parse_markdown(readme), sample(), headers, ignore, "README.md", dir)) {
        out.push_back(format_diagnostic(d));
    }
    return out;
}

} // namespace

/// Пути: с известным расширением или с существующим первым сегментом.
TEST(RuleNames, ClassifyPaths) {
    expect_class("src/session/session.cpp", SpanKind::PATH, "src/session/session.cpp");
    expect_class("./src/a.cpp", SpanKind::PATH, "src/a.cpp");
    {
        SCOPED_TRACE("первый сегмент src есть в проекте");
        expect_class("src/session/", SpanKind::PATH);
    }
    expect_class("src/app/server.*", SpanKind::PATH);
    {
        SCOPED_TRACE("номер строки отбрасывается");
        expect_class("src/a.cpp:42", SpanKind::PATH, "src/a.cpp");
    }
    expect_class("acme/cfg/config.hpp", SpanKind::PATH);
}

/// Не пути: слова через '/', каталоги сборки, URL, системные пути, плейсхолдеры, команды.
TEST(RuleNames, ClassifyNotPaths) {
    for (std::string_view text : {"TCP/UDP", "build/", "operator/", "https://x.org/a.md", "/etc/app/app.conf", "~/x.cpp", "/etc/app/<name>",
                                  "${DIR}/a.cpp", "monitor -f net", "-o file.txt", "validate/commit"}) {
        expect_class(text, SpanKind::NONE);
    }
}

/// Имя файла без каталога с известным расширением.
TEST(RuleNames, ClassifyFileNames) {
    expect_class("metrcs.cpp", SpanKind::FILE_NAME, "metrcs.cpp");
    expect_class("CMakeLists.txt", SpanKind::FILE_NAME);
    expect_class(".gitlab-ci.yml", SpanKind::FILE_NAME);
    {
        SCOPED_TRACE("собранные артефакты не проверяются");
        expect_class("libfoo.so", SpanKind::NONE);
    }
    expect_class("3.14", SpanKind::NONE);
}

/// Идентификаторы: snake_case, camelCase, квалифицированные имена и вызовы; проверяется последний компонент.
TEST(RuleNames, ClassifyIdentifiers) {
    expect_class("safe_invoke", SpanKind::IDENTIFIER, "safe_invoke");
    expect_class("safe_invoke()", SpanKind::IDENTIFIER, "safe_invoke");
    expect_class("getItems(string name)", SpanKind::IDENTIFIER, "getItems");
    expect_class("Session::close_failed()", SpanKind::IDENTIFIER, "close_failed");
    expect_class("core::io::WriteJsonNew", SpanKind::IDENTIFIER, "WriteJsonNew");
    expect_class("OrderParser", SpanKind::IDENTIFIER);
    {
        SCOPED_TRACE("квалифицированное имя проверяется даже из одного слова");
        expect_class("Session::abort", SpanKind::IDENTIFIER, "abort");
    }
    expect_class("init()", SpanKind::IDENTIFIER, "init");
}

/// Не идентификаторы: обычные слова, std::, значения, выражения.
TEST(RuleNames, ClassifyNotIdentifiers) {
    for (std::string_view text : {"validate", "Session", "monitor", "std::ranges::all_of", "READY=1", "rx-worker", "a + b", "x::", "()", "_"}) {
        expect_class(text, SpanKind::NONE);
    }
}

/// Устаревший README: описывает удалённые файлы и функции.
TEST(RuleNames, OutdatedReadme) {
    auto const out = check(
        "Обработчики в `src/session/session.cpp`.\n"
        "Всё обёрнуто в `safe_invoke()`, состояние в `get_session_state()`.\n"
        "Есть `list_sessions()` и `m_cache`, а также `on_save_done`.\n");
    std::vector<std::string> const expected{
        "README.md:1: ошибка: путь 'src/session/session.cpp' не найден в проекте [names]",
        "README.md:2: ошибка: имя 'get_session_state' (из 'get_session_state()') не найдено в коде проекта [names]",
        "README.md:2: ошибка: имя 'safe_invoke' (из 'safe_invoke()') не найдено в коде проекта [names]",
    };
    EXPECT_EQ(out, expected) << "существующие имена строки 3 не должны давать ошибок";
}

/// Существующие пути находятся всеми способами: точно, glob, суффикс, glob по суффиксу, от каталога README.
TEST(RuleNames, ExistingPaths) {
    EXPECT_TRUE(check("`src/main.cpp` `src/app/server.*` `acme/x.hpp` `acme/os/*.py` `src/app/` `build-deps.lst`\n").empty());
    EXPECT_TRUE(check("`app/server.cpp`\n", {}, "src").empty()) << "путь относительно каталога README src/";
}

/// Голое имя файла и ЗАГЛАВНОЕ имя дают предупреждение, а не ошибку.
TEST(RuleNames, Warnings) {
    auto const out = check("`Tracer.hpp` и `PATH_TO_DATA`\n");
    std::vector<std::string> const expected{
        "README.md:1: предупреждение: файл 'Tracer.hpp' не найден ни в проекте, ни в системных заголовках [names]",
        "README.md:1: предупреждение: имя 'PATH_TO_DATA' не найдено в коде проекта (переменная окружения или CMake?) [names]",
    };
    EXPECT_EQ(out, expected);
}

/// Повторное упоминание того же имени не даёт второй ошибки.
TEST(RuleNames, ReportOnce) {
    auto const out = check("`safe_invoke()`\n\n`safe_invoke`\n");
    ASSERT_EQ(out.size(), 1U);
    EXPECT_EQ(out[0].substr(0, 12), "README.md:1:") << "ошибка на первом упоминании";
}

/// Исключения сравниваются и с текстом span, и с найденным именем.
TEST(RuleNames, Ignore) {
    EXPECT_TRUE(check("`my_sax` `safe_invoke()` `src/old.cpp`\n", {"my_sax", "safe_invoke", "src/old.cpp"}).empty());
    EXPECT_TRUE(check("`safe_invoke()`\n", {"safe_invoke()"}).empty());
}

/// Файл исключений: комментарии, пустые строки и пробелы по краям.
TEST(RuleNames, LoadIgnore) {
    auto const path = std::filesystem::temp_directory_path() / "readme-lint-ignore-test";
    std::ofstream{path} << "# примеры из документации\nmy_sax\n\n  your_type  # тип пользователя\r\n";
    auto const ignore = load_ignore(path);
    std::filesystem::remove(path);
    EXPECT_EQ(ignore, (std::set<std::string, std::less<>>{"my_sax", "your_type"}));
    EXPECT_TRUE(load_ignore("/nonexistent/ignore").empty()) << "отсутствующий файл не ошибка";
}
