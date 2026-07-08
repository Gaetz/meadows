#include "data/plugins/RecordDiff.hpp"

#include <utility>

namespace data {

void diffToRecord(const reflect::TypeInfo& type, const void* object,
                  const void* reference, Record& record,
                  bool includeInherited) {
    const auto emit = [&](const reflect::FieldInfo& field) {
        if (field.flags & reflect::Transient) {
            return;
        }
        reflect::Value value = field.get(object);
        if (!reference || field.get(reference) != value) {
            record.fields.emplace(field.id, std::move(value));
        }
    };
    if (includeInherited) {
        reflect::forEachField(type, emit);
    } else {
        for (const reflect::FieldInfo& field : type.fields) {
            emit(field);
        }
    }
}

} // namespace data
