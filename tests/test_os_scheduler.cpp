#include "scheduler.h"
#include <cassert>
#include <iostream>
#include <unistd.h>
#include <cerrno>
#include <sys/resource.h>

int main() {
    const pid_t pid = getpid();

    // Sudo Privilege Pre-check: Verify CAP_SYS_NICE before executing assertions
    errno = 0;
    if (setpriority(PRIO_PROCESS, pid, -1) == -1) {
        if (errno == EACCES || errno == EPERM) {
            std::cerr << "\033[1;31m[!] Unit tests must be run with sudo to test OS scheduling\033[0m\n";
            return 1;
        }
    }
    // Revert probe nice adjustment to baseline
    setpriority(PRIO_PROCESS, pid, 0);

    // Test 1: Apply Focus Priority (-10)
    assert(apply_focus_priority(pid));
    assert(get_process_priority(pid) == -10);

    // Test 2: Reset to Fair-Share Baseline (0)
    assert(reset_priority(pid));
    assert(get_process_priority(pid) == 0);

    std::cout << "\033[32m[+] OS Scheduler Unit Tests Passed: State transitions verified.\033[0m\n";
    return 0;
}
