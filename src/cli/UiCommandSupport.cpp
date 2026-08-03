#include "cli/UiCommandSupport.hpp"

#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Element.h>

#include <charconv>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

namespace saida::ui_cli {

bool parseSize(const std::string& text, uint32_t& width, uint32_t& height, std::string& error) {
    const size_t separator = text.find_first_of("xX");
    if (separator == std::string::npos || separator == 0 || separator + 1 >= text.size()) {
        error = "expected WxH, got '" + text + "'";
        return false;
    }
    auto parseDimension = [&](const std::string& part, uint32_t& out) {
        unsigned long value = 0;
        const char* begin = part.data();
        const char* end = begin + part.size();
        const std::from_chars_result result = std::from_chars(begin, end, value);
        if (result.ec != std::errc{} || result.ptr != end || value == 0 || value > kMaxDimension) {
            error = "each dimension must be between 1 and " + std::to_string(kMaxDimension) +
                    ", got '" + part + "'";
            return false;
        }
        out = static_cast<uint32_t>(value);
        return true;
    };
    return parseDimension(text.substr(0, separator), width) &&
           parseDimension(text.substr(separator + 1), height);
}

bool writeText(const std::string& path, const std::string& text, std::string& error) {
    if (path == "-") {
        std::cout << text;
        return true;
    }
    const std::filesystem::path target(path);
    if (target.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(target.parent_path(), ec);
    }
    std::ofstream file(target, std::ios::binary | std::ios::trunc);
    if (!file) {
        error = "cannot open '" + path + "' for writing";
        return false;
    }
    file << text;
    if (!file) {
        error = "failed while writing '" + path + "'";
        return false;
    }
    return true;
}

const char* displayName(Rml::Element& element) {
    switch (element.GetComputedValues().display()) {
    case Rml::Style::Display::None: return "none";
    case Rml::Style::Display::Block: return "block";
    case Rml::Style::Display::Inline: return "inline";
    case Rml::Style::Display::InlineBlock: return "inline-block";
    case Rml::Style::Display::FlowRoot: return "flow-root";
    case Rml::Style::Display::Flex: return "flex";
    case Rml::Style::Display::InlineFlex: return "inline-flex";
    case Rml::Style::Display::Table: return "table";
    case Rml::Style::Display::InlineTable: return "inline-table";
    case Rml::Style::Display::TableRow: return "table-row";
    case Rml::Style::Display::TableRowGroup: return "table-row-group";
    case Rml::Style::Display::TableColumn: return "table-column";
    case Rml::Style::Display::TableColumnGroup: return "table-column-group";
    case Rml::Style::Display::TableCell: return "table-cell";
    }
    return "unknown";
}

std::string elementLabel(Rml::Element& element) {
    std::string label = element.GetTagName();
    const Rml::String id = element.GetId();
    if (!id.empty()) label += "#" + id;
    const Rml::String classes = element.GetClassNames();
    if (!classes.empty()) {
        // GetClassNames returns them space-separated; a CSS-shaped label reads
        // better in a report and is what an author would search for.
        std::string dotted;
        size_t start = 0;
        while (start < classes.size()) {
            size_t end = classes.find(' ', start);
            if (end == std::string::npos) end = classes.size();
            if (end > start) dotted += "." + classes.substr(start, end - start);
            start = end + 1;
        }
        label += dotted;
    }
    return label;
}

std::string elementPath(Rml::Element& element) {
    std::vector<std::string> parts;
    for (Rml::Element* node = &element; node != nullptr; node = node->GetParentNode()) {
        // The context's own root element is RmlUi plumbing, not something the
        // author wrote; a path through it would name a thing they cannot find.
        if (node->GetTagName() == "#root") break;
        parts.push_back(elementLabel(*node));
    }
    std::string path;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        if (!path.empty()) path += " > ";
        path += *it;
    }
    return path;
}

} // namespace saida::ui_cli
