#include "rule_deps.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

/// Имена пакетов команды через пробел, версия через '='.
std::string names(AptCommand const &c) {
    std::string out;
    for (auto const &p : c.packages) {
        out += (out.empty() ? "" : " ") + p.name + (p.version.empty() ? "" : "=" + p.version);
    }
    return out;
}

std::vector<std::string> check(std::string const &readme, std::vector<DepSource> const &sources) {
    std::vector<std::string> out;
    for (auto const &d : check_deps(parse_markdown(readme), sources, "README.md")) {
        out.push_back(format_diagnostic(d));
    }
    return out;
}

DepSource lst(std::string const &text) { return parse_build_deps_lst(text, "build-deps.lst"); }

} // namespace

/// Простая команда: sudo, опции и версии.
TEST(RuleDeps, FindSimpleCommand) {
    auto const cmds = find_apt_commands("sudo apt install -y cmake libfoo-dev=2.1.* libbar:amd64\n", 10);
    ASSERT_EQ(cmds.size(), 1U);
    EXPECT_EQ(names(cmds[0]), "cmake libfoo-dev=2.1.* libbar") << "архитектура отбрасывается, шаблон версии сохраняется";
    EXPECT_EQ(cmds[0].line, 10U);
}

/// Склейка строк по '\' и номер строки начала команды.
TEST(RuleDeps, FindContinuation) {
    auto const cmds = find_apt_commands("echo start\napt-get install -y --no-install-recommends \\\n  cmake \\\n  libfoo-dev\nmake\n", 5);
    ASSERT_EQ(cmds.size(), 1U);
    EXPECT_EQ(names(cmds[0]), "cmake libfoo-dev");
    EXPECT_EQ(cmds[0].line, 6U) << "строка с apt-get";
}

/// Команда обрывается на операторах оболочки и комментарии, вторая команда в строке тоже находится.
TEST(RuleDeps, FindTerminators) {
    auto const cmds = find_apt_commands("apt install a-pkg b-pkg && apt install c-pkg; make # apt install d-pkg\n", 1);
    ASSERT_EQ(cmds.size(), 2U);
    EXPECT_EQ(names(cmds[0]), "a-pkg b-pkg");
    EXPECT_EQ(names(cmds[1]), "c-pkg") << "';' в конце токена обрывает команду";
}

/// Не пакеты: установка собранного .deb, подстановки, xargs из файла, не install.
TEST(RuleDeps, FindSkipsNonPackages) {
    EXPECT_TRUE(find_apt_commands("apt install -y ./tool_<версия>_amd64.deb\n", 1).empty());
    EXPECT_TRUE(find_apt_commands("xargs apt install -y < <(envsubst deps.lst)\n", 1).empty());
    EXPECT_TRUE(find_apt_commands("apt install $PKGS\n", 1).empty());
    EXPECT_TRUE(find_apt_commands("apt update && apt upgrade -y\n", 1).empty());
    EXPECT_TRUE(find_apt_commands("apt install Пакет\n", 1).empty()) << "имя пакета Debian только из строчных латинских букв, цифр и + - .";
}

/// build-deps.lst: комментарии, пустые строки, строки с переменными пропускаются.
TEST(RuleDeps, ParseBuildDepsLst) {
    auto const src = lst("# сборка\ncmake\nlibfoo-dev=2.1.*\n\nlibbar-dev=${BAR_VERSION}\nninja-build  # генератор\n");
    EXPECT_TRUE(src.complete);
    EXPECT_EQ(src.packages, (std::map<std::string, std::string>{{"cmake", ""}, {"libfoo-dev", "2.1.*"}, {"ninja-build", ""}}));
}

/// debian/control: многострочное поле, альтернативы, ограничения версий, подстановки и архитектуры.
TEST(RuleDeps, ParseDebianControl) {
    auto const src = parse_debian_control(
        "Source: tool\n"
        "Build-Depends: debhelper-compat (= 13),\n"
        " cmake (>= 3.25),\n"
        " libfoo-dev | libfoo2-dev,\n"
        " libbar-dev [amd64] <!nocheck>,\n"
        " ${misc:Depends}\n"
        "Build-Depends-Indep: python3\n"
        "\n"
        "Package: tool\n"
        "Depends: libruntime1\n",
        "debian/control");
    EXPECT_EQ(src.packages,
              (std::map<std::string, std::string>{
                  {"cmake", ""}, {"debhelper-compat", "13"}, {"libbar-dev", ""}, {"libfoo-dev", ""}, {"libfoo2-dev", ""}, {"python3", ""}}))
        << "Depends пакета не сборочные и не должны попасть в список";
}

/// Файл CI: пакеты из apt-get install, источник неполный.
TEST(RuleDeps, ParseCiFile) {
    auto const src =
        parse_ci_file("      - run: |\n          apt-get update\n          apt-get install -y \\\n            git cmake libfoo-dev\n", "ci.yml");
    EXPECT_FALSE(src.complete);
    EXPECT_EQ(src.packages.size(), 3U);
}

/// Версия в README отличается от зафиксированной в источнике: ошибка.
TEST(RuleDeps, VersionMismatch) {
    auto const out = check("```bash\napt install -y cmake libfoo-dev=2.1.* \\\n  libbar-dev\n```\n", {lst("cmake\nlibfoo-dev=2.2.*\nlibbar-dev\n")});
    std::vector<std::string> const expected{"README.md:2: ошибка: версия пакета 'libfoo-dev': в README '=2.1.*', в build-deps.lst '=2.2.*' [deps]"};
    EXPECT_EQ(out, expected);
}

/// Пакет README отсутствует в источниках, а пакет источника отсутствует в README: предупреждения.
TEST(RuleDeps, MissingBothWays) {
    auto const out = check("```\napt install cmake libfoo-dev libold-dev\n```\n", {lst("cmake\nlibfoo-dev\nlibnew-dev\n")});
    std::vector<std::string> const expected{
        "README.md:2: предупреждение: пакет 'libold-dev' не найден в build-deps.lst [deps]",
        "README.md:2: предупреждение: пакет 'libnew-dev' из build-deps.lst не указан в команде установки [deps]",
    };
    EXPECT_EQ(out, expected);
}

/// Команда без сборочных пакетов считается установкой зависимостей для запуска и не проверяется.
TEST(RuleDeps, RuntimeCommandIgnored) {
    EXPECT_TRUE(check("```bash\nsudo apt install python3-requests\n```\n", {lst("cmake\nlibfoo-dev\n")}).empty());
}

/// Команда покрывает меньше половины источника: недостающие пакеты не сообщаются.
TEST(RuleDeps, PartialCommandNoMissing) {
    EXPECT_TRUE(check("`apt install cmake`\n", {lst("cmake\nlibfoo-dev\nlibbar-dev\n")}).empty()) << "1 из 3 меньше половины";
}

/// Неполный источник (CI) не требует, чтобы его служебные пакеты были в README.
TEST(RuleDeps, IncompleteSourceNoMissing) {
    auto const ci = parse_ci_file("apt-get install -y git ca-certificates cmake libfoo-dev\n", "ci.yml");
    EXPECT_TRUE(check("```sh\napt install cmake libfoo-dev\n```\n", {ci}).empty());
}

/// Блоки других языков и отсутствие источников не проверяются.
TEST(RuleDeps, SkipNonShellAndNoSources) {
    EXPECT_TRUE(check("```cpp\n// apt install libold-dev cmake\n```\n", {lst("cmake\n")}).empty());
    EXPECT_TRUE(check("```bash\napt install cmake libold-dev\n```\n", {}).empty());
}

/// Загрузка источников из каталога проекта.
TEST(RuleDeps, LoadSources) {
    namespace fs = std::filesystem;
    auto const root = fs::temp_directory_path() / ("readme-lint-deps-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
    fs::create_directories(root / "debian");
    fs::create_directories(root / ".github/workflows");
    std::ofstream{root / "build-deps.lst"} << "cmake\n";
    std::ofstream{root / "debian/control"} << "Build-Depends: libfoo-dev\n";
    std::ofstream{root / ".github/workflows/b.yml"} << "run: apt-get install -y git\n";
    std::ofstream{root / ".github/workflows/a.yaml"} << "run: echo nothing\n";
    auto const src = load_dep_sources(root);
    fs::remove_all(root);
    ASSERT_EQ(src.size(), 3U) << "CI без apt install не считается источником";
    EXPECT_EQ(src[0].file, "build-deps.lst");
    EXPECT_EQ(src[1].file, "debian/control");
    EXPECT_EQ(src[2].file, ".github/workflows/b.yml");
}
