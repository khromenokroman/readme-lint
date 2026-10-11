#include "rule_options.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using Names = std::set<std::string, std::less<>>;

CliOptions parse(std::string_view code, bool python = false) {
    CliOptions o;
    parse_cli_options(code, python, o);
    return o;
}

std::vector<std::string> mentions(std::string const &readme, Names const &programs = {"tool"}) {
    std::vector<std::string> out;
    for (auto const &m : find_readme_options(readme, parse_markdown(readme), programs)) {
        out.push_back(std::to_string(m.line) + ":" + m.option);
    }
    return out;
}

std::vector<std::string> check(std::string const &readme, CliOptions const &code, Names const &ignore = {}) {
    std::vector<std::string> out;
    for (auto const &d : check_options(find_readme_options(readme, parse_markdown(readme), {"tool"}), code, ignore, "README.md")) {
        out.push_back(format_diagnostic(d));
    }
    return out;
}

/// Код утилиты с getopt_long: строка ключей и массив struct option с парами.
constexpr char const GETOPT_CODE[] = R"(
int main(int argc, char **argv) {
    static struct ::option opts[] = {
        {"help", no_argument, nullptr, 'h'},
        {"output", required_argument, nullptr, 'o'},
        {"check", optional_argument, nullptr, 'c'},
        {"dry-run", no_argument, nullptr, 1000},
        {nullptr, 0, nullptr, 0},
    };
    while ((c = getopt_long(argc, argv, "+ho:c::x", opts, nullptr)) != -1) {}
}
)";

} // namespace

/// getopt_long: короткие ключи из строки, длинные и пары из struct option (в том числе struct ::option).
TEST(RuleOptions, ParseGetopt) {
    auto const o = parse(GETOPT_CODE);
    EXPECT_EQ(o.declared, (Names{"--check", "--dry-run", "--help", "--output", "-c", "-h", "-o", "-x"}));
    EXPECT_EQ(o.pairs.at("-o"), "--output");
    EXPECT_EQ(o.pairs.at("--output"), "-o");
    EXPECT_FALSE(o.pairs.contains("--dry-run")) << "числовое значение val не короткий ключ";
}

/// Ручной разбор: литералы в сравнениях объявляют ключи, пара берётся из одной строки.
TEST(RuleOptions, ParseManual) {
    auto const o = parse(R"(
Options parse(int argc, char **argv) {
    if (arg == "-f" || arg == "--file") {}
    else if (std::strcmp(argv[i], "--dump") == 0) {}
    run_child({"ip", "-4", "route"});
}
)");
    EXPECT_EQ(o.declared, (Names{"--dump", "--file", "-f"}));
    EXPECT_TRUE(o.known.contains("-4")) << "литерал без сравнения известен, но не объявлен";
    EXPECT_EQ(o.pairs.at("-f"), "--file");
}

/// Сравнения в файле, который не разбирает аргументы, ключей не объявляют.
TEST(RuleOptions, ComparisonsOutsideCliFile) {
    auto const o = parse(R"(bool other_tool(std::string const &t) { return t == "-H" || t == "--host"; })");
    EXPECT_TRUE(o.declared.empty());
    EXPECT_TRUE(o.known.contains("--host"));
}

/// Ключи в комментариях C/C++ и Python не считаются, литералы с // и # внутри не обрезаются.
TEST(RuleOptions, IgnoreComments) {
    auto const c = parse(
        "int main(int argc, char** argv) {\n"
        "    // if (a == \"--old\") {}\n"
        "    /* arg == \"-z\" */ if (a == \"--url\" || u == \"http://x\") {}\n"
        "}\n");
    EXPECT_EQ(c.declared, (Names{"--url"}));
    auto const py = parse("p.add_argument('--real')  # p.add_argument('--old')\np.add_argument('--hash', default='#')\n", true);
    EXPECT_EQ(py.declared, (Names{"--hash", "--real"}));
}

/// boost::program_options / cxxopts и CLI11 / argparse.
TEST(RuleOptions, ParseLibraries) {
    auto const boost = parse(R"(int main(int argc, char** argv) { desc.add_options()("help,h", "справка")("level", po::value<int>(), "уровень"); })");
    EXPECT_EQ(boost.declared, (Names{"--help", "--level", "-h"}));
    EXPECT_EQ(boost.pairs.at("-h"), "--help");
    auto const cli11 = parse(R"(int main(int argc, char** argv) { app.add_option("-n,--count", n); app.add_flag("--quiet"); })");
    EXPECT_EQ(cli11.declared, (Names{"--count", "--quiet", "-n"}));
    auto const py = parse("p = argparse.ArgumentParser()\np.add_argument('-v', '--verbose')\n", true);
    EXPECT_EQ(py.declared, (Names{"--verbose", "-v"}));
    EXPECT_EQ(py.pairs.at("--verbose"), "-v");
}

/// Мусор не считается ключом: заглавные длинные, тройной дефис, add_options в Python.
TEST(RuleOptions, RejectNonOptions) {
    auto const o = parse(R"(int main(int argc, char** argv) { if (a == "--DOT" || a == "---" || a == "--_x" || a == "-") {} })");
    EXPECT_TRUE(o.declared.empty());
    EXPECT_TRUE(parse("self.add_options()('_I_')\n", true).declared.empty());
}

/// README: таблица, справка, жирный ключ, список, inline-код, команды программы.
TEST(RuleOptions, FindInReadme) {
    auto const m = mentions(
        "| `-f`, `--file ФАЙЛ` | файл |\n"
        "  -o, --output=PATH   куда писать\n"
        "**-b** _size_  \n"
        "- `--dry-run` ничего не менять\n"
        "Ключ `-x` и `--level <N>` в тексте.\n"
        "$ sudo ./build/tool -q --mode=fast | grep x --color\n");
    std::vector<std::string> const expected{"1:-f", "1:--file", "2:-o", "2:--output", "3:-b", "4:--dry-run", "5:-x", "5:--level", "6:-q", "6:--mode"};
    EXPECT_EQ(m, expected) << "--color принадлежит grep, а не программе";
}

/// Ключи чужих программ и имя программы в прозе не берутся.
TEST(RuleOptions, IgnoreForeignOptions) {
    EXPECT_TRUE(mentions("Собрать: `cmake --build . --target doc`.\n"
                         "Документация tool собирается через cmake --build.\n"
                         "brew install tool --HEAD\n"
                         "apt install -y libfoo-dev\n"
                         "- пункт списка -- с тире\n"
                         "| --- | --- |\n")
                    .empty());
}

/// Ключ README, которого нет в коде: ошибка.
TEST(RuleOptions, DocumentedButMissing) {
    auto const out = check("| `-o`, `--output` | |\n| `--force` | |\n", parse(GETOPT_CODE));
    ASSERT_FALSE(out.empty());
    EXPECT_EQ(out[0], "README.md:2: ошибка: ключ '--force' описан в README, но не найден в коде [options]");
}

/// Неописанные ключи: пара одной строкой, справка не требуется, порог описанности.
TEST(RuleOptions, Undocumented) {
    auto const code = parse(GETOPT_CODE);
    auto const out = check("| `-o`, `--output` | |\n| `--dry-run` | |\n", code);
    std::vector<std::string> const expected{
        "README.md:1: предупреждение: ключ '-c, --check' есть в коде, но не описан в README [options]",
        "README.md:1: предупреждение: ключ '-x' есть в коде, но не описан в README [options]",
    };
    EXPECT_EQ(out, expected) << "--help и -h не требуются, пара -c/--check одной строкой";
    EXPECT_TRUE(check("| `-o` | |\n", code).empty()) << "описан один ключ из 8: README не описывает CLI, неописанные не сообщаются";
    EXPECT_TRUE(check("| `-o` | |\n| `--dry-run` | |\n", code, {"-x", "--check"}).empty()) << "исключения";
}

/// Без ключей в коде правило молчит.
TEST(RuleOptions, NoCodeOptions) { EXPECT_TRUE(check("| `--anything` | |\n", CliOptions{}).empty()); }

/// Вспомогательные каталоги не участвуют, имена программ из CMake.
TEST(RuleOptions, ProgramNamesAndAuxiliary) {
    namespace fs = std::filesystem;
    auto const root = fs::temp_directory_path() / ("readme-lint-opt-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed())) / "proj";
    fs::create_directories(root / "bench");
    fs::create_directories(root / "bin");
    std::ofstream{root / "CMakeLists.txt"}
        << "project(tool\n  VERSION 1.0)\nadd_executable(${PROJECT_NAME}-cli main.cpp)\nadd_executable(helper h.cpp)\n";
    std::ofstream{root / "main.cpp"} << "int main(int argc, char** argv) { getopt(argc, argv, \"ab\"); }";
    std::ofstream{root / "bench/b.c"} << "int main(int argc, char** argv) { getopt(argc, argv, \"lC\"); }";
    std::ofstream{root / "bin/deploy"} << "#!/bin/sh\n";
    Repo const repo{root};
    auto const names = program_names(root, repo);
    auto const opts = load_cli_options(root, repo);
    fs::remove_all(root.parent_path());
    EXPECT_EQ(names, (Names{"deploy", "helper", "proj", "tool", "tool-cli"}));
    EXPECT_EQ(opts.declared, (Names{"-a", "-b"})) << "ключи бенчмарка не относятся к программе";
}
