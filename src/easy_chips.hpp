#pragma once

// Lets the Easy-only auto chips be used at any difficulty.
//
// The game asks one function whether a chip set contains an auto chip; this
// makes that answer "no". It is a three byte patch applied in memory, so it can
// be turned on and off while playing and nothing is written to the save. A chip
// already equipped when it is switched off simply stays where it is, and only
// becomes unequippable once removed.
bool set_easy_chips_anywhere(bool enabled);
