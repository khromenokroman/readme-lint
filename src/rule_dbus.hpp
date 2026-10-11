#pragma once
#include <cstddef>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "diagnostic.hpp"
#include "repo.hpp"

/**
 * @brief Метод D-Bus с сигнатурами входных и выходных аргументов.
 */
struct DbusMethod {
    std::string in;  ///< сигнатура входных аргументов, например "sas"
    std::string out; ///< сигнатура выходных аргументов
};

/**
 * @brief Интерфейс D-Bus из XML-описания.
 */
struct DbusInterface {
    std::map<std::string, DbusMethod> methods;     ///< методы по имени
    std::map<std::string, std::string> properties; ///< свойства: имя -> тип
    std::string file;                              ///< файл описания, для сообщений
};

/**
 * @brief Метод из таблицы sd-bus (SD_BUS_METHOD), интерфейс в таблице не указан.
 */
struct VtableMethod {
    std::string name; ///< имя метода
    std::string file; ///< файл с таблицей, для сообщений
    DbusMethod sig;   ///< сигнатуры
};

/**
 * @brief Всё, что известно о D-Bus интерфейсах проекта.
 */
struct DbusModel {
    std::map<std::string, DbusInterface> interfaces; ///< интерфейсы из XML по имени
    std::vector<VtableMethod> vtable_methods;        ///< методы из таблиц sd-bus
};

/**
 * @brief Вызов D-Bus метода из README.
 */
struct DbusCall {
    std::string tool;           ///< "busctl" или "dbus-send"
    std::string interface;      ///< имя интерфейса
    std::string method;         ///< имя метода
    std::string signature{};    ///< сигнатура аргументов из README
    std::string problem{};      ///< непустая: аргументы не разобраны, текст причины
    std::size_t line{};         ///< строка README, с 1
    bool has_signature = false; ///< сигнатура указана (busctl) или вычислена (dbus-send)
};

/**
 * @brief Разбирает XML-описание (introspection) и добавляет интерфейсы в модель.
 * @param xml Текст файла.
 * @param file Имя файла для сообщений.
 * @param model Модель, в которую добавляются интерфейсы.
 */
void parse_introspection_xml(std::string_view xml, std::string const &file, DbusModel &model);

/**
 * @brief Находит в коде C/C++ методы таблиц sd-bus: SD_BUS_METHOD и SD_BUS_METHOD_WITH_NAMES и т.п.
 */
void parse_sdbus_vtables(std::string_view code, std::string const &file, DbusModel &model);

/**
 * @brief Загружает модель: XML-описания из проекта и sd-bus таблицы из кода.
 * @param root Корень проекта.
 * @param repo Список файлов проекта.
 */
[[nodiscard]] DbusModel load_dbus_model(std::filesystem::path const &root, Repo const &repo);

/**
 * @brief Находит вызовы busctl call и dbus-send (method_call) во всех строках текста.
 *
 * Строки с '\' в конце склеиваются, кавычки снимаются, обратные кавычки inline-кода игнорируются.
 * Для dbus-send сигнатура вычисляется из аргументов вида тип:значение.
 */
[[nodiscard]] std::vector<DbusCall> find_dbus_calls(std::string_view text);

/**
 * @brief Правило dbus: вызовы из README согласованы с интерфейсами проекта.
 *
 * Интерфейс ищется в XML проекта, затем в system_dirs (файл ИНТЕРФЕЙС.xml). Найден: метода нет или
 * сигнатура отличается: ошибка. Не найден, но метод есть в таблице sd-bus: сверяется сигнатура.
 * Иначе (кроме org.freedesktop.*) имя метода должно встречаться в коде, иначе предупреждение.
 * @param readme Текст README.
 * @param model Модель интерфейсов проекта.
 * @param repo Проект, для поиска имени метода в коде.
 * @param system_dirs Каталоги системных описаний, например /usr/share/dbus-1/interfaces.
 * @param ignore Исключения: сравниваются с именем метода и с "интерфейс.метод".
 * @param file Имя README для сообщений.
 */
[[nodiscard]] std::vector<Diagnostic> check_dbus(std::string_view readme, DbusModel const &model, Repo const &repo,
                                                 std::vector<std::filesystem::path> const &system_dirs,
                                                 std::set<std::string, std::less<>> const &ignore, std::string const &file);
