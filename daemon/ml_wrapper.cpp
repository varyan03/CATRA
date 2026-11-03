#include "ml_wrapper.h"

// Pre-include every standard header that header.h pulls in, OUTSIDE the extern "C" block.
// libstdc++'s <math.h> drags in <cmath> templates, which fail to compile with C linkage
// ("template with C linkage"). Including them first makes the inner includes no-ops via guards.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <float.h>
#include <math.h>
#include <stdint.h>

#include <cmath>

extern "C" {
#include "header.h"
}

namespace {

constexpr int kNumFeatures = 2;

// Out-of-distribution guard. The synthetic training set never contains a fully idle user
// (relax samples carry ~15 switches/min), so at (wpm=0, switches=0) the model emits
// P(focus)=0.517. Because the AnchorEngine ORs the ML signal into session warmth, that
// would keep an idle session permanently "warm". Below this floor, no typing => no focus.
// Remove once the model is retrained with idle samples (wpm~0, low switches -> label 0).
constexpr double kIdleWpmFloor = 5.0;

// The generated predictor signals "missing" when Entry::missing == -1. Writing a float
// into the union overwrites all 4 bytes, so finite values can never alias -1 (that bit
// pattern is a NaN). Guard the layout assumption at compile time.
static_assert(sizeof(union Entry) == sizeof(float), "Unexpected Treelite Entry layout");

inline float sanitize(double v) noexcept {
    if (!std::isfinite(v) || v < 0.0) {
        return 0.0f;
    }
    return static_cast<float>(v);
}

} // namespace

float predict_focus_probability(double wpm, double switches_per_min) noexcept {
    if (sanitize(wpm) < kIdleWpmFloor) {
        return 0.0f;
    }

    union Entry features[kNumFeatures];
    features[0].fvalue = sanitize(wpm);
    features[1].fvalue = sanitize(switches_per_min);

    // predict() ACCUMULATES into result[0] (+=), so the buffer must start at zero.
    float result[1] = {0.0f};
    predict(features, /*pred_margin=*/0, result);   // pred_margin=0 -> sigmoid applied

    const float p = result[0];
    if (!std::isfinite(p)) {
        return 0.0f;
    }
    return p;
}

bool predict_focus(double wpm, int tty_switches_per_min) noexcept {
    return predict_focus_probability(wpm, static_cast<double>(tty_switches_per_min)) > 0.5f;
}
