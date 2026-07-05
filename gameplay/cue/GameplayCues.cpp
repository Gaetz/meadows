#include "gameplay/cue/GameplayCues.hpp"

#include "data/forms/FormQuery.hpp"

namespace gameplay {

void CueTable::build(const data::FormDatabase& forms) {
    byTag.clear();
    data::forEach<data::CueForm>(forms, [&](const data::CueForm& cue) {
        // Load order wins on duplicate tags (insert_or_assign + handle
        // order = deterministic §5 semantics).
        byTag.insert_or_assign(cue.tag, &cue);
    });
}

const data::CueForm* CueTable::find(std::string_view tag) const {
    str current { tag };
    while (!current.empty()) {
        if (const auto it = byTag.find(current); it != byTag.end()) {
            return it->second;
        }
        const size_t dot = current.rfind('.');
        if (dot == str::npos) {
            return nullptr;
        }
        current.resize(dot); // "Cue.Hit.Slash" -> "Cue.Hit" -> "Cue"
    }
    return nullptr;
}

} // namespace gameplay
