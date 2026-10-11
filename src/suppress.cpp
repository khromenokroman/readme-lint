#include "suppress.hpp"

#include <algorithm>
#include <limits>

namespace {

constexpr std::string_view DISABLE = "readme-lint-disable";
constexpr std::string_view ENABLE = "readme-lint-enable";

std::string_view trim(std::string_view s) {
    auto const b = s.find_first_not_of(" \t\r");
    if (b == std::string_view::npos) {
        return {};
    }
    return s.substr(b, s.find_last_not_of(" \t\r") - b + 1);
}

std::vector<std::string> words(std::string_view s) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == ',')) {
            ++i;
        }
        auto const b = i;
        while (i < s.size() && s[i] != ' ' && s[i] != '\t' && s[i] != ',') {
            ++i;
        }
        if (i > b) {
            out.emplace_back(s.substr(b, i - b));
        }
    }
    return out;
}

} // namespace

Suppressions::Suppressions(std::string_view readme, Markdown const &md) {
    // Строки блоков кода (вместе с ограждениями): маркеры в них только пример.
    auto const in_block = [&](std::size_t line) {
        return std::any_of(md.blocks.begin(), md.blocks.end(), [&](CodeBlock const &b) {
            auto const lines = static_cast<std::size_t>(std::count(b.text.begin(), b.text.end(), '\n'));
            auto const last = b.closed ? b.first_line + lines : std::numeric_limits<std::size_t>::max();
            return line >= b.line && line <= last;
        });
    };
    std::vector<Range> open;
    std::size_t line_no = 0;
    std::size_t pos = 0;
    while (pos < readme.size()) {
        auto eol = readme.find('\n', pos);
        if (eol == std::string_view::npos) {
            eol = readme.size();
        }
        auto const line = trim(readme.substr(pos, eol - pos));
        pos = eol + 1;
        ++line_no;
        if (!line.starts_with("<!--") || !line.ends_with("-->") || in_block(line_no)) {
            continue;
        }
        auto const body = trim(line.substr(4, line.size() - 7));
        auto const w = words(body);
        if (w.empty() || (w.front() != DISABLE && w.front() != ENABLE)) {
            continue;
        }
        std::set<std::string, std::less<>> const rules(w.begin() + 1, w.end());
        if (w.front() == DISABLE) {
            open.push_back({rules, line_no + 1, std::numeric_limits<std::size_t>::max(), rules.empty()});
            continue;
        }
        // enable без правил закрывает все открытые участки, с правилами только совпадающие.
        for (auto it = open.begin(); it != open.end();) {
            if (rules.empty() || it->rules == rules) {
                it->last = line_no - 1;
                m_ranges.push_back(*it);
                it = open.erase(it);
            } else {
                ++it;
            }
        }
    }
    m_ranges.insert(m_ranges.end(), open.begin(), open.end());
}

bool Suppressions::suppressed(std::size_t line, std::string_view rule) const {
    return std::any_of(m_ranges.begin(), m_ranges.end(),
                       [&](Range const &r) { return line >= r.first && line <= r.last && (r.all || r.rules.contains(rule)); });
}

Markdown Suppressions::visible(Markdown const &md, std::string_view rule) const {
    Markdown out;
    std::copy_if(md.blocks.begin(), md.blocks.end(), std::back_inserter(out.blocks), [&](CodeBlock const &b) { return !suppressed(b.line, rule); });
    std::copy_if(md.spans.begin(), md.spans.end(), std::back_inserter(out.spans), [&](InlineCode const &s) { return !suppressed(s.line, rule); });
    return out;
}

void Suppressions::filter(std::vector<Diagnostic> &diags) const {
    std::erase_if(diags, [&](Diagnostic const &d) { return suppressed(d.line, d.rule); });
}
