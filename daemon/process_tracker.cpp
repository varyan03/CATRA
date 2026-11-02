#include "process_tracker.h"
#include <fstream>
#include <sstream>
#include <vector>
#include <algorithm>

ProcessInfo get_process_info(pid_t pid) {
    // Step A: Parse /proc/[pid]/cmdline (bypasses 15-char comm limit)
    std::ifstream cmd_file("/proc/" + std::to_string(pid) + "/cmdline", std::ios::binary);
    if (!cmd_file.is_open()) {
        return {"", 'X', 0, false};
    }

    std::string raw_cmdline((std::istreambuf_iterator<char>(cmd_file)),
                            std::istreambuf_iterator<char>());
    if (raw_cmdline.empty()) {
        return {"", 'X', 0, false};
    }

    // Arguments are delimited by null bytes (\0); replace with spaces for single readable string
    for (char& ch : raw_cmdline) {
        if (ch == '\0') {
            ch = ' ';
        }
    }

    // Strip trailing space
    while (!raw_cmdline.empty() && (raw_cmdline.back() == ' ' || raw_cmdline.back() == '\n' || raw_cmdline.back() == '\r')) {
        raw_cmdline.pop_back();
    }

    // Step B: Safely parse /proc/[pid]/stat (handles spaces and parentheses in process name)
    std::ifstream stat_file("/proc/" + std::to_string(pid) + "/stat");
    if (!stat_file.is_open()) {
        return {"", 'X', 0, false};
    }

    std::string stat_line;
    if (!std::getline(stat_file, stat_line)) {
        return {"", 'X', 0, false};
    }

    const std::size_t last_paren = stat_line.rfind(')');
    if (last_paren == std::string::npos || (last_paren + 2) >= stat_line.length()) {
        return {"", 'X', 0, false};
    }

    const char state = stat_line[last_paren + 2];
    if ((last_paren + 4) >= stat_line.length()) {
        return {"", 'X', 0, false};
    }

    // Parse remaining stat substring starting after the state character
    std::istringstream iss(stat_line.substr(last_paren + 4));
    std::string token;
    
    // start_time is the 19th token in this remaining substring (field 22 overall)
    for (int i = 0; i < 18; ++i) {
        if (!(iss >> token)) {
            return {"", 'X', 0, false};
        }
    }

    unsigned long long start_time = 0;
    if (!(iss >> start_time)) {
        return {"", 'X', 0, false};
    }

    return {raw_cmdline, state, start_time, true};
}
