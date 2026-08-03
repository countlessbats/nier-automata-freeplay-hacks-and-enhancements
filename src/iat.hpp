#pragma once

// Redirects one of the game's imported functions to a replacement.
//
// The import table is rewritten rather than the function itself, so nothing is
// patched inside another module and the change is undone by writing the old
// pointer back. The game is packed, and its real import table only exists once
// the packer has run, so this has to be called from the running process.
//
// Returns false when the game does not import that function at all, which is
// an ordinary answer and not a failure.
bool patch_game_import(const char* dll, const char* function,
                       void* replacement, void** original);
