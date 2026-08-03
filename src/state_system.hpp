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
