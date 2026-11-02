#include <iostream>
#include <iomanip>
#include <string>
#include <chrono>

extern "C" {
#include "../daemon/main.c"
}

int main() {
    std::cout << "====================================================\n";
    std::cout << "   CATRA Terminal Telemetry & Inference Validation\n";
    std::cout << "====================================================\n\n";

    std::cout << "Press [Enter] when ready to start typing...";
    std::string start_trigger;
    std::getline(std::cin, start_trigger);

    std::cout << "\nType the following sentence as quickly and naturally as possible:\n";
    std::cout << "\"The quick brown fox jumps over the lazy dog near the riverbank.\"\n\n";
    std::cout << "> ";

    const auto start_timestamp = std::chrono::steady_clock::now();
    std::string typed_text;
    std::getline(std::cin, typed_text);
    const auto end_timestamp = std::chrono::steady_clock::now();

    const std::chrono::duration<float> elapsed_duration = end_timestamp - start_timestamp;
    const float elapsed_seconds = elapsed_duration.count();

    // Standard typing WPM metric: (total_characters / 5.0) / (elapsed_seconds / 60.0)
    float calculated_wpm = 0.0f;
    if (elapsed_seconds > 0.05f && !typed_text.empty()) {
        calculated_wpm = (static_cast<float>(typed_text.length()) / 5.0f) / (elapsed_seconds / 60.0f);
    }

    std::cout << "\nEnter simulated window switches for this session: ";
    float simulated_window_switches = 0.0f;
    if (!(std::cin >> simulated_window_switches)) {
        simulated_window_switches = 0.0f;
    }

    // Format input array for Treelite decision tree prediction
    union Entry feature_buffer[2];
    feature_buffer[0].fvalue = calculated_wpm;
    feature_buffer[1].fvalue = simulated_window_switches;

    // Treelite accumulates margins across trees; explicit zero initialization is required
    float inference_output[1] = {0.0f};
    predict(feature_buffer, 0, inference_output);

    const float focus_probability = inference_output[0];
    const int predicted_state = (focus_probability >= 0.5f) ? 1 : 0;
    const std::string_view state_label = (predicted_state == 1) ? "Focus (1)" : "Relax (0)";

    std::cout << "\n---------------- Telemetry & Inference Report ----------------\n";
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Characters Typed:          " << typed_text.length() << "\n";
    std::cout << "Elapsed Time:              " << elapsed_seconds << " seconds\n";
    std::cout << "Calculated Typing Speed:   " << calculated_wpm << " WPM\n";
    std::cout << "Simulated Window Switches: " << simulated_window_switches << " / min\n";
    std::cout << std::setprecision(4);
    std::cout << "Model Focus Probability:   " << focus_probability << " (" << (focus_probability * 100.0f) << "%)\n";
    std::cout << "Predicted Allocation Tier: " << state_label << "\n";
    std::cout << "--------------------------------------------------------------\n";

    return 0;
}
