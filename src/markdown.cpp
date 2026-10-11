#include "markdown.hpp"

#include <utility>

namespace {

constexpr std::size_t MIN_FENCE = 3;

/// Ограждение блока кода: символ, длина и info-строка.
struct Fence {
    std::string_view info;
    std::size_t length{};
    char ch{};
};

std::size_t count_run(std::string_view s, std::size_t pos, char ch) {
    std::size_t n = 0;
    while (pos + n < s.size() && s[pos + n] == ch) {
        ++n;
    }
    return n;
}

std::string_view trim(std::string_view s) {
    auto const b = s.find_first_not_of(" \t");
    if (b == std::string_view::npos) {
        return {};
    }
    return s.substr(b, s.find_last_not_of(" \t") - b + 1);
}

/// Строка открывает ограждение, если после отступа идёт не меньше 3 ` или ~.
bool parse_fence(std::string_view line, Fence &fence) {
    auto const start = line.find_first_not_of(" \t");
    if (start == std::string_view::npos || (line[start] != '`' && line[start] != '~')) {
        return false;
    }
    auto const ch = line[start];
    auto const len = count_run(line, start, ch);
    if (len < MIN_FENCE) {
        return false;
    }
    auto const info = trim(line.substr(start + len));
    // В info-строке ограждения из обратных кавычек обратная кавычка недопустима (CommonMark).
    if (ch == '`' && info.find('`') != std::string_view::npos) {
        return false;
    }
    fence = {info, len, ch};
    return true;
}

bool closes_fence(std::string_view line, Fence const &open) {
    Fence f;
    return parse_fence(line, f) && f.ch == open.ch && f.length >= open.length && f.info.empty();
}

/// Строка абзаца с номером в файле.
struct ParaLine {
    std::string_view text;
    std::size_t line_no{};
};

/// Строка начинает новый блок: заголовок, строка таблицы, пункт списка или цитата.
bool starts_block(std::string_view line) {
    auto const t = trim(line);
    if (t.empty()) {
        return true;
    }
    if (t.front() == '#' || t.front() == '|' || t.front() == '>') {
        return true;
    }
    if ((t.front() == '-' || t.front() == '*' || t.front() == '+') && (t.size() == 1 || t[1] == ' ')) {
        return true;
    }
    std::size_t digits = 0;
    while (digits < t.size() && t[digits] >= '0' && t[digits] <= '9') {
        ++digits;
    }
    return digits > 0 && digits < t.size() && (t[digits] == '.' || t[digits] == ')');
}

/// Ищет inline-код в абзаце: span может продолжаться на следующих строках абзаца.
void scan_paragraph(std::vector<ParaLine> const &para, std::vector<InlineCode> &out) {
    std::string joined;
    std::vector<std::size_t> starts;
    for (auto const &l : para) {
        starts.push_back(joined.size());
        joined.append(l.text).push_back('\n');
    }
    // Номер строки и колонка по смещению в склеенном абзаце.
    auto locate = [&](std::size_t off) {
        std::size_t k = 0;
        while (k + 1 < starts.size() && starts[k + 1] <= off) {
            ++k;
        }
        return std::pair{para[k].line_no, off - starts[k] + 1};
    };
    std::string_view const text = joined;
    std::size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '\\' && i + 1 < text.size()) {
            i += 2;
            continue;
        }
        if (text[i] != '`') {
            ++i;
            continue;
        }
        auto const open = count_run(text, i, '`');
        auto j = i + open;
        auto close = std::string_view::npos;
        while (j < text.size()) {
            if (text[j] != '`') {
                ++j;
                continue;
            }
            auto const run = count_run(text, j, '`');
            if (run == open) {
                close = j;
                break;
            }
            j += run;
        }
        if (close == std::string_view::npos) {
            // Незакрытая последовательность кавычек остаётся обычным текстом.
            i += open;
            continue;
        }
        // Перевод строки внутри span становится пробелом, отступ продолжения строки отбрасывается.
        std::string content;
        for (std::size_t k = i + open; k < close; ++k) {
            if (text[k] == '\n') {
                content.push_back(' ');
                while (k + 1 < close && (text[k + 1] == ' ' || text[k + 1] == '\t')) {
                    ++k;
                }
            } else {
                content.push_back(text[k]);
            }
        }
        // Один пробел с обеих сторон снимается, чтобы можно было писать `` `a` ``.
        if (content.size() >= 2 && content.front() == ' ' && content.back() == ' ' && !trim(content).empty()) {
            content = content.substr(1, content.size() - 2);
        }
        auto const [line_no, column] = locate(i);
        out.push_back({std::move(content), line_no, column});
        i = close + open;
    }
}

} // namespace

Markdown parse_markdown(std::string_view text) {
    Markdown md;
    Fence open;
    bool in_block = false;
    std::size_t line_no = 0;
    std::size_t pos = 0;
    std::vector<ParaLine> para;
    auto flush = [&] {
        scan_paragraph(para, md.spans);
        para.clear();
    };
    while (pos < text.size()) {
        auto eol = text.find('\n', pos);
        if (eol == std::string_view::npos) {
            eol = text.size();
        }
        auto line = text.substr(pos, eol - pos);
        pos = eol + 1;
        ++line_no;
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }

        if (in_block) {
            if (closes_fence(line, open)) {
                in_block = false;
            } else {
                auto &b = md.blocks.back();
                b.text.append(line).push_back('\n');
            }
            continue;
        }
        if (parse_fence(line, open)) {
            flush();
            in_block = true;
            auto const lang = open.info.substr(0, open.info.find_first_of(" \t{"));
            md.blocks.push_back({std::string{lang}, {}, line_no, line_no + 1});
            continue;
        }
        if (starts_block(line)) {
            flush();
        }
        if (!trim(line).empty()) {
            para.push_back({line, line_no});
        }
    }
    flush();
    if (in_block) {
        md.blocks.back().closed = false;
    }
    return md;
}

std::vector<Diagnostic> check_markdown(Markdown const &md, std::string const &file) {
    std::vector<Diagnostic> out;
    for (auto const &b : md.blocks) {
        if (!b.closed) {
            out.push_back(
                {.file = file, .rule = "markdown", .message = "блок кода не закрыт, всё до конца файла отображается как код", .line = b.line});
        }
    }
    return out;
}
