#include "data/plugins/Resolver.hpp"

#include <algorithm>
#include <unordered_map>

#include "engine/core/Log.hpp"

namespace data {

namespace {

struct Write {
    const Plugin* plugin { nullptr };
    const Record* record { nullptr };
};

struct PendingForm {
    core::Guid id;
    const reflect::TypeInfo* type { nullptr };
    vector<Write> writes; // in load order; first one is the create
};

} // namespace

ResolveReport resolve(const vector<const Plugin*>& loadOrder,
                      const FormTypeRegistry& types,
                      FormDatabase& outDatabase) {
    ResolveReport report;

    // --- Load-order sanity (audit U7-6): every declared dependency must be
    // present AND earlier in the load order. Violations don't abort — the
    // field-level layering below still resolves deterministically — but they
    // are exactly the "works by accident" setups a modlist tool must surface.
    {
        std::unordered_map<core::Guid, size_t> rankOf;
        for (size_t i = 0; i < loadOrder.size(); ++i) {
            rankOf.emplace(loadOrder[i]->id, i);
        }
        for (size_t i = 0; i < loadOrder.size(); ++i) {
            for (const core::Guid& dep : loadOrder[i]->dependencies) {
                const auto it = rankOf.find(dep);
                if (it == rankOf.end()) {
                    LOG_WARN("Resolve: '{}' depends on {}, which is not loaded",
                             loadOrder[i]->name, dep.toString());
                    report.dependencyViolations++;
                } else if (it->second > i) {
                    LOG_WARN("Resolve: '{}' depends on '{}', which loads "
                             "AFTER it",
                             loadOrder[i]->name,
                             loadOrder[it->second]->name);
                    report.dependencyViolations++;
                }
            }
        }
    }

    // --- Collection: one walk in load order ---------------------------------
    vector<PendingForm> pending;              // creation order
    std::unordered_map<core::Guid, u32> indexOf;
    vector<Write> floating;                    // patches seen before any create

    for (const Plugin* plugin : loadOrder) {
        for (const Record& record : plugin->records) {
            const auto it = indexOf.find(record.formId);

            if (record.creates) {
                if (it == indexOf.end()) {
                    const reflect::TypeInfo* type = types.findType(record.typeId);
                    if (!type) {
                        // Text loading filters this, but cooked/binary data
                        // from an older version may not.
                        LOG_WARN("Resolve: '{}' creates {} with unknown type "
                                 "id {:#x}, skipped",
                                 plugin->name, record.formId.toString(),
                                 record.typeId);
                        report.recordsSkipped++;
                        continue;
                    }
                    indexOf.emplace(record.formId,
                                    static_cast<u32>(pending.size()));
                    pending.push_back({ record.formId, type,
                                        { Write { plugin, &record } } });
                } else {
                    LOG_WARN("Resolve: '{}' re-creates {}, treated as a patch",
                             plugin->name, record.formId.toString());
                    pending[it->second].writes.push_back({ plugin, &record });
                }
            } else {
                if (it == indexOf.end()) {
                    // Might be created by a later plugin (no enforced master
                    // order yet): keep it floating, decide at the end.
                    floating.push_back({ plugin, &record });
                } else {
                    pending[it->second].writes.push_back({ plugin, &record });
                }
            }
        }
    }

    // Floating patches: attach if some later plugin created the form
    // (out-of-order load — legal but suspicious, so warn), else orphan.
    // Attached this way they apply BEFORE the create record's own fields in
    // write order below — consistent with strict load-order semantics, where
    // the later creator simply is the later writer.
    for (const Write& write : floating) {
        const auto it = indexOf.find(write.record->formId);
        if (it == indexOf.end()) {
            LOG_WARN("Resolve: '{}' patches {}, which nothing creates",
                     write.plugin->name, write.record->formId.toString());
            report.orphanPatches++;
            continue;
        }
        LOG_WARN("Resolve: '{}' patches {} before its creator in load order",
                 write.plugin->name, write.record->formId.toString());
        auto& writes = pending[it->second].writes;
        // Re-insert by load order position.
        const auto pos = std::find_if(
            writes.begin(), writes.end(), [&](const Write& other) {
                const auto rank = [&](const Plugin* p) {
                    return std::find(loadOrder.begin(), loadOrder.end(), p) -
                           loadOrder.begin();
                };
                return rank(other.plugin) > rank(write.plugin);
            });
        writes.insert(pos, write);
    }

    // --- Materialization: deterministic, in creation order -------------------
    for (const PendingForm& pendingForm : pending) {
        uptr<Form> form = types.instantiate(pendingForm.type->id);
        if (!form) {
            LOG_ERROR("Resolve: no factory for type '{}'",
                      pendingForm.type->name);
            report.recordsSkipped +=
                static_cast<u32>(pendingForm.writes.size());
            continue;
        }
        form->id = pendingForm.id;

        // Writer + a pointer to what it wrote (the record outlives the
        // resolve; the value is only copied if a conflict materializes).
        std::unordered_map<
            u32, vector<std::pair<const Plugin*, const reflect::Value*>>>
            writersPerField;

        for (const Write& write : pendingForm.writes) {
            // A record may address the form through its own type or a base
            // type; anything else is a modding error.
            if (!pendingForm.type->isA(write.record->typeId)) {
                LOG_WARN("Resolve: '{}' patches {} as wrong type, skipped",
                         write.plugin->name, pendingForm.id.toString());
                report.recordsSkipped++;
                continue;
            }

            for (const auto& [fieldId, value] : write.record->fields) {
                const reflect::FieldInfo* field =
                    pendingForm.type->findField(fieldId);
                if (!field) {
                    LOG_WARN("Resolve: '{}': unknown field {:#x} on {}, "
                             "skipped",
                             write.plugin->name, fieldId,
                             pendingForm.type->name);
                    continue;
                }
                if (!field->set(form.get(), value)) {
                    LOG_WARN("Resolve: '{}': kind mismatch on {}.{}, skipped",
                             write.plugin->name, pendingForm.type->name,
                             field->name);
                    continue;
                }
                writersPerField[fieldId].push_back({ write.plugin, &value });
            }
            report.recordsApplied++;
        }

        // Conflicts in declared field order (parents first) for determinism.
        const auto collectConflicts = [&](const reflect::TypeInfo* type,
                                          auto&& self) -> void {
            if (!type) {
                return;
            }
            self(type->parent, self);
            for (const reflect::FieldInfo& field : type->fields) {
                const auto it = writersPerField.find(field.id);
                if (it == writersPerField.end() || it->second.size() < 2) {
                    continue;
                }
                FieldConflict conflict;
                conflict.formId = pendingForm.id;
                conflict.typeName = pendingForm.type->name;
                conflict.fieldName = field.name;
                conflict.typeId = pendingForm.type->id;
                conflict.fieldId = field.id;
                for (const auto& [writer, value] : it->second) {
                    conflict.writers.push_back({ writer->name, *value });
                }
                report.conflicts.push_back(std::move(conflict));
            }
        };
        collectConflicts(pendingForm.type, collectConflicts);

        if (outDatabase.add(std::move(form), *pendingForm.type).isValid()) {
            report.formsCreated++;
        }
    }

    return report;
}

} // namespace data
