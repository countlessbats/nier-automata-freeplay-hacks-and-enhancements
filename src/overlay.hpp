#pragma once

// In-game settings panel drawn on the game's own swap chain. Toggled with F10.
bool install_overlay();
void shutdown_overlay();

// True while the panel is open, so input can be kept away from the game.
bool overlay_is_open();

// Wraps the game's DirectInput object so its devices report idle while the
// panel is open. Call with the interface DirectInput8Create returned.
void hook_direct_input(void* direct_input);

// Queues synthetic confirm presses on the pad, used to walk the title menu.
// Any real input from the player cancels whatever is still queued.
void request_menu_confirm(unsigned presses, unsigned first_delay_ms);
bool menu_confirm_pending();

