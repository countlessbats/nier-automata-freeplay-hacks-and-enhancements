#pragma once

// Disables the death penalty so equipped plug-in chips stay equipped through
// death. The corpse still spawns; recovering it returns nothing because no
// chip is ever flagged as lost, so nothing can be duplicated.
bool install_chip_keeper();
