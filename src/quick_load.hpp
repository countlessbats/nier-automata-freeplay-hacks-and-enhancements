#pragma once

// Loads the most recent save without walking the title menu.
//
// The game's save system is a state machine sharing one word. State 1 is its
// completion step: it checks the staging buffer's header and copies it over the
// live game data. Nothing about that step depends on the menu, so filling the
// staging buffer from a slot file and moving the machine to state 1 applies a
// save the same way choosing it would.
//
// Returns false until the game is far enough along to accept it, so the caller
// can simply keep asking.
bool try_quick_load();
