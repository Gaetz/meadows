#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include "data/forms/CoreForms.hpp"
#include "data/forms/FormTypeRegistry.hpp"
#include "data/plugins/PluginLoader.hpp"
#include "data/plugins/TomlWriter.hpp"
#include "engine/core/ConcurrentQueue.hpp"
#include "engine/core/Jobs.hpp"
#include "game/SaveGame.hpp"

// The async disk save — the split write (pure serializeSave +
// atomic writeSaveText), the single-flight gate, and the real
// JobSystem -> ConcurrentQueue round trip the SaveController runs.

namespace {

core::Guid guid(const char* text) {
    return *core::Guid::fromString(text);
}

// A small but non-trivial plugin: a created weapon + a field patch.
data::Plugin makePlugin() {
    data::Plugin plugin;
    plugin.id = guid("77770000-0000-4000-8000-000000000001");
    plugin.name = "save-async-test";
    const reflect::TypeInfo& type = data::WeaponForm::staticTypeInfo();
    data::Record creates;
    creates.formId = guid("77770000-0000-4000-8000-000000000002");
    creates.typeId = type.id;
    creates.creates = true;
    creates.fields.emplace(type.findField("editorId")->id,
                           reflect::Value { str { "AsyncSword" } });
    creates.fields.emplace(type.findField("damage")->id,
                           reflect::Value { 42.0f });
    plugin.records.push_back(std::move(creates));
    data::Record patch;
    patch.formId = guid("77770000-0000-4000-8000-000000000002");
    patch.typeId = type.id;
    patch.creates = false;
    patch.fields.emplace(type.findField("weight")->id,
                         reflect::Value { 3.5f });
    plugin.records.push_back(std::move(patch));
    return plugin;
}

str readFile(const std::filesystem::path& path) {
    std::ifstream in { path, std::ios::binary };
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

std::filesystem::path tmpPathFor(const str& slot) {
    std::filesystem::path tmp = game::savePath(slot);
    tmp += ".tmp";
    return tmp;
}

// Removes saves/<slot>.toml (+ .tmp) when the test scope ends.
struct SlotCleanup {
    str slot;
    ~SlotCleanup() {
        std::error_code ec;
        std::filesystem::remove(game::savePath(slot), ec);
        std::filesystem::remove(tmpPathFor(slot), ec);
    }
};

} // namespace

TEST_CASE("save async: serializeSave is byte-identical to what writeSave "
          "puts on disk") {
    data::FormTypeRegistry types;
    types.registerFormType<data::WeaponForm>();
    const data::Plugin plugin = makePlugin();

    const str text = game::serializeSave(plugin, types);
    CHECK(!text.empty());
    // The pure seam IS the old serialization (no behavior change).
    CHECK(text == data::writePluginToml(plugin, types));

    const str slot = "c97-sync";
    SlotCleanup cleanup { slot };
    REQUIRE(game::writeSave(slot, plugin, types));
    CHECK(readFile(game::savePath(slot)) == text); // byte-identical
    CHECK_FALSE(std::filesystem::exists(tmpPathFor(slot)));
}

TEST_CASE("save async: writeSaveText publishes atomically — no .tmp left, "
          "rename replaces an existing save") {
    const str slot = "c97-atomic";
    SlotCleanup cleanup { slot };

    REQUIRE(game::writeSaveText(slot, "first version\n"));
    CHECK(readFile(game::savePath(slot)) == "first version\n");
    CHECK_FALSE(std::filesystem::exists(tmpPathFor(slot)));

    // Overwrite an EXISTING save — the Windows rename hazard (a plain
    // std::filesystem::rename may refuse when the target exists).
    REQUIRE(game::writeSaveText(slot, "second version\n"));
    CHECK(readFile(game::savePath(slot)) == "second version\n");
    CHECK_FALSE(std::filesystem::exists(tmpPathFor(slot)));
}

TEST_CASE("save async: the single-flight gate — one save in flight, a "
          "request while busy defers the LAST slot") {
    game::SaveFlightGate gate;
    CHECK_FALSE(gate.busy());

    // Idle: the request starts immediately.
    CHECK(gate.requestStart("a"));
    CHECK(gate.busy());

    // Busy: requests are deferred, last slot wins, no queue growth.
    CHECK_FALSE(gate.requestStart("b"));
    CHECK_FALSE(gate.requestStart("c"));
    CHECK(gate.busy());

    // Completion hands back the deferred slot exactly once.
    const auto deferred = gate.onComplete();
    REQUIRE(deferred.has_value());
    CHECK(*deferred == "c");
    CHECK_FALSE(gate.busy());

    // The relaunch takes the gate again; a clean completion defers nothing.
    CHECK(gate.requestStart(*deferred));
    CHECK_FALSE(gate.onComplete().has_value());
    CHECK_FALSE(gate.busy());
}

TEST_CASE("save async: serialize + write on a real JobSystem, completion "
          "drained from a ConcurrentQueue") {
    data::FormTypeRegistry types;
    types.registerFormType<data::WeaponForm>();
    const data::Plugin plugin = makePlugin();
    const str expected = game::serializeSave(plugin, types);
    const str slot = "c97-async";
    SlotCleanup cleanup { slot };

    // The SaveController worker shape: the job owns plugin + registry
    // COPIES and captures the shared queue (never a `this`).
    struct Done {
        str slot;
        bool ok { false };
    };
    struct Shared {
        core::ConcurrentQueue<Done> completions;
    };
    auto shared = std::make_shared<Shared>();

    core::JobSystem jobs { 2 };
    jobs.enqueue([sharedRef = shared, plugin, types, slot] {
        const str text = game::serializeSave(plugin, types);
        sharedRef->completions.push(
            { slot, game::writeSaveText(slot, text) });
    });

    // Drain like the per-frame pump would — poll until the worker lands.
    Done done {};
    bool arrived = false;
    for (int i = 0; i < 5000 && !arrived; ++i) {
        arrived = shared->completions.tryPop(done);
        if (!arrived) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    REQUIRE(arrived);
    CHECK(done.slot == slot);
    CHECK(done.ok);

    // The file exists, is byte-identical, parses, and left no .tmp.
    const str onDisk = readFile(game::savePath(slot));
    CHECK(onDisk == expected);
    const auto parsed = data::parsePluginToml(onDisk, types, slot);
    REQUIRE(parsed.has_value());
    CHECK(parsed->records.size() == plugin.records.size());
    CHECK_FALSE(std::filesystem::exists(tmpPathFor(slot)));
}
