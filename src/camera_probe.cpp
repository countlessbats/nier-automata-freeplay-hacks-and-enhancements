#include "camera_probe.hpp"

#include <windows.h>
#include <process.h>

#include <cmath>

#include "config.hpp"

namespace {

// setCamReset at +0x4EA640 loads this address with a lea rather than reading a
// pointer from it, so the camera manager is a static object here.
constexpr unsigned kCameraManagerRva = 0x1020870;
constexpr unsigned kDumpBytes = 0x400;

bool plausible_angle(float value) {
    // Angles in this engine are radians, and the fields worth finding sit in a
    // sane range. Anything enormous, denormal or NaN is some other kind of
    // field and only makes the dump harder to read.
    if (!std::isfinite(value)) return false;
    const float magnitude = std::fabs(value);
    return magnitude == 0.0f || (magnitude > 1e-4f && magnitude < 1e4f);
}

unsigned WINAPI watch_camera(void*) {
    auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    const auto* fields = reinterpret_cast<const float*>(base + kCameraManagerRva);
    static float previous[kDumpBytes / sizeof(float)]{};
    bool have_previous = false;
    bool down = false;
    unsigned dump = 0;

    for (;;) {
        const bool now = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        if (now && !down) {
            ++dump;
            log_line("Camera dump %u ---------------------------------", dump);
            for (unsigned i = 0; i < kDumpBytes / sizeof(float); ++i) {
                const float value = fields[i];
                if (!plausible_angle(value)) continue;
                // After the first dump only the fields that actually moved are
                // interesting; everything else is noise.
                if (have_previous && std::fabs(value - previous[i]) < 1e-4f) continue;
                if (have_previous) {
                    log_line("  +0x%03X  %+.4f  (was %+.4f, moved %+.4f)",
                             i * 4, value, previous[i], value - previous[i]);
                } else {
                    log_line("  +0x%03X  %+.4f", i * 4, value);
                }
                previous[i] = value;
            }
            have_previous = true;
        }
        down = now;
        Sleep(16);
    }
}

}  // namespace

void install_camera_probe() {
    _beginthreadex(nullptr, 0, &watch_camera, nullptr, 0, nullptr);
    log_line("Camera probe: press F9 to dump the camera manager at +0x%X",
             kCameraManagerRva);
}
