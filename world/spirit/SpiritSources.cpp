#include "world/spirit/SpiritSources.hpp"

#include <algorithm>
#include <cmath>

#include "data/forms/FormQuery.hpp"
#include "data/plugins/RecordDiff.hpp"

namespace world {

namespace {

// Derived-identity family for placed sources (the cell/prefab combine
// idiom): the sequence keeps ids unique within a list, the spot keeps
// two lists (two maps) apart.
core::Guid sourceGuid(const core::Guid& worldspace, i32 sequence, f32 x,
                      f32 z) {
    const u64 spot =
        (static_cast<u64>(static_cast<u32>(static_cast<i32>(x))) << 32) |
        static_cast<u64>(static_cast<u32>(static_cast<i32>(z)));
    const core::Guid seq { static_cast<u64>(static_cast<u32>(sequence)),
                           0x7370697269743031ull }; // "spirit01"
    return core::Guid::combine(core::Guid::combine(worldspace, seq),
                               core::Guid { spot, 0x73706f7473706f74ull });
}

} // namespace

core::Guid SpiritSourceList::add(render::terrain::SpiritSource source,
                                 const core::Guid& worldspace) {
    if (list.size() >= kMaxSources) {
        list.erase(list.begin()); // the oldest yields
    }
    source.sequence = nextSequence++;
    source.id = sourceGuid(worldspace, source.sequence, source.x, source.z);
    list.push_back({ source, worldspace });
    return source.id;
}

bool SpiritSourceList::remove(const core::Guid& id) {
    const auto it = std::find_if(list.begin(), list.end(),
                                 [&](const Entry& e) {
                                     return e.source.id == id;
                                 });
    if (it == list.end()) {
        return false;
    }
    list.erase(it);
    return true;
}

void SpiritSourceList::clear() { list.clear(); }

bool SpiritSourceList::tick(f32 simSeconds) {
    // Finite lifetimes count down to exactly 0 and leave; a negative
    // `remaining` MEANS permanent, so the countdown never crosses zero.
    for (Entry& e : list) {
        if (e.source.remaining >= 0.0f) {
            e.source.remaining =
                std::max(0.0f, e.source.remaining - simSeconds);
        }
    }
    const size_t before = list.size();
    std::erase_if(list, [](const Entry& e) {
        return e.source.remaining == 0.0f;
    });
    return list.size() != before;
}

vector<render::terraingen::WaterSource> SpiritSourceList::waterSourcesFor(
    const WorldspaceFilter& filter) const {
    vector<render::terraingen::WaterSource> out;
    for (const Entry& e : list) {
        if (e.source.kind != render::terrain::SpiritKind::Water ||
            !filter.matches(e.worldspace)) {
            continue;
        }
        const auto& s = e.source;
        if (s.radius <= 4.0f) {
            out.push_back({ s.x, s.z, s.rate });
            continue;
        }
        // A wide spring: the kernel's 13-cell disc is ~4 m — spread the
        // discharge over a center + six ring taps, same total.
        const f32 share = s.rate / 7.0f;
        out.push_back({ s.x, s.z, share });
        const f32 ring = s.radius * 0.55f;
        for (u32 k = 0; k < 6; ++k) {
            const f32 a = static_cast<f32>(k) * (6.2831853f / 6.0f);
            out.push_back({ s.x + std::cos(a) * ring,
                            s.z + std::sin(a) * ring, share });
        }
    }
    return out;
}

vector<data::Record> SpiritSourceList::capture() const {
    vector<const Entry*> sorted;
    sorted.reserve(list.size());
    for (const Entry& e : list) {
        sorted.push_back(&e);
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const Entry* a, const Entry* b) {
                  return a->source.sequence < b->source.sequence;
              });
    static const SpiritSourceForm kDefaults {};
    const reflect::TypeInfo& type = SpiritSourceForm::staticTypeInfo();
    vector<data::Record> records;
    for (const Entry* e : sorted) {
        const auto& s = e->source;
        SpiritSourceForm form;
        form.worldspace = e->worldspace;
        form.spirit = str { render::terrain::spiritName(s.kind) };
        form.x = s.x;
        form.z = s.z;
        form.dirX = s.dirX;
        form.dirZ = s.dirZ;
        form.rate = s.rate;
        form.radius = s.radius;
        form.remainingSeconds = s.remaining;
        form.sequence = s.sequence;
        data::Record record;
        record.formId = s.id;
        record.typeId = type.id;
        record.creates = true;
        data::diffToRecord(type, &form, &kDefaults, record,
                           /*includeInherited=*/false);
        records.push_back(std::move(record));
    }
    return records;
}

void SpiritSourceList::apply(const data::FormDatabase& forms) {
    list.clear();
    nextSequence = 1;
    data::forEach<SpiritSourceForm>(
        forms, [&](const SpiritSourceForm& form) {
            const auto kind = render::terrain::spiritFromName(form.spirit);
            if (kind == render::terrain::SpiritKind::kCount) {
                return; // unknown spirit name: a data typo, skipped
            }
            Entry e;
            e.worldspace = form.worldspace;
            e.source.id = form.id;
            e.source.kind = kind;
            e.source.x = form.x;
            e.source.z = form.z;
            e.source.dirX = form.dirX;
            e.source.dirZ = form.dirZ;
            e.source.rate = form.rate;
            e.source.radius = form.radius;
            e.source.remaining = form.remainingSeconds;
            e.source.sequence = form.sequence;
            nextSequence = std::max(nextSequence, form.sequence + 1);
            list.push_back(std::move(e));
        });
    std::sort(list.begin(), list.end(), [](const Entry& a, const Entry& b) {
        return a.source.sequence < b.source.sequence;
    });
}

} // namespace world
