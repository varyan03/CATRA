#include <iostream>
#include <iomanip>
#include <vector>
#include <string_view>

extern "C" {
#include "header.h"
}

struct TelemetryTestCase {
    std::string_view scenario;
    float typing_speed_wpm;
    float window_switches_per_min;
};

int main() {
    const std::vector<TelemetryTestCase> test_suite = {
        {"Deep Focus",              90.0f,   1.0f},
        {"Total Idle (Relax)",       0.0f,   0.0f},
        {"Distracted/Multitasking", 25.0f,  15.0f},
        {"Frenetic/Chaotic",       130.0f,  25.0f},
        {"Micro-breaks",             5.0f,   2.0f}
    };

    std::cout << std::left 
              << std::setw(26) << "Scenario"
              << std::right 
              << std::setw(12) << "WPM"
              << std::setw(14) << "Switches/m"
              << std::setw(14) << "Prob(Focus)"
              << std::setw(12) << "Class"
              << "   State\n";
    std::cout << std::string(85, '-') << "\n";

    for (const auto& test : test_suite) {
        // Feature 0: typing_speed_wpm, Feature 1: window_switches_per_min
        union Entry feature_buffer[2];
        feature_buffer[0].fvalue = test.typing_speed_wpm;
        feature_buffer[1].fvalue = test.window_switches_per_min;

        // Treelite accumulates tree margins; explicit zero-init is mandatory
        float inference_output[1] = {0.0f};
        
        // pred_margin = 0 triggers postprocess() to apply the sigmoid transfer function
        predict(feature_buffer, 0, inference_output);

        float focus_probability = inference_output[0];
        int predicted_class = (focus_probability >= 0.5f) ? 1 : 0;
        std::string_view state_str = (predicted_class == 1) ? "Focus" : "Relax";

        std::cout << std::left 
                  << std::setw(26) << test.scenario
                  << std::right 
                  << std::fixed << std::setprecision(1)
                  << std::setw(12) << test.typing_speed_wpm
                  << std::setw(14) << test.window_switches_per_min
                  << std::setprecision(4)
                  << std::setw(14) << focus_probability
                  << std::setw(12) << predicted_class
                  << "   " << state_str << "\n";
    }

    return 0;
}
