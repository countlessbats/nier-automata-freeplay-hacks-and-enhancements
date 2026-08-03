#pragma once

#include <cstdint>

// The game's `hap` state system. Token categories are a linked list of named
// objects; `@Continue` is the one that owns continuing from a save.
//
// Locating it is the first half of loading the most recent save without
// pressing anything: it replaces guessing at menu layout with the object the
// game itself uses.
struct TokenCategoryView {
    uintptr_t address{};
    const char* name{};
};

// Walks the list and writes every category to the log. Returns the address of
// the named one, or 0.
uintptr_t find_token_category(const char* name, bool log_all);

// `@Continue` turned out not to be a token category, so the load path is not a
// lookup by that name. `@SceneState` is present, and its category sits at the
// end of the SceneStateSystem object, so the system itself is 0x40 earlier.
// Returns 0 if the category is absent.
uintptr_t find_scene_state_system();

// StateObjects are a second, separate linked list from the token categories.
// `ContinueState` is one, and its script export takes a single int, which is
// the shape of "continue from slot N" — the call the Start Game button ends up
// making. Walks the list and logs every name; returns the named one, or 0.
uintptr_t find_state_object(const char* name, bool log_all);

// Walking the list needs its head, which has not been found. Scanning for every
// object of the right shape does not: it finds them wherever they live, and
// catches ones created after startup if called again later.
void scan_state_objects();
