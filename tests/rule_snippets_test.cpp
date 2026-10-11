#include "rule_snippets.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

CodeBlock block(std::string lang, std::string text, std::size_t line = 10) { return {std::move(lang), std::move(text), line, line + 1}; }

/// Временный проект с заголовком в lib/include/acme/calc.hpp.
struct Project {
    fs::path root = fs::temp_directory_path() / ("readme-lint-snip-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "-" +
                                                 ::testing::UnitTest::GetInstance()->current_test_info()->name());
    Project() {
        fs::create_directories(root / "lib/include/acme");
        std::ofstream{root / "lib/include/acme/calc.hpp"}
            << "#pragma once\nnamespace acme {\ninline int add(int a, int b) { return a + b; }\n"
               "template <typename T> struct Box;\ntemplate <typename T> int unbox() { Box<T> b; return 0; }\n}\n";
        std::ofstream{root / "CMakeLists.txt"} << "project(calc)\nset(CMAKE_CXX_STANDARD 17)\n";
    }
    ~Project() { fs::remove_all(root); }
    [[nodiscard]] std::vector<std::string> check(std::string const &readme) const {
        Repo const repo{root};
        auto const cfg = snippet_config(root, repo, "");
        std::vector<std::string> out;
        for (auto const &d : check_snippets(parse_markdown(readme), readme, cfg, "README.md")) {
            out.push_back(format_diagnostic(d));
        }
        return out;
    }
};

} // namespace

/// Какие блоки проверяются: язык C/C++, есть #include или main, нет пометок и пропусков.
TEST(RuleSnippets, SkipReason) {
    EXPECT_FALSE(skip_reason(block("cpp", "#include <vector>\n"), "```cpp"));
    EXPECT_FALSE(skip_reason(block("c", "int main(void) { return 0; }\n"), "```c"));
    EXPECT_TRUE(skip_reason(block("bash", "#include\n"), "```bash"));
    EXPECT_TRUE(skip_reason(block("cpp", "auto x = f();\n"), "```cpp")) << "фрагмент без #include и main";
    EXPECT_TRUE(skip_reason(block("cpp", "#include <a.h>\n"), "```cpp ignore"));
    EXPECT_TRUE(skip_reason(block("cpp", "#include <a.h>\nvoid f() { ... }\n"), "```cpp"));
    EXPECT_TRUE(skip_reason(block("cpp", "#include <a.h>\n...\n"), "```cpp"));
    EXPECT_FALSE(skip_reason(block("cpp", "#include <a.h>\ntemplate <typename... Ts> void f(Ts... ts);\n"), "```cpp"))
        << "variadic-шаблоны не пропуски";
}

/// Обёртка: директивы и using снаружи, остальное в main, строки соответствуют README.
TEST(RuleSnippets, Wrapped) {
    auto const src = wrapped(block("cpp", "#include <cstdio>\nusing namespace std;\nputs(\"x\");\n", 20));
    EXPECT_EQ(src.text, "#include <cstdio>\nusing namespace std;\nint main() {\nputs(\"x\");\n}\n");
    EXPECT_EQ(src.line_map, (std::vector<std::size_t>{21, 22, 0, 23, 0}));
}

/// Разбор вывода компилятора: строка ошибки в примере, ошибка в заголовке, отсутствующий заголовок.
TEST(RuleSnippets, ParseOutput) {
    auto src = as_is(block("cpp", "#include <x>\nint a = b;\n", 30));
    src.file_name = "/tmp/s0.cpp";
    auto const in_snippet = parse_compiler_output("/tmp/s0.cpp:2:9: error: 'b' was not declared in this scope\n", src);
    EXPECT_EQ(in_snippet.line, 32U);
    EXPECT_EQ(in_snippet.message, "'b' was not declared in this scope");
    EXPECT_FALSE(in_snippet.missing_header);
    auto const in_header = parse_compiler_output(
        "In file included from /tmp/s0.cpp:1:\n/usr/include/x.h:5:1: error: boom\n"
        "/tmp/s0.cpp:2:5:   required from here\n",
        src);
    EXPECT_EQ(in_header.line, 32U) << "строка из required from here, а не номер строки заголовка";
    auto const missing = parse_compiler_output("/tmp/s0.cpp:1:10: fatal error: x: No such file or directory\n", src);
    EXPECT_TRUE(missing.missing_header);
    EXPECT_EQ(missing.line, 31U);
}

/// Настройки: стандарт из CMake и каталог include проекта.
TEST(RuleSnippets, Config) {
    Project const p;
    Repo const repo{p.root};
    auto const cfg = snippet_config(p.root, repo, "my-cxx");
    EXPECT_EQ(cfg.compiler, "my-cxx");
    EXPECT_EQ(cfg.cxx_std, "c++17");
    EXPECT_NE(std::find(cfg.include_dirs.begin(), cfg.include_dirs.end(), fs::absolute(p.root / "lib/include")), cfg.include_dirs.end());
}

/// Рабочие примеры (с main, без main, на C) ошибок не дают.
TEST(RuleSnippets, GoodSnippets) {
    Project const p;
    EXPECT_TRUE(p.check("```cpp\n#include <acme/calc.hpp>\nint main() { return acme::add(1, 2); }\n```\n"
                        "```C++\n#include <acme/calc.hpp>\nint x = acme::add(1, 2);\n(void)x;\n```\n"
                        "```c\n#include <stdio.h>\nint main(void) { puts(\"x\"); return 0; }\n```\n"
                        "```cpp\nauto x = not_compiled();\n```\n")
                    .empty());
}

/// Ошибка в примере указывает на строку README; ошибка в заголовке на место вызова; нет заголовка: предупреждение.
TEST(RuleSnippets, BadSnippets) {
    Project const p;
    auto const out = p.check(
        "# T\n"
        "```cpp\n#include <acme/calc.hpp>\nint main() {\n    return acme::sub(1, 2);\n}\n```\n"
        "```cpp\n#include <acme/calc.hpp>\nint main() {\n    return acme::unbox<int>();\n}\n```\n"
        "```cpp\n#include <acme/missing.hpp>\nint main() {}\n```\n");
    ASSERT_EQ(out.size(), 3U) << (out.empty() ? "" : out[0]);
    EXPECT_TRUE(out[0].starts_with("README.md:5: ошибка: пример не компилируется: ")) << out[0];
    EXPECT_NE(out[0].find("sub"), std::string::npos) << out[0];
    EXPECT_TRUE(out[1].starts_with("README.md:11: ошибка: пример не компилируется: ")) << "строка вызова unbox<int>: " << out[1];
    EXPECT_EQ(out[2], "README.md:15: предупреждение: пример не проверен: acme/missing.hpp: No such file or directory [snippets]");
}

/// Компилятор не запускается: одно предупреждение.
TEST(RuleSnippets, NoCompiler) {
    Project const p;
    Repo const repo{p.root};
    auto const cfg = snippet_config(p.root, repo, "/nonexistent/c++");
    auto const readme = std::string{"```cpp\n#include <a>\nint main(){}\n```\n```cpp\n#include <b>\nint main(){}\n```\n"};
    auto const out = check_snippets(parse_markdown(readme), readme, cfg, "README.md");
    ASSERT_EQ(out.size(), 1U);
    EXPECT_EQ(out[0].message, "компилятор '/nonexistent/c++' не запускается");
    EXPECT_EQ(out[0].severity, Severity::WARNING);
}
