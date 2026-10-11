#include "diagnostic.hpp"

#include <fmt/format.h>

std::string format_diagnostic(Diagnostic const &d) {
    auto const level = d.severity == Severity::ERROR ? "ошибка" : "предупреждение";
    return fmt::format("{}:{}: {}: {} [{}]", d.file, d.line, level, d.message, d.rule);
}
