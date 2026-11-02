#include "telemetry_sensor.h"
#include <iostream>
#include <unistd.h>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <device_path_or_fifo>\n";
        std::cerr << "Example: " << argv[0] << " /dev/input/event0\n";
        return 1;
    }

    const std::string device_path = argv[1];
    std::cout << "[*] Initializing KeyboardSensor on: " << device_path << "\n";

    KeyboardSensor sensor;
    if (!sensor.start(device_path)) {
        std::cerr << "[-] Failed to open device: " << device_path 
                  << " (check read permissions / root access)\n";
        return 1;
    }

    std::cout << "[+] Sensor thread active. Sampling keystrokes for up to 15 seconds...\n";

    for (int tick = 1; tick <= 15; ++tick) {
        sleep(1);

        const int count = sensor.get_and_reset_count();
        std::cout << "[+] Keystrokes in last 1s: " << count 
                  << " | Sensor active: " << (sensor.running() ? "true" : "false") << "\n";

        if (!sensor.running()) {
            const int remaining = sensor.get_and_reset_count();
            if (remaining > 0) {
                std::cout << "[+] Final buffered keystrokes drained: " << remaining << "\n";
            }
            std::cout << "[!] Sensor stream closed (EOF/HUP). Exiting.\n";
            break;
        }
    }

    std::cout << "[*] Stopping sensor and joining thread...\n";
    sensor.stop();
    return 0;
}
