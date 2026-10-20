#pragma once

#include "ui/WebCss.hpp"
#include <string>

namespace saida {
namespace web_markup_detail {
inline std::string lower(std::string value) {
    for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}
struct Attribute { size_t begin = 0, end = 0; std::string value; bool found = false; };
inline Attribute attribute(const std::string& tag, const std::string& name) {
    size_t i = 1;
    while (i < tag.size() && !std::isspace(static_cast<unsigned char>(tag[i])) && tag[i] != '>') ++i;
    while (i < tag.size()) {
        while (i < tag.size() && std::isspace(static_cast<unsigned char>(tag[i]))) ++i;
        const size_t start = i;
        while (i < tag.size() && (std::isalnum(static_cast<unsigned char>(tag[i])) || tag[i] == '-' || tag[i] == ':' || tag[i] == '_')) ++i;
        if (start == i) break;
        const std::string key = lower(tag.substr(start, i-start));
        while (i < tag.size() && std::isspace(static_cast<unsigned char>(tag[i]))) ++i;
        if (i == tag.size() || tag[i] != '=') continue;
        ++i;
        while (i < tag.size() && std::isspace(static_cast<unsigned char>(tag[i]))) ++i;
        const char quote = i < tag.size() && (tag[i] == '\'' || tag[i] == '"') ? tag[i++] : 0;
        const size_t begin = i;
        while (i < tag.size() && (quote ? tag[i] != quote : !std::isspace(static_cast<unsigned char>(tag[i])) && tag[i] != '>')) ++i;
        if (key == name) return {begin, i, tag.substr(begin, i-begin), true};
        if (quote && i < tag.size()) ++i;
    }
    return {};
}
}

// Small authoring adapter, not an HTML5 recovery parser. Lex tags outside raw
// script/style content so example markup inside JavaScript strings is untouched.
inline std::string normalizeWebMarkup(const std::string& input) {
    using namespace web_markup_detail;
    const std::string folded = lower(input);
    std::string out;
    bool webDocument = false;
    for (size_t i = 0; i < input.size();) {
        if (input.compare(i, 4, "<!--") == 0) {
            size_t end = input.find("-->", i + 4);
            end = end == std::string::npos ? input.size() : end + 3;
            out.append(input, i, end-i); i = end; continue;
        }
        if (folded.compare(i, 9, "<!doctype") == 0) {
            const size_t end = input.find('>', i);
            if (end != std::string::npos) {
                for (; i <= end; ++i) out += input[i] == '\n' ? '\n' : ' ';
                continue;
            }
        }
        if (input[i] != '<') { out += input[i++]; continue; }
        size_t end = i + 1;
        char quote = 0;
        for (; end < input.size(); ++end) {
            const char c = input[end];
            if (quote) { if (c == quote) quote = 0; }
            else if (c == '\'' || c == '"') quote = c;
            else if (c == '>') break;
        }
        if (end == input.size()) { out.append(input, i, input.size()-i); break; }
        std::string tag = input.substr(i, end-i+1);
        const bool closing = tag.size() > 1 && tag[1] == '/';
        const size_t nameStart = closing ? 2 : 1;
        size_t nameEnd = nameStart;
        while (nameEnd < tag.size() && std::isalnum(static_cast<unsigned char>(tag[nameEnd]))) ++nameEnd;
        const std::string name = lower(tag.substr(nameStart, nameEnd-nameStart));
        if (name == "html") { webDocument = true; tag.replace(nameStart, nameEnd-nameStart, "rml"); }
        if (!closing && name == "link") {
            auto type = attribute(tag, "type");
            const auto rel = attribute(tag, "rel");
            if (type.found && lower(type.value) == "text/css") tag.replace(type.begin, type.end-type.begin, "text/rcss");
            else if (!type.found && rel.found && lower(rel.value) == "stylesheet") {
                size_t at = tag.size()-1;
                while (at > 0 && std::isspace(static_cast<unsigned char>(tag[at-1]))) --at;
                if (at > 0 && tag[at-1] == '/') --at;
                tag.insert(at, " type=\"text/rcss\"");
            }
        }
        if (webDocument && !closing) {
            const auto style = attribute(tag, "style");
            if (style.found) tag.replace(style.begin, style.end-style.begin, normalizeWebCss(style.value));
        }
        if (!closing && (name == "link" || name == "meta" || name == "input" || name == "img" || name == "br" || name == "hr")) {
            size_t last = tag.size()-1;
            while (last > 0 && std::isspace(static_cast<unsigned char>(tag[last-1]))) --last;
            if (last > 0 && tag[last-1] != '/') tag.insert(tag.size()-1, "/");
        }
        out += tag; i = end + 1;
        size_t last = tag.size()-1;
        while (last > 0 && std::isspace(static_cast<unsigned char>(tag[last-1]))) --last;
        const bool selfClosing = last > 0 && tag[last-1] == '/';
        if (!closing && !selfClosing && (name == "script" || name == "style")) {
            const std::string terminator = "</" + name;
            size_t close = folded.find(terminator, i);
            while (close != std::string::npos && close + terminator.size() < folded.size() &&
                   folded[close + terminator.size()] != '>' &&
                   !std::isspace(static_cast<unsigned char>(folded[close + terminator.size()])))
                close = folded.find(terminator, close + terminator.size());
            const size_t next = close == std::string::npos ? input.size() : close;
            const std::string raw = input.substr(i, next-i);
            out += name == "style" && webDocument ? normalizeWebCss(raw) : raw;
            i = next;
        }
    }
    return out;
}
} // namespace saida
