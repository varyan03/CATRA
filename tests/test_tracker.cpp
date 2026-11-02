#include "../daemon/process_tracker.h"
#include <iostream>
#include <unistd.h>

int main() {
    std::cout << "Enter a PID to track: ";
    pid_t pid = 0;
    if (!(std::cin >> pid) || pid <= 0) {
        std::cerr << "[-] Invalid PID.\n";
        return 1;
    }

    ProcessInfo initial_info = get_process_info(pid);
    if (!initial_info.is_valid) {
        std::cerr << "[-] Process " << pid << " not found, inaccessible, or has empty cmdline.\n";
        return 1;
    }

    const unsigned long long initial_start_time = initial_info.start_time;
    std::cout << "[*] Monitoring PID " << pid << " (start_time: " << initial_start_time << ")...\n";

    while (true) {
        const ProcessInfo info = get_process_info(pid);

        if (!info.is_valid) {
            std::cout << "[!] Process " << pid << " terminated or inaccessible.\n";
            break;
        }
        if (info.state == 'X') {
            std::cout << "[!] Process " << pid << " is Dead ('X').\n";
            break;
        }
        if (info.state == 'Z') {
            std::cout << "[!] Process " << pid << " entered Zombie state ('Z').\n";
            break;
        }
        if (info.start_time != initial_start_time) {
            std::cout << "[!] PID " << pid << " reuse detected (start_time mismatch: " 
                      << initial_start_time << " -> " << info.start_time << ").\n";
            break;
        }

        std::cout << "[+] PID: " << pid 
                  << " | State: " << info.state 
                  << " | Name: " << info.cmdline << "\n";

        sleep(1);
    }

    return 0;
}
