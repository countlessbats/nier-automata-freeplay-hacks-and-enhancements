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
