#include "process_tracker.h"
#include <iostream>
#include <unistd.h>

int main() {
    std::cout << "Enter a PID to track: ";
    pid_t pid = 0;
    if (!(std::cin >> pid) || pid <= 0) {
        std::cerr << "[-] Invalid PID.\n";
        return 1;
    }

    std::cout << "[*] Monitoring PID " << pid << " (/proc tracking loop active)...\n";

    while (true) {
        const std::string name = get_process_name(pid);
        const char state = get_process_state(pid);

        std::cout << "[+] PID: " << pid 
                  << " | Name: " << name 
                  << " | State: " << state << "\n";

        if (state == 'X') {
            std::cout << "[!] Process " << pid << " is terminated or unreachable. Stopping tracker.\n";
            break;
        }

        sleep(1);
    }

    return 0;
}
