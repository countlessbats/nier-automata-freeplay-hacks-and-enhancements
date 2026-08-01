#pragma once

// In-game settings panel drawn on the game's own swap chain. Toggled with F10.
bool install_overlay();
void shutdown_overlay();

// True while the panel is open, so input can be kept away from the game.
bool overlay_is_open();

