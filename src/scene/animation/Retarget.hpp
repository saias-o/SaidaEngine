#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace saida {

// Name-based animation retargeting: maps a target-rig bone name → the source-clip
// track name, so a clip authored on a differently-named skeleton (e.g. Mixamo's
// "mixamorig:Hips") plays on this rig ("Hips"). It handles naming conventions —
// not differing rest poses / proportions, which is a future pass. An empty map is
// the identity (the clip's track names must match the rig's bone names).
class RetargetMap {
public:
    void set(const std::string& rigBone, const std::string& clipBone) {
        map_[rigBone] = clipBone;
    }
    bool empty() const { return map_.empty(); }

    // Clip track name for a rig bone (identity when there is no entry).
    const std::string& resolve(const std::string& rigBone) const {
        auto it = map_.find(rigBone);
        return it != map_.end() ? it->second : rigBone;
    }

    // Build a mapping by normalizing names (lowercase, strip a "namespace:" prefix
    // such as "mixamorig:", drop separators) plus a few synonyms (pelvis→hips), and
    // matching rig↔clip. Only differing names are stored, so a perfectly-matching
    // pair yields an empty (identity) map.
    static RetargetMap autoMap(const std::vector<std::string>& rigBones,
                               const std::vector<std::string>& clipBones);

private:
    std::unordered_map<std::string, std::string> map_;
};

} // namespace saida
