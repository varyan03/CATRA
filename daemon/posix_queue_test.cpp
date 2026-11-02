#include <iostream>
#include <sys/resource.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

int main() {
    const pid_t current_pid = getpid();
    std::cout << "[CATRA Scheduler Test] Target PID: " << current_pid << "\n";

    // Query initial priority.
    // getpriority() returns -1 on error, but -1 is also a valid nice value.
    // POSIX specification requires clearing errno before the call.
    errno = 0;
    const int initial_priority = getpriority(PRIO_PROCESS, current_pid);
    if (errno != 0) {
        std::cerr << "[-] Failed to read initial priority: " << std::strerror(errno) << "\n";
        return 1;
    }
    std::cout << "[+] Initial Process Priority (nice): " << initial_priority << "\n";

    // Attempt priority promotion to Tier 1 (-10)
    constexpr int target_nice = -10;
    std::cout << "[*] Attempting priority shift to " << target_nice << " (Tier 1 Focus)...\n";

    if (setpriority(PRIO_PROCESS, current_pid, target_nice) == -1) {
        const int err = errno;
        std::cerr << "[-] setpriority() failed: " << std::strerror(err) << "\n";
        if (err == EACCES || err == EPERM) {
            std::cerr << "[!] Permission denied: Lowering nice values (raising CPU priority) requires CAP_SYS_NICE.\n"
                      << "    Re-run the binary with root privileges: sudo ./posix_queue_test\n";
        }
        return 1;
    }

    // Verify kernel state transition
    errno = 0;
    const int updated_priority = getpriority(PRIO_PROCESS, current_pid);
    if (errno != 0) {
        std::cerr << "[-] Failed to read updated priority: " << std::strerror(errno) << "\n";
        return 1;
    }

    std::cout << "[+] Kernel Verified Priority (nice): " << updated_priority << "\n";
    if (updated_priority == target_nice) {
        std::cout << "[+] SUCCESS: POSIX scheduling priority override confirmed.\n";
    } else {
        std::cerr << "[-] MISMATCH: Expected " << target_nice << ", kernel reports " << updated_priority << "\n";
        return 1;
    }

    return 0;
}
