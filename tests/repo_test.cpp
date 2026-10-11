#include "repo.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_set>

namespace fs = std::filesystem;

namespace {

Repo sample() {
    return Repo{{"src/app/main.cpp", "src/app/server.hpp", "lib/include/acme/cfg/config.hpp", "bin/deploy_tool", "README.md"}, "int get_value();"};
}

/// Временный каталог, удаляется в деструкторе.
struct TempDir {
    fs::path path = fs::temp_directory_path() / ("readme-lint-test-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "-" +
                                                 ::testing::UnitTest::GetInstance()->current_test_info()->name());
    TempDir() { fs::create_directories(path); }
    ~TempDir() { fs::remove_all(path); }
    void write(std::string const &rel, std::string const &text) const {
        fs::create_directories((path / rel).parent_path());
        std::ofstream{path / rel} << text;
    }
};

} // namespace

/// Файлы и каталоги ищутся по точному пути, "./" в начале и "/" в конце игнорируются.
TEST(Repo, HasPath) {
    auto const r = sample();
    EXPECT_TRUE(r.has_path("src/app/main.cpp"));
    EXPECT_TRUE(r.has_path("./src/app/main.cpp"));
    EXPECT_TRUE(r.has_path("src/app")) << "каталог с файлами считается существующим";
    EXPECT_TRUE(r.has_path("src/app/"));
    EXPECT_FALSE(r.has_path("src/session/session.cpp"));
    EXPECT_FALSE(r.has_path("src/ap")) << "префикс имени каталога не является каталогом";
}

/// Путь от include-каталога внутри проекта находится по концу пути, но только по целым сегментам.
TEST(Repo, HasPathSuffix) {
    auto const r = sample();
    EXPECT_TRUE(r.has_path_suffix("acme/cfg/config.hpp"));
    EXPECT_FALSE(r.has_path_suffix("fg/config.hpp")) << "совпадение должно начинаться с границы сегмента";
}

/// Glob работает в пределах сегмента, суффиксный glob ищет шаблон в конце пути.
TEST(Repo, Glob) {
    auto const r = sample();
    EXPECT_TRUE(r.matches_glob("src/app/server.*"));
    EXPECT_FALSE(r.matches_glob("src/*.cpp")) << "'*' не должна переходить через '/'";
    EXPECT_TRUE(r.matches_glob_suffix("app/*.cpp"));
    EXPECT_FALSE(r.matches_glob_suffix("os/*.py"));
}

/// Имя файла, каталог верхнего уровня и слова кода.
TEST(Repo, NamesAndWords) {
    auto const r = sample();
    EXPECT_TRUE(r.has_basename("config.hpp"));
    EXPECT_FALSE(r.has_basename("session.cpp"));
    EXPECT_TRUE(r.has_top_level("lib"));
    EXPECT_TRUE(r.has_top_level("README.md"));
    EXPECT_FALSE(r.has_top_level("include")) << "вложенный каталог не является каталогом верхнего уровня";
    EXPECT_TRUE(r.has_word("get_value"));
    EXPECT_FALSE(r.has_word("get_val")) << "слово ищется целиком";
    EXPECT_TRUE(r.has_word("deploy_tool")) << "имя программы без расширения считается словом проекта";
    EXPECT_TRUE(r.has_word("main")) << "имя файла без расширения считается словом проекта";
}

/// Код и документация: в документации слова не ищутся.
TEST(Repo, IsCodeFile) {
    for (auto const *f : {"a.cpp", "x/y.hpp", "CMakeLists.txt", "cmake/f.cmake", "share/i.xml", "m.yang", "s.py", "build-deps.lst"}) {
        EXPECT_TRUE(is_code_file(f)) << f;
    }
    for (auto const *f : {"README.md", "design/a.puml", "whatsnew.txt", "img/a.png"}) {
        EXPECT_FALSE(is_code_file(f)) << f;
    }
}

/// Идентификаторы выделяются целиком, числа и части слов не считаются.
TEST(Repo, CollectWords) {
    std::unordered_set<std::string> w;
    collect_words("a_b(1, x2); 3abc ::ns::Name", w);
    EXPECT_TRUE(w.contains("a_b"));
    EXPECT_TRUE(w.contains("x2"));
    EXPECT_TRUE(w.contains("Name"));
    EXPECT_FALSE(w.contains("abc")) << "'3abc' не идентификатор, его хвост не должен считаться словом";
}

/// Вне git проект обходится по каталогу: каталоги сборки и скрытые пропускаются, скрипты без расширения читаются.
TEST(Repo, WalkWithoutGit) {
    TempDir const t;
    t.write("src/a.cpp", "void real_func();");
    t.write("build/gen.cpp", "void generated_func();");
    t.write(".hidden/h.cpp", "void hidden_func();");
    t.write("bin/tool", "#!/bin/sh\nexport TOOL_VAR=1\n");
    t.write("data/blob", "BLOB_WORD");
    Repo const r{t.path};
    EXPECT_TRUE(r.has_path("src/a.cpp"));
    EXPECT_TRUE(r.has_word("real_func"));
    EXPECT_FALSE(r.has_path("build/gen.cpp")) << "каталог build не часть проекта";
    EXPECT_FALSE(r.has_word("generated_func"));
    EXPECT_FALSE(r.has_word("hidden_func"));
    EXPECT_TRUE(r.has_word("TOOL_VAR")) << "скрипт без расширения с #! считается кодом";
    EXPECT_FALSE(r.has_word("BLOB_WORD")) << "файл без расширения и без #! кодом не считается";
}

/// Системные заголовки: путь, имя файла и поиск слов целиком.
TEST(SystemHeaders, FindInDirs) {
    TempDir const t;
    t.write("vendor/vendor_plugin.h", "int vnd_hook_failed; int vnd_hook_end_x;");
    SystemHeaders h{{t.path}};
    EXPECT_TRUE(h.has_path("vendor/vendor_plugin.h"));
    EXPECT_FALSE(h.has_path("vendor_plugin.h"));
    EXPECT_TRUE(h.has_basename("vendor_plugin.h"));
    auto const found = h.find_words({"vnd_hook_failed", "vnd_hook_end", "missing_name"});
    EXPECT_EQ(found, (std::set<std::string>{"vnd_hook_failed"})) << "vnd_hook_end встречается только как часть vnd_hook_end_x";
}
