#include <iostream>
#include <cassert>
#include <string_view>

extern "C" {
#include "header.h"
}

static int infer_telemetry_state(float typing_speed_wpm, float window_switches_per_min) {
    union Entry feature_buffer[2];
    feature_buffer[0].fvalue = typing_speed_wpm;
    feature_buffer[1].fvalue = window_switches_per_min;

    float inference_output[1] = {0.0f};
    predict(feature_buffer, 0, inference_output);

    return (inference_output[0] >= 0.5f) ? 1 : 0;
}

int main() {
    // Deep Focus: High sustained WPM with minimal window switching -> Focus (1)
    assert(infer_telemetry_state(90.0f, 1.0f) == 1);

    // Total Idle: Zero keystrokes, zero switching -> Focus baseline (1)
    assert(infer_telemetry_state(0.0f, 0.0f) == 1);

    // Distracted: Low typing speed with frequent task switching -> Relax (0)
    assert(infer_telemetry_state(25.0f, 15.0f) == 0);

    // Frenetic: High typing speed coupled with erratic switching -> Relax (0)
    assert(infer_telemetry_state(130.0f, 25.0f) == 0);

    // Micro-breaks: Keystroke drop with low switching -> Focus preservation (1)
    assert(infer_telemetry_state(5.0f, 2.0f) == 1);

    std::cout << "\033[32m[PASS] All CATRA unit tests passed successfully.\033[0m\n";
    return 0;
}
