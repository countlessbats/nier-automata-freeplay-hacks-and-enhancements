#pragma once

// Keeps L2 on lock-on while auto chips are equipped.
//
// Equipping an auto chip normally takes L2 over as a toggle for those chips.
// This answers "no auto chips equipped" at only the two places that make that
// swap, so the button keeps targeting enemies and, since L2 was the only way to
// switch the chips off, they stay on. Menus still get the real answer.
//
// Applied in memory and reversible, so it can be switched while playing.
bool set_auto_chips_always_on(bool enabled);
