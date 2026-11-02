#include <iostream>
#include <iomanip>
#include <string>
#include <chrono>
#include <limits>
#include <unistd.h>

#include "scheduler.h"

extern "C" {
#include "main.c"
}

int main() {
    const pid_t pid = getpid();

    std::cout << "========================================================\n";
    std::cout << "  [CATRA] Daemon Initialized. Target PID: " << pid << "\n";
    std::cout << "  Context-Aware Tiered Resource Allocation Daemon\n";
    std::cout << "========================================================\n";

    while (true) {
        std::cout << "\n--------------------------------------------------------\n";
        std::cout << "[*] Press [Enter] to begin telemetry interval (or 'q' to quit)... ";
        
        std::string init_prompt;
        if (!std::getline(std::cin, init_prompt)) {
            break;
        }
        if (!init_prompt.empty() && (init_prompt[0] == 'q' || init_prompt[0] == 'Q')) {
            std::cout << "\n[!] Shutting down CATRA daemon. Resetting priority...\n";
            reset_priority(pid);
            break;
        }

        std::cout << "[*] Work burst active. Type below and press [Enter] when done:\n> ";

        const auto start_timestamp = std::chrono::steady_clock::now();
        std::string typed_text;
        std::getline(std::cin, typed_text);
        const auto end_timestamp = std::chrono::steady_clock::now();

        const std::chrono::duration<float> elapsed_duration = end_timestamp - start_timestamp;
        const float elapsed_seconds = elapsed_duration.count();

        // Standard typing speed metric: (characters / 5.0) / (minutes)
        float calculated_wpm = 0.0f;
        if (elapsed_seconds > 0.05f && !typed_text.empty()) {
            calculated_wpm = (static_cast<float>(typed_text.length()) / 5.0f) / (elapsed_seconds / 60.0f);
        }

        std::cout << "[*] Enter simulated window switches for this interval: ";
        std::string switches_input;
        float window_switches = 0.0f;
        if (std::getline(std::cin, switches_input) && !switches_input.empty()) {
            try {
                window_switches = std::stof(switches_input);
            } catch (...) {
                window_switches = 0.0f;
            }
        }

        // 1. Ingestion: Map features into Treelite union buffer
        union Entry feature_buffer[2];
        feature_buffer[0].fvalue = calculated_wpm;
        feature_buffer[1].fvalue = window_switches;

        // 2. Inference: Evaluate decision tree ensemble
        float inference_output[1] = {0.0f};
        predict(feature_buffer, 0, inference_output);
        const float focus_probability = inference_output[0];

        // 3. Actuation: Reallocate CPU priorities via POSIX scheduler
        const bool is_focus = (focus_probability >= 0.5f);
        if (is_focus) {
            apply_focus_priority(pid);
        } else {
            reset_priority(pid);
        }

        // 4. Kernel Verification: Query actual nice value from the scheduler
        const int verified_nice = get_process_priority(pid);

        // 5. Live Telemetry & Actuation Dashboard
        std::cout << "\n================= DAEMON ACTUATION STATUS =================\n";
        std::cout << std::fixed << std::setprecision(1);
        std::cout << "  Telemetry Input  : " << calculated_wpm << " WPM | " 
                  << window_switches << " switches/min (" << elapsed_seconds << "s elapsed)\n";
        std::cout << std::setprecision(4);
        std::cout << "  ML Probability   : " << focus_probability << " (" << (focus_probability * 100.0f) << "%)\n";
        std::cout << "  Predicted State  : " << (is_focus ? "\033[1;32mFocus (Tier 1)\033[0m" : "\033[1;33mRelax (Tier 2)\033[0m") << "\n";
        std::cout << "  Kernel Nice Value: " << verified_nice 
                  << (verified_nice < 0 ? " [PROMOTED]" : " [STANDARD]") << "\n";
        std::cout << "===========================================================\n";
    }

    return 0;
}
