#pragma once

// Watches for the save file being opened and records who asked for it.
//
// Every attempt to find the load path by reasoning about objects has failed:
// `@Continue` is not a token category, `ContinueState` is not reachable in the
// StateObject chain, and scanning for objects of the right shape returns string
// tables. This inverts the problem. The save file has to be opened by whatever
// loads it, so hooking the open and capturing the return addresses names the
// functions involved without guessing at anything.
bool install_save_probe();
