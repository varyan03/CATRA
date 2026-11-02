#include "scheduler.h"
#include <sys/resource.h>
#include <cerrno>
#include <cstring>
#include <iostream>

bool apply_focus_priority(pid_t pid) {
    errno = 0;
    if (setpriority(PRIO_PROCESS, pid, -10) == -1 && errno != 0) {
        std::cerr << "[-] Failed to set Focus priority (-10) on PID " << pid << ": " << std::strerror(errno) << "\n";
        return false;
    }
    return true;
}

bool reset_priority(pid_t pid) {
    errno = 0;
    if (setpriority(PRIO_PROCESS, pid, 0) == -1 && errno != 0) {
        std::cerr << "[-] Failed to reset priority (0) on PID " << pid << ": " << std::strerror(errno) << "\n";
        return false;
    }
    return true;
}

int get_process_priority(pid_t pid) {
    errno = 0;
    const int priority = getpriority(PRIO_PROCESS, pid);
    if (errno != 0) {
        std::cerr << "[-] Failed to get priority for PID " << pid << ": " << std::strerror(errno) << "\n";
    }
    return priority;
}
