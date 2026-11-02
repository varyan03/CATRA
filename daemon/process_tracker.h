#pragma once

#include <string>
#include <sys/types.h>

struct ProcessInfo {
    std::string cmdline;
    char state;
    unsigned long long start_time;
    bool is_valid;
};

ProcessInfo get_process_info(pid_t pid);
