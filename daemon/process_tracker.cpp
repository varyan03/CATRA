#include "process_tracker.h"
#include <fstream>
#include <string>

std::string get_process_name(pid_t pid) {
    std::ifstream comm_file("/proc/" + std::to_string(pid) + "/comm");
    if (!comm_file.is_open()) {
        return "UNKNOWN";
    }

    std::string process_name;
    if (std::getline(comm_file, process_name)) {
        // Strip trailing newline characters
        while (!process_name.empty() && (process_name.back() == '\n' || process_name.back() == '\r')) {
            process_name.pop_back();
        }
        return process_name.empty() ? "UNKNOWN" : process_name;
    }

    return "UNKNOWN";
}

char get_process_state(pid_t pid) {
    std::ifstream stat_file("/proc/" + std::to_string(pid) + "/stat");
    if (!stat_file.is_open()) {
        return 'X';
    }

    std::string stat_line;
    if (!std::getline(stat_file, stat_line)) {
        return 'X';
    }

    // Process names can contain spaces and parentheses: e.g., "1234 (Web Content) S ..."
    // Locate the last closing parenthesis to reliably skip the filename field
    const std::size_t last_paren_pos = stat_line.rfind(')');
    if (last_paren_pos == std::string::npos || (last_paren_pos + 2) >= stat_line.length()) {
        return 'X';
    }

    // State character follows ") "
    return stat_line[last_paren_pos + 2];
}
