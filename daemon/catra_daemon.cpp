#include "telemetry_sensor.h"
#include "anchor_engine.h"
#include "ml_wrapper.h"

#include <iostream>
#include <iomanip>
#include <string>
#include <csignal>
#include <thread>
#include <chrono>
#include <deque>
#include <numeric>
#include <unistd.h>

namespace {
void signal_handler(int sig) {
    (void)sig;
    shutdown_requested = 1;
}

void install_signal_handlers() {
    struct sigaction sa{};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; // Explicitly no SA_RESTART

    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

// Interruptible sleep in 100 ms slices so SIGINT/SIGTERM are honoured promptly.
void interruptible_sleep(std::chrono::milliseconds total) {
    constexpr auto kSlice = std::chrono::milliseconds(100);
    for (auto slept = std::chrono::milliseconds(0); slept < total && !shutdown_requested; slept += kSlice) {
        std::this_thread::sleep_for(kSlice);
    }
}

// Fixed-capacity rolling window: push one sample per tick, evict the oldest beyond capacity.
void push_rolling(std::deque<int>& window, int value, std::size_t capacity) {
    window.push_back(value);
    while (window.size() > capacity) {
        window.pop_front();
    }
}
} // namespace

int main(int argc, char* argv[]) {
    if (geteuid() != 0) {
        std::cerr << "Error: CATRA daemon requires root privileges (sudo) to manage process priorities.\n";
        return 1;
    }

    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <device_or_fifo_path>\n";
        return 1;
    }

    install_signal_handlers();

    const std::string device_path = argv[1];

    AnchorEngine engine;

    KeyboardSensor sensor;
    if (!sensor.start(device_path)) {
        std::cerr << "Error: Failed to open telemetry input device: " << device_path << "\n";
        return 1;
    }

    constexpr int INTERVAL = 2;                                   // seconds per tick
    constexpr std::size_t WPM_WINDOW_TICKS = 10;                  // 20 s keystroke window
    constexpr std::size_t SWITCH_WINDOW_TICKS = 60 / INTERVAL;    // 60 s TTY-switch window (30 ticks)

    std::deque<int> keystroke_window;
    std::deque<int> tty_switch_window;
    std::string previous_tty;

    std::cout << "[CATRA] Active on " << device_path
              << " | interval " << INTERVAL << "s"
              << " | WPM window " << (WPM_WINDOW_TICKS * INTERVAL) << "s"
              << " | switch window " << (SWITCH_WINDOW_TICKS * INTERVAL) << "s" << std::endl;

    while (!shutdown_requested) {
        interruptible_sleep(std::chrono::seconds(INTERVAL));
        if (shutdown_requested) {
            break;
        }

        // ---- 1. Keystroke telemetry --------------------------------------------------
        const int strokes = sensor.get_and_reset_count();
        push_rolling(keystroke_window, strokes, WPM_WINDOW_TICKS);

        if (!sensor.running()) {
            std::cout << "[CATRA] Sensor stream closed, retrying..." << std::endl;
            sensor.stop();
            interruptible_sleep(std::chrono::seconds(1));
            if (!shutdown_requested) {
                if (!sensor.start(device_path)) {
                    std::cout << "[CATRA] Sensor restart failed, will retry next tick" << std::endl;
                } else {
                    std::cout << "[CATRA] Sensor restarted" << std::endl;
                }
            }
        }

        // WPM = (keystrokes / 5) per minute, averaged over the filled portion of the window
        // so the first 20 s after startup are not artificially diluted.
        const double keystroke_sum = std::accumulate(keystroke_window.begin(), keystroke_window.end(), 0.0);
        const double window_minutes = static_cast<double>(keystroke_window.size() * INTERVAL) / 60.0;
        const double wpm = (window_minutes > 0.0) ? (keystroke_sum / 5.0) / window_minutes : 0.0;

        // ---- 2. Context-switch telemetry --------------------------------------------
        // The window is exactly 60 s, so its sum is already "switches per minute".
        // Note: the active TTY is resolved inside sweep(), so this reflects the
        // previous tick's sweep (≤ 2 s lag), which is negligible against a 60 s window.
        const int tty_switches_per_min =
            std::accumulate(tty_switch_window.begin(), tty_switch_window.end(), 0);

        // ---- 3. Inference + actuation -----------------------------------------------
        const float focus_prob = predict_focus_probability(wpm, static_cast<double>(tty_switches_per_min));
        const bool is_focused = focus_prob > 0.5f;

        engine.sweep(is_focused);

        // ---- 4. Record whether the active TTY changed during this sweep -------------
        // Only count genuine A -> B transitions. An empty result (no TTY found) neither
        // counts as a switch nor overwrites previous_tty, so A -> "" -> A is not a switch.
        const std::string& current_tty = engine.get_active_tty();
        int switched = 0;
        if (!current_tty.empty()) {
            if (!previous_tty.empty() && current_tty != previous_tty) {
                switched = 1;
            }
            previous_tty = current_tty;
        }
        push_rolling(tty_switch_window, switched, SWITCH_WINDOW_TICKS);

        std::cout << "[CATRA] WPM: " << std::fixed << std::setprecision(1) << wpm
                  << " | Switches/min: " << tty_switches_per_min
                  << " | P(focus): " << std::setprecision(3) << focus_prob
                  << " | State: " << (is_focused ? "FOCUS" : "RELAX")
                  << (switched ? " | TTY switch -> " + current_tty : std::string{})
                  << std::endl;
    }

    sensor.stop();
    // The AnchorEngine destructor also calls restore_all(), which is safe because restore_all() is idempotent.
    engine.restore_all();
    std::cout << "[CATRA] Daemon shutdown complete. Priorities cleanly restored." << std::endl;

    return 0;
}
