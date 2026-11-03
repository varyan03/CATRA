#pragma once

/**
 * ML Wrapper
 *
 * C++ facade over the Treelite/TL2cgen-generated XGBoost predictor (main.c / header.h).
 * The generated C sources stay untouched so the model can be re-exported freely;
 * all C linkage concerns are contained in ml_wrapper.cpp.
 *
 * Feature order MUST match train_and_export.py:
 *   [0] typing_speed_wpm
 *   [1] window_switches_per_min   (fed by the TTY-switch proxy in the daemon)
 *
 * Both functions are pure and thread-safe (no shared state).
 */

// Returns the model's Focus probability in [0, 1]. Non-finite or negative inputs are clamped to 0.
// Returns 0 (Relax) when wpm is below an idle floor (5 WPM): the model is unreliable there.
float predict_focus_probability(double wpm, double switches_per_min) noexcept;

// Returns true when the Focus probability is strictly greater than 0.5.
bool predict_focus(double wpm, int tty_switches_per_min) noexcept;
