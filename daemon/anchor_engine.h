#pragma once

#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>
#include <chrono>
#include <csignal>
#include <sys/types.h>

extern volatile sig_atomic_t shutdown_requested;

struct ThreadState {
    pid_t tgid;
    unsigned long long starttime;
    unsigned long long last_total_time;
    int original_nice;
    bool is_boosted;
    int promotion_ticks;
    int demotion_ticks;
    bool active_this_sweep;
    double cpu_pct;
    int idle_sweeps;
};

struct ProcInfo {
    pid_t pid, ppid, pgid, tpgid;
    unsigned tty_nr;
    std::string comm;
};

/**
 * AnchorEngine
 *
 * Autonomous hybrid process discovery and scheduling engine.
 * Discovers the active user session TTY, builds process hierarchies,
 * measures thread CPU consumption, and manages per-thread nice promotion
 * and safe state recovery across restarts/SIGKILL.
 *
 * Threading Model:
 * All methods are strictly intended to be called from a single controller thread.
 */
class AnchorEngine {
public:
    AnchorEngine();
    ~AnchorEngine();

    AnchorEngine(const AnchorEngine&) = delete;
    AnchorEngine& operator=(const AnchorEngine&) = delete;
    AnchorEngine(AnchorEngine&&) = delete;
    AnchorEngine& operator=(AnchorEngine&&) = delete;

    void sweep(bool ml_focus_signal);
    void restore_all();
    void check_autogroup() const;

private:
    long clock_ticks_per_sec_{100};
    std::chrono::steady_clock::time_point last_sweep_time_{std::chrono::steady_clock::now()};
    std::string state_dir_{"/run/catra"};
    std::string state_file_path_{"/run/catra/state.dat"};

    std::unordered_map<pid_t, ThreadState> threads_;
    std::string active_pts_path_;
    pid_t active_tpgid_{0};
    time_t newest_pts_atime_{0};

    void persist_state();
    void recover_state();
    void determine_state_paths();

    bool find_active_tty(uid_t& target_uid, unsigned int& pts_major, unsigned int& pts_minor);
    void collect_process_tree(uid_t target_uid, unsigned int pts_major, unsigned int pts_minor,
                              std::unordered_set<pid_t>& in_scope_pids);
};
