#include "rule_dbus.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr char const XML[] = R"(<?xml version="1.0"?>
<!DOCTYPE node PUBLIC "-//freedesktop//DTD D-BUS Object Introspection 1.0//EN" "">
<node name="/com/example/store">
  <!-- <interface name="com.example.commented"/> -->
  <interface name="com.example.store.query">
    <method name="getItems">
      <arg type="s" name="name" direction="in"/>
      <arg type="as" name="items" direction="out"/>
    </method>
    <method name="ping"/>
    <signal name="changed"><arg type="s" name="what"/></signal>
    <property name="Count" type="u" access="read"/>
  </interface>
  <interface name='com.example.store.control'>
    <method name="setLimits">
      <arg type="a{sas}" name="limits"/>
      <arg direction="out" type="s" name="result"/>
    </method>
  </interface>
</node>
)";

DbusModel xml_model() {
    DbusModel m;
    parse_introspection_xml(XML, "share/store.xml", m);
    return m;
}

Repo repo() { return Repo{{"src/main.cpp"}, "void registerMethod(); void reload_config();"}; }

std::vector<std::string> check(std::string const &readme, DbusModel const &model = xml_model(), std::vector<std::filesystem::path> const &dirs = {},
                               std::set<std::string, std::less<>> const &ignore = {}) {
    std::vector<std::string> out;
    for (auto const &d : check_dbus(readme, model, repo(), dirs, ignore, "README.md")) {
        out.push_back(format_diagnostic(d));
    }
    return out;
}

} // namespace

/// XML: методы, направления аргументов (по умолчанию in), свойства, комментарии и сигналы не путаются с методами.
TEST(RuleDbus, ParseXml) {
    auto const m = xml_model();
    ASSERT_EQ(m.interfaces.size(), 2U) << "интерфейс в комментарии не должен попасть в модель";
    auto const &q = m.interfaces.at("com.example.store.query");
    EXPECT_EQ(q.file, "share/store.xml");
    EXPECT_EQ(q.methods.at("getItems").in, "s");
    EXPECT_EQ(q.methods.at("getItems").out, "as");
    EXPECT_EQ(q.methods.at("ping").in, "") << "метод без аргументов";
    EXPECT_FALSE(q.methods.contains("changed")) << "сигнал не метод";
    EXPECT_EQ(q.properties.at("Count"), "u");
    EXPECT_EQ(m.interfaces.at("com.example.store.control").methods.at("setLimits").in, "a{sas}") << "direction по умолчанию in";
}

/// sd-bus: все варианты SD_BUS_METHOD, NULL как пустая сигнатура, склейка литералов.
TEST(RuleDbus, ParseVtables) {
    DbusModel m;
    parse_sdbus_vtables(R"(
static const sd_bus_vtable vt[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Reload", NULL, NULL, method_reload, 0),
    SD_BUS_METHOD_WITH_OFFSET("Get", "s", "a" "s", method_get, 8, 0),
    SD_BUS_METHOD_WITH_NAMES("Set", "su", SD_BUS_PARAM(key) SD_BUS_PARAM(value), "b", SD_BUS_PARAM(ok), method_set, 0),
    SD_BUS_METHOD_WITH_ARGS("Find", SD_BUS_ARGS("s", pattern, "u", limit), SD_BUS_RESULT("ao", paths), method_find, 0),
    SD_BUS_METHOD_WITH_ARGS("Stop", SD_BUS_NO_ARGS, SD_BUS_NO_RESULT, method_stop, 0),
    MY_SD_BUS_METHOD("Ignored", "s", "s", h, 0),
    SD_BUS_VTABLE_END
};)",
                        "src/bus.c", m);
    ASSERT_EQ(m.vtable_methods.size(), 5U) << "макрос с другим префиксом не должен считаться";
    auto const sig = [&](std::size_t i) { return m.vtable_methods[i].name + ":" + m.vtable_methods[i].sig.in + ">" + m.vtable_methods[i].sig.out; };
    EXPECT_EQ(sig(0), "Reload:>");
    EXPECT_EQ(sig(1), "Get:s>as");
    EXPECT_EQ(sig(2), "Set:su>b");
    EXPECT_EQ(sig(3), "Find:su>ao");
    EXPECT_EQ(sig(4), "Stop:>");
    EXPECT_EQ(m.vtable_methods[0].file, "src/bus.c");
}

/// busctl call: опции, кавычки, склейка строк, сигнатура; tree/introspect/emit не вызовы методов.
TEST(RuleDbus, FindBusctl) {
    auto const calls = find_dbus_calls(
        "busctl --system call com.example.store /com/example/store \\\n"
        "  com.example.store.query getItems 's' \"all items\"\n"
        "busctl -H host call svc /o com.example.store.query ping\n"
        "busctl introspect svc /o\n"
        "busctl emit /o com.example.x changed s v\n");
    ASSERT_EQ(calls.size(), 2U);
    EXPECT_EQ(calls[0].interface, "com.example.store.query");
    EXPECT_EQ(calls[0].method, "getItems");
    EXPECT_EQ(calls[0].signature, "s") << "кавычки снимаются";
    EXPECT_EQ(calls[0].line, 1U) << "строка начала склеенной команды";
    EXPECT_EQ(calls[1].method, "ping") << "значение -H не путается с подкомандой";
    EXPECT_FALSE(calls[1].has_signature);
    EXPECT_EQ(calls[1].line, 3U);
}

/// dbus-send: сигнатура вычисляется из аргументов, сигналы пропускаются.
TEST(RuleDbus, FindDbusSend) {
    auto const calls = find_dbus_calls(
        "`dbus-send --print-reply --dest=com.example.store /o com.example.store.control.setLimits dict:string:string:a,b int32:5 array:string:x,y "
        "variant:uint32:1`\n"
        "dbus-send --type=signal /o com.example.x.changed string:a\n"
        "dbus-send --system --type=method_call /o com.example.y.run blob\n");
    ASSERT_EQ(calls.size(), 2U);
    EXPECT_EQ(calls[0].interface, "com.example.store.control");
    EXPECT_EQ(calls[0].method, "setLimits");
    EXPECT_EQ(calls[0].signature, "a{ss}iasv");
    EXPECT_TRUE(calls[0].problem.empty());
    EXPECT_EQ(calls[1].method, "run");
    EXPECT_EQ(calls[1].problem, "аргумент dbus-send 'blob' не разобран, ожидается тип:значение");
}

/// Плейсхолдеры и операторы оболочки.
TEST(RuleDbus, FindSkipsPlaceholders) {
    EXPECT_TRUE(find_dbus_calls("busctl call <service> /o <interface> <method>\n").empty());
    auto const calls = find_dbus_calls("busctl call s /o com.example.a run | jq .\n");
    ASSERT_EQ(calls.size(), 1U);
    EXPECT_FALSE(calls[0].has_signature) << "| обрывает команду";
}

/// Совпадающие вызовы ошибок не дают.
TEST(RuleDbus, MatchingCalls) {
    EXPECT_TRUE(check("busctl call svc /o com.example.store.query getItems s x\n"
                      "busctl call svc /o com.example.store.query ping\n"
                      "dbus-send --print-reply /o com.example.store.query.getItems string:x\n")
                    .empty());
}

/// Ошибки по XML: другая сигнатура, вызов без аргументов, несуществующий метод.
TEST(RuleDbus, XmlErrors) {
    auto const out = check(
        "busctl call svc /o com.example.store.control setLimits s x\n"
        "busctl call svc /o com.example.store.query getItems\n"
        "busctl call svc /o com.example.store.query getAll\n");
    std::vector<std::string> const expected{
        "README.md:1: ошибка: сигнатура com.example.store.control.setLimits: в README 's', в share/store.xml 'a{sas}' [dbus]",
        "README.md:2: ошибка: busctl com.example.store.query.getItems вызывается без аргументов, а метод принимает 's' (share/store.xml) [dbus]",
        "README.md:3: ошибка: метод 'getAll' не найден в интерфейсе 'com.example.store.query' (share/store.xml; есть: getItems, ping) [dbus]",
    };
    EXPECT_EQ(out, expected);
}

/// Нет XML: сигнатура сверяется с таблицей sd-bus.
TEST(RuleDbus, VtableFallback) {
    DbusModel m;
    parse_sdbus_vtables(R"(SD_BUS_METHOD("Get", "s", "as", h, 0))", "src/bus.c", m);
    EXPECT_TRUE(check("busctl call svc /o com.example.vt Get s key\n", m).empty());
    auto const out = check("busctl call svc /o com.example.vt Get u 5\n", m);
    ASSERT_EQ(out.size(), 1U);
    EXPECT_EQ(out[0], "README.md:1: ошибка: сигнатура com.example.vt.Get: в README 'u', в SD_BUS_METHOD в src/bus.c 's' [dbus]");
}

/// Нет описаний: имя метода ищется в коде, org.freedesktop.* пропускаются, исключения работают.
TEST(RuleDbus, NameOnlyFallback) {
    DbusModel const empty;
    EXPECT_TRUE(check("busctl call svc /o com.example.app reload_config\n", empty).empty()) << "метод есть в коде";
    EXPECT_TRUE(check("busctl call org.freedesktop.systemd1 /o org.freedesktop.systemd1.Manager Reload\n", empty).empty());
    auto const out = check("busctl call svc /o com.example.app dropAll\n", empty);
    std::vector<std::string> const expected{
        "README.md:1: предупреждение: метод 'com.example.app.dropAll' не найден ни в описаниях D-Bus, ни в коде проекта [dbus]"};
    EXPECT_EQ(out, expected);
    EXPECT_TRUE(check("busctl call svc /o com.example.app dropAll\n", empty, {}, {"dropAll"}).empty());
    EXPECT_TRUE(check("busctl call svc /o com.example.app dropAll\n", empty, {}, {"com.example.app.dropAll"}).empty());
}

/// Интерфейс чужого сервиса берётся из системного каталога описаний ИНТЕРФЕЙС.xml.
TEST(RuleDbus, SystemInterfaces) {
    namespace fs = std::filesystem;
    auto const dir = fs::temp_directory_path() / ("readme-lint-dbus-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
    fs::create_directories(dir);
    std::ofstream{dir / "com.example.ext.xml"}
        << R"(<node><interface name="com.example.ext"><method name="Run"><arg type="u"/></method></interface></node>)";
    auto const out = check("busctl call ext /o com.example.ext Run s x\n", DbusModel{}, {dir});
    fs::remove_all(dir);
    ASSERT_EQ(out.size(), 1U);
    EXPECT_NE(out[0].find("в README 's'"), std::string::npos) << out[0];
    EXPECT_NE(out[0].find("'u'"), std::string::npos) << out[0];
}
