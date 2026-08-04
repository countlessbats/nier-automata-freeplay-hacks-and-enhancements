// PARKED. Not in the build; see Build.ps1.
//
// Written to chase a camera that appeared to drift back to a resting angle. The
// drift was never confirmed to be real, so this is set aside rather than
// deleted: the addresses it was built around are still good, and
// docs/NIER-INTERNALS.md records what was found about the camera parameters.
//
// To bring it back, add src/camera_probe.cpp to Build.ps1 and call
// install_camera_probe() from the event loop.
#pragma once

// Dumps the camera manager's fields on a key press, so the values that drive
// the camera back to its resting angle can be identified from real play rather
// than guessed at.
//
// The manager is a static object; setCamReset passes its address directly, so
// it needs no pointer chasing. Two dumps are enough: one taken while the camera
// is somewhere it was put by hand, another once it has drifted back. Whatever
// moved between them is the recovery, and whatever it moved toward is the
// resting angle.
void install_camera_probe();
