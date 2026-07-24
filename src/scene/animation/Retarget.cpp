#include "scene/animation/Retarget.hpp"

#include <cctype>

namespace saida {

namespace {
// Normalize a joint name for fuzzy matching across conventions.
std::string normalize(const std::string& in) {
    std::string s = in;
    if (auto colon = s.find(':'); colon != std::string::npos)
        s = s.substr(colon + 1);  // drop "mixamorig:" / other namespaces
    std::string out;
    out.reserve(s.size());
    for (char ch : s) {
        if (ch == ' ' || ch == '_' || ch == '-' || ch == '.') continue;
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    if (out == "pelvis") out = "hips";  // common synonym
    return out;
}
} // namespace

RetargetMap RetargetMap::autoMap(const std::vector<std::string>& rigBones,
                                 const std::vector<std::string>& clipBones) {
    std::unordered_map<std::string, std::string> clipByNorm;
    for (const std::string& cb : clipBones) clipByNorm.emplace(normalize(cb), cb);

    RetargetMap m;
    for (const std::string& rb : rigBones) {
        auto it = clipByNorm.find(normalize(rb));
        if (it != clipByNorm.end() && it->second != rb)  // only store real remaps
            m.set(rb, it->second);
    }
    return m;
}

} // namespace saida
