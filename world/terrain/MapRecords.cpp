#include "world/terrain/MapRecords.hpp"

#include <cstdio>

#include "engine/core/Log.hpp"
#include "world/worldspace/WorldForms.hpp"

namespace world {

namespace {

// Derived-identity family for one map: every record hangs off the map
// guid through the same combine the dungeon/cell contracts use.
core::Guid derived(const core::Guid& mapGuid, u64 index) {
    return core::Guid::combine(mapGuid,
                               core::Guid { index, 0x6d61707265637331ull });
}

// Fixed index bases per record family (stable across re-bakes).
constexpr u64 kLakeBase = 0x1000;
constexpr u64 kRiverBase = 0x2000;
constexpr u64 kRiverPointBase = 0x3000; // off the RIVER guid
constexpr u64 kSliceBase = 0x4000;

struct Stager {
    data::EditSession& session;
    const data::FormDatabase& forms;

    void ensure(const reflect::TypeInfo& type, const core::Guid& guid,
                const str& editorId) {
        if (!forms.find(guid) && !session.isCreated(guid)) {
            session.createForm(type.id, editorId, guid);
        }
    }

    template <typename T>
    void set(const reflect::TypeInfo& type, const core::Guid& guid,
             const char* field, const T& value) {
        session.setField(guid, type.findField(field)->id,
                         reflect::Value { value });
    }
};

} // namespace

MapStageResult stageMapRecords(
    data::EditSession& session, const data::FormDatabase& forms,
    const core::Guid& mapGuid, const str& mapName, i32 mapX, i32 mapZ,
    f32 mapSize, u32 mapSeed, const vector<MapSliceRecord>& slices,
    const vector<render::terraingen::Lake>& lakes,
    const vector<render::terraingen::River>& rivers) {
    Stager st { session, forms };
    MapStageResult out;
    out.worldspace = mapGuid;

    // The worldspace IS the map (guid = mapGuid): identity + rect.
    {
        const reflect::TypeInfo& type =
            WorldspaceForm::staticTypeInfo();
        st.ensure(type, mapGuid, mapName);
        st.set(type, mapGuid, "bounded", true);
        st.set(type, mapGuid, "mapX", mapX);
        st.set(type, mapGuid, "mapZ", mapZ);
        st.set(type, mapGuid, "mapSize", mapSize);
        st.set(type, mapGuid, "mapSeed", mapSeed);
    }

    // One TerrainRegionForm per slice — the record set IS the slice
    // list (the runtime manifest stays a cache sidecar).
    {
        const reflect::TypeInfo& type =
            TerrainRegionForm::staticTypeInfo();
        for (size_t i = 0; i < slices.size(); ++i) {
            const MapSliceRecord& slice = slices[i];
            const core::Guid guid = derived(mapGuid, kSliceBase + i);
            char editorId[64];
            std::snprintf(editorId, sizeof(editorId), "%s_S%d_%d",
                          mapName.c_str(), slice.tx, slice.tz);
            st.ensure(type, guid, editorId);
            st.set(type, guid, "worldspace", mapGuid);
            st.set(type, guid, "asset", slice.asset);
            st.set(type, guid, "detailAmplitude",
                   slice.detailAmplitude);
            st.set(type, guid, "detailWavelength",
                   slice.detailWavelength);
            st.set(type, guid, "detailOctaves", slice.detailOctaves);
        }
        out.slices = static_cast<u32>(slices.size());
    }

    // Water: rect + level per lake (masks stay .twb-tier), the course
    // as child records per river (the §C.1 child-record convention).
    {
        const reflect::TypeInfo& type = WaterBodyForm::staticTypeInfo();
        for (size_t i = 0; i < lakes.size(); ++i) {
            const render::terraingen::Lake& lake = lakes[i];
            const core::Guid guid = derived(mapGuid, kLakeBase + i);
            char editorId[64];
            std::snprintf(editorId, sizeof(editorId), "%s_Lake%zu",
                          mapName.c_str(), i);
            st.ensure(type, guid, editorId);
            st.set(type, guid, "worldspace", mapGuid);
            st.set(type, guid, "surfaceLevel", lake.level);
            st.set(type, guid, "minX", lake.minX);
            st.set(type, guid, "minZ", lake.minZ);
            st.set(type, guid, "maxX", lake.maxX);
            st.set(type, guid, "maxZ", lake.maxZ);
        }
        out.lakes = static_cast<u32>(lakes.size());
    }
    {
        const reflect::TypeInfo& riverType =
            RiverForm::staticTypeInfo();
        const reflect::TypeInfo& pointType =
            RiverPointForm::staticTypeInfo();
        for (size_t r = 0; r < rivers.size(); ++r) {
            const render::terraingen::River& river = rivers[r];
            const core::Guid riverGuid =
                derived(mapGuid, kRiverBase + r);
            char editorId[64];
            std::snprintf(editorId, sizeof(editorId), "%s_River%zu",
                          mapName.c_str(), r);
            st.ensure(riverType, riverGuid, editorId);
            st.set(riverType, riverGuid, "worldspace", mapGuid);
            for (size_t p = 0; p < river.points.size(); ++p) {
                const render::terraingen::RiverPoint& pt =
                    river.points[p];
                const core::Guid pointGuid =
                    derived(riverGuid, kRiverPointBase + p);
                char ptId[80];
                std::snprintf(ptId, sizeof(ptId), "%s_River%zu_P%zu",
                              mapName.c_str(), r, p);
                st.ensure(pointType, pointGuid, ptId);
                st.set(pointType, pointGuid, "parent", riverGuid);
                st.set(pointType, pointGuid, "index",
                       static_cast<i32>(p));
                st.set(pointType, pointGuid, "position",
                       Vec3 { pt.x, pt.surface, pt.z });
                st.set(pointType, pointGuid, "halfWidth",
                       pt.halfWidth);
            }
        }
        out.rivers = static_cast<u32>(rivers.size());
    }

    LOG_INFO("Map '{}': staged {} slices, {} lakes, {} rivers "
             "(session drafts; Export ships the mod)",
             mapName, out.slices, out.lakes, out.rivers);
    return out;
}

} // namespace world
