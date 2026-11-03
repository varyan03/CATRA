#if __has_include("anchor_engine.h")
#include "anchor_engine.h"
#elif __has_include("../daemon/anchor_engine.h")
#include "../daemon/anchor_engine.h"
#else
#include "daemon/anchor_engine.h"
#endif

#include <iostream>
#include <cassert>
#include <cstring>
#include <cerrno>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <csignal>
#include <chrono>
#include <thread>

#if defined(__linux__) && __has_include(<sys/prctl.h>)
#include <sys/prctl.h>
#endif

extern volatile sig_atomic_t shutdown_requested;

namespace {
pid_t g_child_pid = 0;

void signal_cleanup_handler(int sig) {
    (void)sig;
    shutdown_requested = 1;
    if (g_child_pid > 0) {
        kill(g_child_pid, SIGKILL);
        waitpid(g_child_pid, nullptr, 0);
        g_child_pid = 0;
    }
    _exit(1);
}

void install_signal_handlers() {
    struct sigaction sa{};
    sa.sa_handler = signal_cleanup_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

void cpu_burner_loop() {
    while (true) {
        volatile double x = 1.0;
        x = x * 1.01;
    }
}
} // namespace

int main() {
    install_signal_handlers();

    // Privilege pre-check: setting nice -5 requires CAP_SYS_NICE or root
    errno = 0;
    if (setpriority(PRIO_PROCESS, getpid(), -1) == -1) {
        if (errno == EACCES || errno == EPERM) {
            std::cerr << "\033[1;31m[!] Automated test requires root privileges (sudo) to verify negative nice scheduling.\033[0m\n";
            return 1;
        }
    }
    setpriority(PRIO_PROCESS, getpid(), 0);

    std::cout << "========================================================\n";
    std::cout << "  [CATRA] Automated Anchor Engine Integration Test\n";
    std::cout << "========================================================\n\n";

    // Touch the current terminal so find_active_tty explicitly prioritizes this session
    const char* my_tty = ttyname(STDIN_FILENO);
    if (my_tty) {
        struct timeval now_tv[2];
        gettimeofday(&now_tv[0], nullptr);
        now_tv[1] = now_tv[0];
        utimes(my_tty, now_tv);
    }

    pid_t child_pid = fork();
    if (child_pid < 0) {
        std::cerr << "[!] fork() failed: " << std::strerror(errno) << "\n";
        return 1;
    }

    if (child_pid == 0) {
        // Drop root privileges to match terminal owner UID if running under sudo
        uid_t target_user_uid = 0;
        gid_t target_user_gid = 0;

        if (my_tty) {
            struct stat tty_st{};
            if (stat(my_tty, &tty_st) == 0 && tty_st.st_uid != 0) {
                target_user_uid = tty_st.st_uid;
                target_user_gid = tty_st.st_gid;
            }
        }

        if (target_user_uid == 0) {
            const char* sudo_uid_str = std::getenv("SUDO_UID");
            const char* sudo_gid_str = std::getenv("SUDO_GID");
            if (sudo_uid_str != nullptr) {
                target_user_uid = static_cast<uid_t>(std::stoul(sudo_uid_str));
                target_user_gid = sudo_gid_str ? static_cast<gid_t>(std::stoul(sudo_gid_str)) : target_user_uid;
            }
        }

        if (target_user_uid != 0 && getuid() == 0) {
            setgid(target_user_gid);
            setuid(target_user_uid);
#if defined(PR_SET_DUMPABLE)
            prctl(PR_SET_DUMPABLE, 1);
#endif
        }

        cpu_burner_loop();
        _exit(0);
    }

    g_child_pid = child_pid;

    // Small delay to let child initialize setuid/setgid and spin up burner loop
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    errno = 0;
    const int original_nice = getpriority(PRIO_PROCESS, child_pid);

    auto fail_test = [&](const std::string& error_msg) {
        std::cerr << "\n\033[1;31m[FAIL] " << error_msg << "\033[0m\n";
        if (child_pid > 0) {
            kill(child_pid, SIGKILL);
            waitpid(child_pid, nullptr, 0);
            g_child_pid = 0;
        }
        return 1;
    };

    AnchorEngine engine;

    // ==========================================
    // Phase 1: Promotion (Focus Mode)
    // ==========================================
    std::cout << "[1/3] Simulating Focus mode and heavy CPU load...\n";
    for (int i = 0; i < 3; ++i) {
        engine.sweep(true);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    errno = 0;
    int boosted_priority = getpriority(PRIO_PROCESS, child_pid);
    if (errno != 0) {
        return fail_test("Failed to query child priority: " + std::string(std::strerror(errno)));
    }

    if (boosted_priority != -5) {
        // Fallback: sweep once more in case process scheduling delayed initial tick
        engine.sweep(true);
        boosted_priority = getpriority(PRIO_PROCESS, child_pid);
    }

    if (boosted_priority != -5) {
        return fail_test("Priority assertion failed! Expected nice -5, but got: " + std::to_string(boosted_priority));
    }
    std::cout << "[+] Phase 1 Verified: Child process successfully boosted to nice " << boosted_priority << ".\n\n";

    // ==========================================
    // Phase 2: Demotion (Relax Mode)
    // ==========================================
    std::cout << "[2/3] Simulating Relax mode...\n";
    // Pause burner workload so thread CPU drops below 5.0% threshold
    kill(child_pid, SIGSTOP);

    // Age the active terminal atime so is_session_warm evaluates to false
    const char* tty_name = ttyname(STDIN_FILENO);
    if (tty_name) {
        struct timeval tv[2];
        tv[0].tv_sec = time(nullptr) - 45;
        tv[0].tv_usec = 0;
        tv[1].tv_sec = time(nullptr) - 45;
        tv[1].tv_usec = 0;
        utimes(tty_name, tv);
    }

    for (int i = 0; i < 4; ++i) {
        engine.sweep(false);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    errno = 0;
    int restored_priority = getpriority(PRIO_PROCESS, child_pid);
    if (errno != 0) {
        return fail_test("Failed to query child priority after demotion: " + std::string(std::strerror(errno)));
    }

    if (restored_priority != original_nice) {
        // Fallback: sweep once more in case of cloud VM scheduling jitter
        engine.sweep(false);
        restored_priority = getpriority(PRIO_PROCESS, child_pid);
    }

    if (restored_priority != original_nice) {
        return fail_test("Demotion assertion failed! Expected nice " + std::to_string(original_nice) +
                         ", but got: " + std::to_string(restored_priority));
    }
    std::cout << "[+] Phase 2 Verified: Child process priority restored to baseline " << restored_priority << ".\n\n";

    // ==========================================
    // Phase 3: Cleanup
    // ==========================================
    std::cout << "[3/3] Cleaning up...\n";
    kill(child_pid, SIGKILL);
    waitpid(child_pid, nullptr, 0);
    g_child_pid = 0;

    std::cout << "\033[32m[PASS] Anchor Engine hysteresis and priority manipulation working correctly!\033[0m\n";
    return 0;
}
