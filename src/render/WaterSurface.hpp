#pragma once
#include "graphics/Mesh.hpp"
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

namespace saida {
// The optional signed depth has exactly the same vertex cardinality as the
// triangle surface. Malformed data must refuse a draw, never silently fall back.
inline bool parseWaterSurface(const std::string& encoded, const std::string& depths, std::vector<Vertex>& vertices) {
    std::vector<float> values;
    values.reserve(encoded.size() / 8);
    const char* cursor = encoded.c_str();
    while (*cursor) {
        while (std::isspace(static_cast<unsigned char>(*cursor))) ++cursor;
        if (!*cursor) break;
        char* end = nullptr;
        const float value = std::strtof(cursor, &end);
        if (end == cursor || !std::isfinite(value)) return false;
        values.push_back(value);
        cursor = end;
    }
    if (values.empty() || values.size() % 9 != 0 || values.size() / 3 > std::numeric_limits<uint32_t>::max())
        return false;
    vertices.resize(values.size() / 3);
    for (size_t i = 0; i < vertices.size(); ++i)
        vertices[i].pos = {values[i * 3], values[i * 3 + 1], values[i * 3 + 2]};
    if (!depths.empty()) {
        const char* cursor=depths.c_str();
        for(auto& vertex:vertices) {
            while(std::isspace(static_cast<unsigned char>(*cursor)))++cursor;
            char* end=nullptr;const float depth=std::strtof(cursor,&end);
            if(end==cursor||!std::isfinite(depth))return false;
            vertex.color.x=depth;cursor=end;
        }
        while(std::isspace(static_cast<unsigned char>(*cursor)))++cursor;
        if(*cursor)return false;
    }
    return true;
}
} // namespace saida
