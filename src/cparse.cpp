#include "cparse.hpp"

namespace {

/// Пропускает строку или символьный литерал, начинающийся в i; возвращает позицию закрывающей кавычки.
std::size_t skip_quoted(std::string_view s, std::size_t i) {
    auto const q = s[i];
    for (++i; i < s.size() && s[i] != q; ++i) {
        if (s[i] == '\\') {
            ++i;
        }
    }
    return i;
}

} // namespace

std::vector<std::string_view> split_args(std::string_view s) {
    std::vector<std::string_view> out;
    int depth = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        auto const c = s[i];
        if (c == '"' || c == '\'') {
            i = skip_quoted(s, i);
        } else if (c == '(' || c == '{' || c == '[') {
            ++depth;
        } else if (c == ')' || c == '}' || c == ']') {
            --depth;
        } else if (c == ',' && depth == 0) {
            out.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    out.push_back(s.substr(start));
    return out;
}

std::string_view bracket_content(std::string_view s, std::size_t open) {
    if (open >= s.size()) {
        return {};
    }
    auto const o = s[open];
    auto const c = o == '(' ? ')' : o == '{' ? '}' : ']';
    int depth = 0;
    for (std::size_t i = open; i < s.size(); ++i) {
        if (s[i] == '"' || s[i] == '\'') {
            i = skip_quoted(s, i);
        } else if (s[i] == o) {
            ++depth;
        } else if (s[i] == c && --depth == 0) {
            return s.substr(open + 1, i - open - 1);
        }
    }
    return {};
}

std::vector<std::string> string_literals(std::string_view s, bool single_quotes) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        auto const q = s[i];
        if (q == '\'' && !single_quotes) {
            i = skip_quoted(s, i);
            continue;
        }
        if (q != '"' && q != '\'') {
            continue;
        }
        std::string lit;
        for (++i; i < s.size() && s[i] != q; ++i) {
            if (s[i] == '\\' && i + 1 < s.size()) {
                ++i;
            }
            lit.push_back(s[i]);
        }
        out.push_back(std::move(lit));
    }
    return out;
}

std::string strip_comments(std::string_view code, bool python) {
    std::string out{code};
    auto blank = [&](std::size_t from, std::size_t to) {
        for (auto k = from; k < to && k < out.size(); ++k) {
            if (out[k] != '\n') {
                out[k] = ' ';
            }
        }
    };
    for (std::size_t i = 0; i < code.size(); ++i) {
        auto const c = code[i];
        if (c == '"' || c == '\'') {
            i = skip_quoted(code, i);
        } else if (python && c == '#') {
            auto const eol = code.find('\n', i);
            blank(i, eol == std::string_view::npos ? code.size() : eol);
            i = eol == std::string_view::npos ? code.size() : eol;
        } else if (!python && c == '/' && i + 1 < code.size() && code[i + 1] == '/') {
            auto const eol = code.find('\n', i);
            blank(i, eol == std::string_view::npos ? code.size() : eol);
            i = eol == std::string_view::npos ? code.size() : eol;
        } else if (!python && c == '/' && i + 1 < code.size() && code[i + 1] == '*') {
            auto const end = code.find("*/", i + 2);
            auto const stop = end == std::string_view::npos ? code.size() : end + 2;
            blank(i, stop);
            i = stop - 1;
        }
    }
    return out;
}
