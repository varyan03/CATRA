#include "anchor_engine.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <queue>
#include <cstring>
#include <cerrno>

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/resource.h>

#if defined(__has_include) && __has_include(<sys/sysmacros.h>)
#include <sys/sysmacros.h>
#endif

// Global shutdown flag requested by POSIX signal handlers
volatile sig_atomic_t shutdown_requested = 0;

namespace {

const std::unordered_set<std::string> kExcludedComms = {
    "bash", "zsh", "sh", "fish", "dash", "tmux", "screen", "sshd", "login"
};

struct DetailedStat {
    pid_t pid{0};
    std::string comm;
    char state{'X'};
    pid_t ppid{0};
    pid_t pgid{0};
    pid_t session{0};
    unsigned int tty_nr{0};
    pid_t tpgid{0};
    unsigned long utime{0};
    unsigned long stime{0};
    unsigned long long starttime{0};
    bool valid{false};
};

// Robust shared parser for /proc/[pid]/stat and /proc/[pid]/task/[tid]/stat
DetailedStat parse_stat_line(const std::string& filepath) {
    DetailedStat result;
    std::ifstream stat_file(filepath);
    if (!stat_file.is_open()) {
        return result;
    }

    std::string line;
    if (!std::getline(stat_file, line)) {
        return result;
    }

    const std::size_t first_paren = line.find('(');
    const std::size_t last_paren = line.rfind(')');
    if (first_paren == std::string::npos || last_paren == std::string::npos || first_paren >= last_paren) {
        return result;
    }

    // Extract PID from prefix before '('
    try {
        result.pid = std::stoi(line.substr(0, first_paren));
    } catch (...) {
        return result;
    }

    result.comm = line.substr(first_paren + 1, last_paren - first_paren - 1);

    if ((last_paren + 2) >= line.length()) {
        return result;
    }

    // Parse tokens following ") "
    std::istringstream iss(line.substr(last_paren + 2));
    std::string token;
    std::vector<std::string> tokens;
    tokens.reserve(35);

    while (iss >> token) {
        tokens.push_back(token);
    }

    // Need at least 20 tokens to reach starttime (field 22 overall)
    if (tokens.size() < 20) {
        return result;
    }

    try {
        result.state = tokens[0][0];                           // field 3
        result.ppid = static_cast<pid_t>(std::stol(tokens[1])); // field 4
        result.pgid = static_cast<pid_t>(std::stol(tokens[2])); // field 5
        result.session = static_cast<pid_t>(std::stol(tokens[3])); // field 6
        result.tty_nr = static_cast<unsigned int>(std::stoul(tokens[4])); // field 7
        result.tpgid = static_cast<pid_t>(std::stol(tokens[5])); // field 8

        result.utime = std::stoul(tokens[11]);                  // field 14
        result.stime = std::stoul(tokens[12]);                  // field 15
        result.starttime = std::stoull(tokens[19]);             // field 22
        result.valid = true;
    } catch (...) {
        result.valid = false;
    }

    return result;
}

} // namespace

AnchorEngine::AnchorEngine() {
    long sc_clk = sysconf(_SC_CLK_TCK);
    clock_ticks_per_sec_ = (sc_clk > 0) ? sc_clk : 100;
    last_sweep_time_ = std::chrono::steady_clock::time_point{};

    determine_state_paths();
    check_autogroup();
    recover_state();
}

AnchorEngine::~AnchorEngine() {
    restore_all();
}

void AnchorEngine::determine_state_paths() {
    // Standard secure state location
    if (getuid() == 0) {
        state_dir_ = "/run/catra";
    } else {
        const char* xdg_dir = std::getenv("XDG_RUNTIME_DIR");
        if (xdg_dir && strlen(xdg_dir) > 0) {
            state_dir_ = std::string(xdg_dir) + "/catra";
        } else {
            state_dir_ = "/tmp/catra_" + std::to_string(getuid());
        }
    }

    state_file_path_ = state_dir_ + "/state.dat";

    // Ensure directory exists with 0700 permissions
    struct stat st{};
    if (stat(state_dir_.c_str(), &st) != 0) {
        mkdir(state_dir_.c_str(), 0700);
    } else {
        chmod(state_dir_.c_str(), 0700);
    }
}

void AnchorEngine::check_autogroup() const {
    std::ifstream autogroup_file("/proc/sys/kernel/sched_autogroup_enabled");
    if (autogroup_file.is_open()) {
        int enabled = 0;
        if (autogroup_file >> enabled && enabled == 1) {
            std::cerr << "\n[!] WARNING: CFS sched_autogroup is enabled (kernel.sched_autogroup_enabled = 1).\n"
                      << "    Thread nice priorities only compete within their terminal session autogroup.\n"
                      << "    To allow system-wide CPU tier prioritization, disable autogrouping:\n"
                      << "    sudo sysctl -w kernel.sched_autogroup_enabled=0\n\n";
        }
    }
}

bool AnchorEngine::find_active_tty(uid_t& target_uid, unsigned int& pts_major, unsigned int& pts_minor) {
    DIR* pts_dir = opendir("/dev/pts");
    if (!pts_dir) {
        return false;
    }

    time_t newest_atime = 0;
    std::string newest_path;
    struct stat newest_st{};

    struct dirent* entry = nullptr;
    while ((entry = readdir(pts_dir)) != nullptr) {
        // Skip ptmx and current/parent directories
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0 || strcmp(entry->d_name, "ptmx") == 0) {
            continue;
        }

        std::string pts_path = std::string("/dev/pts/") + entry->d_name;
        struct stat st{};
        if (stat(pts_path.c_str(), &st) == 0 && S_ISCHR(st.st_mode)) {
            if (st.st_atime > newest_atime) {
                newest_atime = st.st_atime;
                newest_path = pts_path;
                newest_st = st;
            }
        }
    }
    closedir(pts_dir);

    if (newest_atime == 0) {
        return false;
    }

    target_uid = newest_st.st_uid;
    newest_pts_atime_ = newest_atime;
    active_pts_path_ = newest_path;

#if defined(major) && defined(minor)
    pts_major = major(newest_st.st_rdev);
    pts_minor = minor(newest_st.st_rdev);
#else
    pts_major = (newest_st.st_rdev >> 8) & 0xfff;
    pts_minor = (newest_st.st_rdev & 0xff);
#endif

    return true;
}

void AnchorEngine::collect_process_tree(uid_t target_uid, unsigned int pts_major, unsigned int pts_minor,
                                        std::unordered_set<pid_t>& in_scope_pids) {
    DIR* proc_dir = opendir("/proc");
    if (!proc_dir) {
        return;
    }

    std::unordered_map<pid_t, std::vector<pid_t>> children_map;
    std::vector<pid_t> root_pids;
    active_tpgid_ = 0;

    struct dirent* entry = nullptr;
    while ((entry = readdir(proc_dir)) != nullptr) {
        if (entry->d_type != DT_DIR || !isdigit(entry->d_name[0])) {
            continue;
        }

        try {
            (void)std::stol(entry->d_name);
        } catch (...) {
            continue;
        }

        std::string pid_path = std::string("/proc/") + entry->d_name;
        struct stat st{};
        if (stat(pid_path.c_str(), &st) != 0 || st.st_uid != target_uid) {
            continue;
        }

        DetailedStat stat_info = parse_stat_line(pid_path + "/stat");
        if (!stat_info.valid) {
            continue;
        }

        // Exclude system shells and session leaders from active candidate scope
        if (kExcludedComms.count(stat_info.comm) > 0 || stat_info.pid == stat_info.session) {
            continue;
        }

        children_map[stat_info.ppid].push_back(stat_info.pid);

        // Decode Linux tty_nr: major = bits 8-19, minor = bits 0-7 | bits 20-31
        unsigned int proc_major = (stat_info.tty_nr >> 8) & 0xfff;
        unsigned int proc_minor = (stat_info.tty_nr & 0xff) | ((stat_info.tty_nr >> 12) & 0xfff00);

        if (proc_major == pts_major && proc_minor == pts_minor) {
            root_pids.push_back(stat_info.pid);
            if (active_tpgid_ == 0 && stat_info.tpgid > 0) {
                active_tpgid_ = stat_info.tpgid;
            }
        }
    }
    closedir(proc_dir);

    // BFS tree walk from session roots down through all descendants
    std::queue<pid_t> q;
    for (pid_t root : root_pids) {
        q.push(root);
        in_scope_pids.insert(root);
    }

    while (!q.empty()) {
        pid_t curr = q.front();
        q.pop();

        auto it = children_map.find(curr);
        if (it != children_map.end()) {
            for (pid_t child : it->second) {
                if (in_scope_pids.insert(child).second) {
                    q.push(child);
                }
            }
        }
    }
}

void AnchorEngine::sweep(bool ml_focus_signal) {
    const auto now = std::chrono::steady_clock::now();
    const bool is_first_sweep = (last_sweep_time_ == std::chrono::steady_clock::time_point{});
    const double elapsed_secs = is_first_sweep
        ? 1.0
        : std::chrono::duration<double>(now - last_sweep_time_).count();

    if (!is_first_sweep && elapsed_secs < 0.05) {
        return;
    }

    // ==========================================
    // Phase A: Measurement & Accounting
    // ==========================================
    for (auto& [tid, state] : threads_) {
        state.active_this_sweep = false;
    }

    uid_t target_uid = 0;
    unsigned int pts_major = 0;
    unsigned int pts_minor = 0;
    bool has_active_tty = find_active_tty(target_uid, pts_major, pts_minor);

    std::unordered_set<pid_t> in_scope_pids;
    if (has_active_tty && target_uid != 0) {
        collect_process_tree(target_uid, pts_major, pts_minor, in_scope_pids);
    }

    for (pid_t pid : in_scope_pids) {
        std::string task_dir_path = "/proc/" + std::to_string(pid) + "/task";
        DIR* task_dir = opendir(task_dir_path.c_str());
        if (!task_dir) {
            continue;
        }

        struct dirent* entry = nullptr;
        while ((entry = readdir(task_dir)) != nullptr) {
            if (!isdigit(entry->d_name[0])) {
                continue;
            }

            pid_t tid = 0;
            try {
                tid = static_cast<pid_t>(std::stol(entry->d_name));
            } catch (...) {
                continue;
            }

            std::string tid_stat_path = task_dir_path + "/" + entry->d_name + "/stat";
            DetailedStat stat_info = parse_stat_line(tid_stat_path);
            if (!stat_info.valid) {
                continue;
            }

            const unsigned long long total_time = stat_info.utime + stat_info.stime;

            auto it = threads_.find(tid);
            if (it == threads_.end()) {
                // New thread observed: initialize without computing CPU delta on first sight
                ThreadState st{};
                st.tgid = pid;
                st.starttime = stat_info.starttime;
                st.last_total_time = total_time;
                st.is_boosted = false;
                st.promotion_ticks = 0;
                st.demotion_ticks = 0;
                st.cpu_pct = 0.0;
                st.active_this_sweep = true;
                st.idle_sweeps = 0;

                errno = 0;
                int cur_nice = getpriority(PRIO_PROCESS, tid);

                // Inherited nice check: only if parent thread group already contains an actively boosted thread
                bool inherited_boost = false;
                int parent_orig_nice = 0;
                for (const auto& [sibling_tid, sibling_st] : threads_) {
                    if (sibling_st.tgid == pid && sibling_st.is_boosted) {
                        inherited_boost = true;
                        parent_orig_nice = sibling_st.original_nice;
                        break;
                    }
                }

                if (inherited_boost && cur_nice == -5) {
                    st.original_nice = parent_orig_nice;
                } else {
                    st.original_nice = (errno == 0) ? cur_nice : 0;
                }

                threads_[tid] = st;
            } else if (it->second.starttime != stat_info.starttime) {
                // PID reuse detected: reset tracking state
                it->second.starttime = stat_info.starttime;
                it->second.tgid = pid;
                it->second.last_total_time = total_time;
                it->second.is_boosted = false;
                it->second.promotion_ticks = 0;
                it->second.demotion_ticks = 0;
                it->second.cpu_pct = 0.0;
                it->second.active_this_sweep = true;
                it->second.idle_sweeps = 0;

                errno = 0;
                int cur_nice = getpriority(PRIO_PROCESS, tid);
                it->second.original_nice = (errno == 0) ? cur_nice : 0;
            } else {
                // Compute non-negative tick differential
                unsigned long long delta = 0;
                if (total_time >= it->second.last_total_time) {
                    delta = total_time - it->second.last_total_time;
                }
                it->second.last_total_time = total_time;
                it->second.cpu_pct = (static_cast<double>(delta) / clock_ticks_per_sec_ / elapsed_secs) * 100.0;
                it->second.active_this_sweep = true;
                it->second.idle_sweeps = 0;
            }
        }
        closedir(task_dir);
    }

    last_sweep_time_ = now;

    // ==========================================
    // Phase B: Decision, Actuation & GC
    // ==========================================
    const auto system_now_secs = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    const double atime_age = std::difftime(system_now_secs, newest_pts_atime_);
    const bool atime_warm = (atime_age < 30.0);
    const bool is_session_warm = (atime_warm || ml_focus_signal);

    std::cout << "[AnchorEngine] State: TTY=" << active_pts_path_
              << " | atime_age=" << static_cast<int>(atime_age) << "s (warm=" << (atime_warm ? "yes" : "no")
              << ") | ml_signal=" << (ml_focus_signal ? "FOCUS" : "RELAX")
              << " => SessionWarm=" << (is_session_warm ? "TRUE" : "FALSE") << "\n";

    bool state_modified = false;
    std::vector<pid_t> to_erase;

    for (auto& [tid, st] : threads_) {
        if (st.active_this_sweep && st.cpu_pct > 15.0 && is_session_warm) {
            st.promotion_ticks++;
            st.demotion_ticks = 0;

            if (st.promotion_ticks >= 2 && !st.is_boosted) {
                // Re-verify thread identity before applying kernel priority boost
                DetailedStat verify_stat = parse_stat_line("/proc/" + std::to_string(st.tgid) + "/task/" + std::to_string(tid) + "/stat");
                if (verify_stat.valid && verify_stat.starttime == st.starttime) {
                    errno = 0;
                    int cur_nice = getpriority(PRIO_PROCESS, tid);
                    if (errno == 0 && cur_nice != -5) {
                        st.original_nice = cur_nice;
                    }

                    if (setpriority(PRIO_PROCESS, tid, -5) == 0) {
                        st.is_boosted = true;
                        state_modified = true;
                        std::cout << "[AnchorEngine] Promoted TID " << tid << " (" << verify_stat.comm 
                                  << ") to Tier 1 (-5) | CPU: " << st.cpu_pct << "%\n";
                    }
                }
            }
        } else if (!st.active_this_sweep || st.cpu_pct < 5.0 || !is_session_warm) {
            st.demotion_ticks++;
            st.promotion_ticks = 0;

            if (st.demotion_ticks >= 3 && st.is_boosted) {
                DetailedStat verify_stat = parse_stat_line("/proc/" + std::to_string(st.tgid) + "/task/" + std::to_string(tid) + "/stat");
                if (!verify_stat.valid) {
                    to_erase.push_back(tid);
                    state_modified = true;
                } else if (verify_stat.starttime != st.starttime) {
                    to_erase.push_back(tid);
                    state_modified = true;
                } else {
                    errno = 0;
                    int cur_nice = getpriority(PRIO_PROCESS, tid);
                    if (errno == 0 && cur_nice == -5) {
                        setpriority(PRIO_PROCESS, tid, st.original_nice);
                    }
                    st.is_boosted = false;
                    state_modified = true;
                    std::cout << "[AnchorEngine] Demoted TID " << tid << " back to baseline (" 
                              << st.original_nice << ") | CPU: " << st.cpu_pct << "%\n";
                }
            }
        }

        // Garbage collection for unboosted stale entries
        if (!st.is_boosted) {
            std::string verify_path = "/proc/" + std::to_string(st.tgid) + "/task/" + std::to_string(tid) + "/stat";
            struct stat chk{};
            if (stat(verify_path.c_str(), &chk) != 0) {
                to_erase.push_back(tid);
            } else if (!st.active_this_sweep) {
                st.idle_sweeps++;
                if (st.idle_sweeps > 20) {
                    to_erase.push_back(tid);
                }
            }
        }
    }

    for (pid_t dead_tid : to_erase) {
        threads_.erase(dead_tid);
    }

    if (state_modified) {
        persist_state();
    }
}

void AnchorEngine::persist_state() {
    std::string temp_file = state_dir_ + "/state.dat.tmp." + std::to_string(getpid());
    std::ofstream out(temp_file, std::ios::trunc);
    if (!out.is_open()) {
        return;
    }

    chmod(temp_file.c_str(), 0600);

    bool has_boosted = false;
    for (const auto& [tid, st] : threads_) {
        if (st.is_boosted) {
            out << tid << ":" << st.starttime << ":" << st.original_nice << "\n";
            has_boosted = true;
        }
    }

    out.flush();
    out.close();

    if (has_boosted) {
        // Atomic replace
        rename(temp_file.c_str(), state_file_path_.c_str());
    } else {
        unlink(temp_file.c_str());
        unlink(state_file_path_.c_str());
    }
}

void AnchorEngine::recover_state() {
    struct stat st{};
    if (stat(state_file_path_.c_str(), &st) != 0) {
        return;
    }

    // Security check: must be owned by current user (or root) and mode 0600
    if ((st.st_mode & 0777) != 0600) {
        unlink(state_file_path_.c_str());
        return;
    }
    if (getuid() == 0 && st.st_uid != 0) {
        unlink(state_file_path_.c_str());
        return;
    }

    std::ifstream in(state_file_path_);
    if (!in.is_open()) {
        return;
    }

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }

        std::size_t c1 = line.find(':');
        std::size_t c2 = (c1 != std::string::npos) ? line.find(':', c1 + 1) : std::string::npos;
        if (c1 == std::string::npos || c2 == std::string::npos) {
            continue;
        }

        try {
            pid_t tid = static_cast<pid_t>(std::stol(line.substr(0, c1)));
            unsigned long long starttime = std::stoull(line.substr(c1 + 1, c2 - c1 - 1));
            int orig_nice = std::stoi(line.substr(c2 + 1));

            DetailedStat cur_stat = parse_stat_line("/proc/" + std::to_string(tid) + "/stat");
            if (cur_stat.valid && cur_stat.starttime == starttime) {
                errno = 0;
                int cur_nice = getpriority(PRIO_PROCESS, tid);
                if (errno == 0 && cur_nice == -5) {
                    setpriority(PRIO_PROCESS, tid, orig_nice);
                    std::cout << "[AnchorEngine] Crash Recovery: Restored TID " << tid << " nice -> " << orig_nice << "\n";
                }
            }
        } catch (...) {
            continue;
        }
    }

    in.close();
    unlink(state_file_path_.c_str());
}

void AnchorEngine::restore_all() {
    for (auto& [tid, st] : threads_) {
        if (st.is_boosted) {
            DetailedStat cur_stat = parse_stat_line("/proc/" + std::to_string(st.tgid) + "/task/" + std::to_string(tid) + "/stat");
            if (cur_stat.valid && cur_stat.starttime == st.starttime) {
                errno = 0;
                int cur_nice = getpriority(PRIO_PROCESS, tid);
                if (errno == 0 && cur_nice == -5) {
                    setpriority(PRIO_PROCESS, tid, st.original_nice);
                }
            }
            st.is_boosted = false;
        }
    }

    threads_.clear();
    unlink(state_file_path_.c_str());
}
