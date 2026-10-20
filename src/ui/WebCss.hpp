#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>

namespace saida {

inline bool webBorderShorthand(const std::string& name) {
    return name == "border" || name == "border-top" || name == "border-right" ||
           name == "border-bottom" || name == "border-left";
}

inline std::string normalizeWebBorderValue(const std::string& value) {
    std::string out;
    for (size_t i=0; i<value.size();) {
        if (value.compare(i, 2, "/*") == 0) {
            const size_t end=value.find("*/",i+2),next=end==std::string::npos?value.size():end+2;
            out.append(value,i,next-i);i=next;continue;
        }
        if (value[i]=='\'' || value[i]=='"') {
            const char quote=value[i];out+=value[i++];
            while(i<value.size()) {const char c=value[i++];out+=c;
                if(c=='\\' && i<value.size())out+=value[i++];else if(c==quote)break;}
            continue;
        }
        if (std::isalpha(static_cast<unsigned char>(value[i]))) {
            size_t end=i+1;
            while(end<value.size() && (std::isalnum(static_cast<unsigned char>(value[end])) || value[end]=='-' || value[end]=='_'))++end;
            const auto token=value.substr(i,end-i);
            if(token!="solid")out+=token;
            i=end;continue;
        }
        out+=value[i++];
    }
    return out;
}

inline std::string normalizeWebAnimationValue(const std::string& value) {
    std::string out;
    for (size_t i = 0; i < value.size();) {
        if (value.compare(i, 2, "/*") == 0) {
            const size_t end = value.find("*/", i+2);
            const size_t next = end == std::string::npos ? value.size() : end+2;
            out.append(value, i, next-i); i = next; continue;
        }
        if (value[i] == '\'' || value[i] == '"') {
            const char quote = value[i]; out += value[i++];
            while (i < value.size()) {
                const char c = value[i++]; out += c;
                if (c == '\\' && i < value.size()) out += value[i++];
                else if (c == quote) break;
            }
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(value[i])) || value[i] == '.') {
            char* end = nullptr;
            const double number = std::strtod(value.c_str()+i, &end);
            const size_t next = static_cast<size_t>(end-value.c_str());
            if (next > i && value.compare(next, 2, "ms") == 0 && std::isfinite(number)) {
                out += std::to_string(number/1000.0) + 's'; i = next+2; continue;
            }
        }
        if (std::isalpha(static_cast<unsigned char>(value[i]))) {
            size_t end = i+1;
            while (end < value.size() && (std::isalnum(static_cast<unsigned char>(value[end])) || value[end] == '-' || value[end] == '_')) ++end;
            const std::string token = value.substr(i, end-i);
            if (token == "ease" || token == "ease-in-out") out += "cubic-in-out";
            else if (token == "ease-in") out += "cubic-in";
            else if (token == "ease-out") out += "cubic-out";
            else if (token == "linear") out += "linear-in-out";
            else out += token;
            i = end; continue;
        }
        out += value[i++];
    }
    return out;
}

// .css and the JS style API use CSS alpha (0..1). Native .rcss keeps RmlUi's
// byte alpha. Preserve comments/strings, including URLs containing CSS examples.
inline std::string normalizeWebCss(const std::string& css) {
    std::string out;
    out.reserve(css.size());
    size_t declaration = 0;
    for (size_t i = 0; i < css.size();) {
        if (css.compare(i, 2, "/*") == 0) {
            const size_t end = css.find("*/", i + 2);
            const size_t next = end == std::string::npos ? css.size() : end + 2;
            out.append(css, i, next - i); i = next; continue;
        }
        if (css[i] == '\'' || css[i] == '"') {
            const char quote = css[i];
            out += css[i++];
            while (i < css.size()) {
                const char c = css[i++]; out += c;
                if (c == '\\' && i < css.size()) out += css[i++];
                else if (c == quote) break;
            }
            continue;
        }
        if (css[i] == ':') {
            std::string name = css.substr(declaration, i-declaration);
            const size_t commentEnd = name.rfind("*/");
            if (commentEnd != std::string::npos) name.erase(0, commentEnd+2);
            name.erase(std::remove_if(name.begin(), name.end(), [](unsigned char c) { return std::isspace(c); }), name.end());
            if (name == "animation" || name == "transition" || webBorderShorthand(name)) {
                size_t next = i+1;
                char quote = 0;
                int parentheses = 0;
                for (; next < css.size(); ++next) {
                    const char c = css[next];
                    if (quote) {
                        if (c == '\\' && next+1 < css.size()) ++next;
                        else if (c == quote) quote = 0;
                    } else if (css.compare(next, 2, "/*") == 0) {
                        const size_t end = css.find("*/", next+2);
                        if (end == std::string::npos) { next = css.size(); break; }
                        next = end+1;
                    } else if (c == '\'' || c == '"') quote = c;
                    else if (c == '(') ++parentheses;
                    else if (c == ')') --parentheses;
                    else if (!parentheses && (c == ';' || c == '}')) break;
                }
                const auto value=css.substr(i+1, next-i-1);
                out += ':';
                // Continue the normal lexer on the border value so Web rgba
                // alpha is normalized too; native .rcss remains unchanged.
                out += webBorderShorthand(name) ? normalizeWebCss(normalizeWebBorderValue(value)) : normalizeWebAnimationValue(value);
                i = next; continue;
            }
        }
        bool rgba = i + 5 <= css.size();
        const char name[] = "rgba(";
        for (size_t n = 0; rgba && n < 5; ++n)
            rgba = std::tolower(static_cast<unsigned char>(css[i + n])) == name[n];
        if (rgba && (i == 0 || (!std::isalnum(static_cast<unsigned char>(css[i-1])) && css[i-1] != '-'))) {
            const size_t close = css.find(')', i + 5);
            if (close != std::string::npos) {
                const size_t comma = css.rfind(',', close);
                if (comma != std::string::npos && comma > i + 5 &&
                    std::count(css.begin() + i, css.begin() + close, ',') == 3) {
                    const char* begin = css.c_str() + comma + 1;
                    char* end = nullptr;
                    const double alpha = std::strtod(begin, &end);
                    while (end && std::isspace(static_cast<unsigned char>(*end))) ++end;
                    if (end != begin && end == css.c_str() + close && std::isfinite(alpha) && alpha >= 0.0 && alpha <= 1.0) {
                        out.append(css, i, comma - i + 1);
                        out += std::to_string(static_cast<int>(std::lround(alpha * 255.0)));
                        out += ')'; i = close + 1; continue;
                    }
                }
            }
        }
        if (css[i] == ';' || css[i] == '{' || css[i] == '}') declaration = i+1;
        out += css[i++];
    }
    return out;
}

inline std::string normalizeWebStyleValue(const std::string& property, const std::string& value) {
    const std::string css = normalizeWebCss(value);
    if (webBorderShorthand(property)) return normalizeWebBorderValue(css);
    return property == "animation" || property == "transition" ? normalizeWebAnimationValue(css) : css;
}

} // namespace saida
