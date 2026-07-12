#include "data/plugins/CsvImport.hpp"

#include "engine/core/Hash.hpp"
#include "engine/core/Log.hpp"
#include "engine/reflect/ValueText.hpp"

namespace data {

namespace {

// Minimal CSV: comma-separated, double-quote quoting with "" escapes,
// tolerant of \r\n. No embedded newlines inside quoted cells (spreadsheet
// exports of names/numbers/texts — the target use — never need them; a
// quoted cell keeps its commas).
vector<str> splitCsvLine(std::string_view line) {
    vector<str> cells;
    str cell;
    bool quoted = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quoted) {
            if (c == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') {
                    cell.push_back('"'); // "" escape
                    ++i;
                } else {
                    quoted = false;
                }
            } else {
                cell.push_back(c);
            }
        } else if (c == '"') {
            quoted = true;
        } else if (c == ',') {
            cells.push_back(std::move(cell));
            cell.clear();
        } else if (c != '\r') {
            cell.push_back(c);
        }
    }
    cells.push_back(std::move(cell));
    return cells;
}

} // namespace

core::Guid csvRowGuid(const core::Guid& pluginId, std::string_view editorId) {
    // Name -> guid via fnv1a + murmur3-finalizer expansions, then
    // combine() with the plugin id (keeps the v4 version/variant bits and
    // namespaces the name — two plugins can share editorIds without
    // colliding). Stable across runs/platforms: pure integer math.
    const u32 a = core::fnv1a(editorId);
    const u32 b = core::hashU32(a ^ 0x9747b28cu);
    core::Guid name;
    name.hi = (static_cast<u64>(a) << 32) | b;
    name.lo = (static_cast<u64>(core::hashU32(a)) << 32) | core::hashU32(~b);
    return core::Guid::combine(pluginId, name);
}

core::Result<Plugin> importCsv(std::string_view csv,
                               const reflect::TypeInfo& type,
                               const core::Guid& pluginId,
                               std::string_view sourceName,
                               const core::Guid& patchTarget) {
    if (!pluginId.isValid()) {
        return core::Error { str { sourceName } +
                             ": import needs a valid plugin guid (row "
                             "identities derive from it)" };
    }

    // Split lines (skip blank ones).
    vector<std::string_view> lines;
    size_t start = 0;
    while (start <= csv.size()) {
        const size_t end = csv.find('\n', start);
        const std::string_view line = csv.substr(
            start,
            end == std::string_view::npos ? std::string_view::npos
                                          : end - start);
        if (!line.empty() && line != "\r") {
            lines.push_back(line);
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    if (lines.empty()) {
        return core::Error { str { sourceName } + ": empty CSV" };
    }

    // Header row -> field columns. `form` is the explicit-identity column.
    const vector<str> header = splitCsvLine(lines[0]);
    vector<const reflect::FieldInfo*> columns(header.size(), nullptr);
    i32 formColumn = -1;
    i32 editorIdColumn = -1;
    for (size_t i = 0; i < header.size(); ++i) {
        if (header[i] == "form") {
            formColumn = static_cast<i32>(i);
            continue;
        }
        if (header[i] == "editorId") {
            editorIdColumn = static_cast<i32>(i);
        }
        columns[i] = type.findField(header[i]);
        if (!columns[i]) {
            LOG_WARN("{}: unknown column '{}' on {} — ignored", sourceName,
                     header[i], type.name);
        } else if ((columns[i]->flags & reflect::Transient) != 0) {
            LOG_WARN("{}: column '{}' is transient, not importable",
                     sourceName, header[i]);
            columns[i] = nullptr;
        }
    }
    if (formColumn < 0 && editorIdColumn < 0) {
        return core::Error { str { sourceName } +
                             ": header needs a 'form' or 'editorId' column "
                             "(row identity)" };
    }

    Plugin plugin;
    plugin.id = pluginId;
    plugin.name = str { sourceName };
    if (patchTarget.isValid()) {
        // A pack requires its target (load-order validation material).
        plugin.dependencies.push_back(patchTarget);
    }
    for (size_t rowIndex = 1; rowIndex < lines.size(); ++rowIndex) {
        const vector<str> cells = splitCsvLine(lines[rowIndex]);

        // Identity: explicit `form` guid, else derived from editorId.
        core::Guid formId;
        if (formColumn >= 0 &&
            static_cast<size_t>(formColumn) < cells.size() &&
            !cells[formColumn].empty()) {
            if (const auto parsed =
                    core::Guid::fromString(cells[formColumn])) {
                formId = *parsed;
            } else {
                LOG_WARN("{}: row {}: malformed 'form' guid — skipped",
                         sourceName, rowIndex + 1);
                continue;
            }
        } else if (editorIdColumn >= 0 &&
                   static_cast<size_t>(editorIdColumn) < cells.size() &&
                   !cells[editorIdColumn].empty()) {
            // Patch mode derives the TARGET plugin's row identity — the
            // record then layers onto the row that plugin created (§5).
            formId = csvRowGuid(
                patchTarget.isValid() ? patchTarget : pluginId,
                cells[editorIdColumn]);
        } else {
            LOG_WARN("{}: row {}: no 'form' nor 'editorId' — skipped",
                     sourceName, rowIndex + 1);
            continue;
        }

        Record record;
        record.formId = formId;
        record.typeId = type.id;
        record.creates = !patchTarget.isValid();
        for (size_t i = 0; i < cells.size() && i < columns.size(); ++i) {
            if (!columns[i] || cells[i].empty()) {
                continue; // empty cell = keep the field's default
            }
            if (patchTarget.isValid() &&
                static_cast<i32>(i) == editorIdColumn) {
                continue; // identity-only on a patch: keep the target's
            }
            auto value =
                reflect::valueFromString(columns[i]->kind, cells[i]);
            if (!value) {
                LOG_WARN("{}: row {}: cell '{}' does not parse as {} — "
                         "skipped",
                         sourceName, rowIndex + 1, cells[i], header[i]);
                continue;
            }
            record.fields.insert_or_assign(columns[i]->id,
                                           std::move(*value));
        }
        plugin.records.push_back(std::move(record));
    }
    return plugin;
}

} // namespace data
