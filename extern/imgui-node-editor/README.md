# imgui-node-editor (vendored)

Upstream: https://github.com/thedmd/imgui-node-editor —
branch `develop`, commit `b302971455b3719ec9b5fb94b2f92d27c62b9ff0`
(MIT, see LICENSE). Powers the node-graph editors (chantier 8.6+:
anim graph, quests, dialogues) through `game/ui/NodeCanvas`.

## Why vendored instead of CPM

The lib does not compile against our pinned Dear ImGui 1.92.8 and needs
two mechanical fixes (below). Applying them as a CPM `PATCHES` entry
broke clean rebuilds: with `CPM_SOURCE_CACHE` the source tree is shared
across build directories, the patch applies once, and every new
configure re-runs the patch step over the already-patched tree
("Reversed (or previously applied) patch detected"). Committed sources
have no fetch and no patch step — the problem class is gone.

## Local changes vs upstream

Recorded verbatim in `upstream-imgui192.patch` (apply it to the upstream
commit to reproduce this tree):

1. `imgui_extra_math.inl` — guard `operator*(float, ImVec2)` behind
   `IMGUI_VERSION_NUM < 19002`: imgui.h defines it itself under
   `IMGUI_DEFINE_MATH_OPERATORS` since 1.90.x (the neighbouring ==/!=
   and unary - operators are already guarded upstream; this one was
   missed).
2. `imgui_node_editor.cpp` — `ImRect::Floor()` no longer exists in
   imgui_internal.h; replaced by `ImFloor` on `Min`/`Max` at the 8 call
   sites.

## Updating

Fetch the new upstream commit, re-apply `upstream-imgui192.patch` (or
drop the hunks upstream has fixed), update the commit hash here and in
the patch header, rebuild.
