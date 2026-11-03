#include "telemetry_sensor.h"
#include "anchor_engine.h"

#include <iostream>
#include <iomanip>
#include <string>
#include <csignal>
#include <thread>
#include <chrono>
#include <deque>
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
} // namespace

// TODO: Replace with XGBoost inference (features: WPM + window-switch proxy)
bool predict_focus_state(double wpm) {
    return wpm >= 20.0;
}

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

    constexpr int INTERVAL = 2;
    constexpr int WINDOW_TICKS = 10;
    std::deque<int> window;

    std::cout << "[CATRA] Active on " << device_path
              << " | interval " << INTERVAL << "s"
              << " | window " << (WINDOW_TICKS * INTERVAL) << "s" << std::endl;

    while (!shutdown_requested) {
        for (int i = 0; i < (INTERVAL * 10); ++i) {
            if (shutdown_requested) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        if (shutdown_requested) {
            break;
        }

        const int strokes = sensor.get_and_reset_count();
        window.push_back(strokes);
        if (window.size() > static_cast<std::size_t>(WINDOW_TICKS)) {
            window.pop_front();
        }

        if (!sensor.running()) {
            std::cout << "[CATRA] Sensor stream closed, retrying..." << std::endl;
            sensor.stop();
            for (int i = 0; i < 10; ++i) {
                if (shutdown_requested) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (!shutdown_requested) {
                if (!sensor.start(device_path)) {
                    std::cout << "[CATRA] Sensor restart failed, will retry next tick" << std::endl;
                } else {
                    std::cout << "[CATRA] Sensor restarted" << std::endl;
                }
            }
        }

        double sum_of_window = 0.0;
        for (int count : window) {
            sum_of_window += static_cast<double>(count);
        }

        const double window_ticks_filled = static_cast<double>(window.size());
        double wpm = 0.0;
        if (window_ticks_filled > 0.0) {
            wpm = (sum_of_window / 5.0) / (window_ticks_filled * static_cast<double>(INTERVAL) / 60.0);
        }

        const bool is_focused = predict_focus_state(wpm);
        engine.sweep(is_focused);

        std::cout << "[CATRA] WPM: " << std::fixed << std::setprecision(1) << wpm
                  << " | State: " << (is_focused ? "FOCUS" : "RELAX") << std::endl;
    }

    sensor.stop();
    // The AnchorEngine destructor also calls restore_all(), which is safe because restore_all() is idempotent.
    engine.restore_all();
    std::cout << "[CATRA] Daemon shutdown complete. Priorities cleanly restored." << std::endl;

    return 0;
}
