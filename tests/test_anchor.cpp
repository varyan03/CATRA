#include "../daemon/anchor_engine.h"

#include <iostream>
#include <string>
#include <cstring>
#include <csignal>
#include <unistd.h>

extern volatile sig_atomic_t shutdown_requested;

namespace {
void signal_handler(int sig) {
    (void)sig;
    shutdown_requested = 1;
}

void install_signal_handlers() {
    struct sigaction sa{};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}
} // namespace

int main(int argc, char* argv[]) {
    install_signal_handlers();

    bool ml_focus_signal = false;
    if (argc > 1 && std::string(argv[1]) == "focus") {
        ml_focus_signal = true;
    }

    std::cout << "========================================================\n";
    std::cout << "  [CATRA] Autonomous Hybrid Anchor Engine Test Runner\n";
    std::cout << "  Mode: " << (ml_focus_signal ? "FORCED ML FOCUS SIGNAL" : "DYNAMIC ATIME HEURISTIC") << "\n";
    std::cout << "  Press Ctrl+C to initiate clean priority restoration...\n";
    std::cout << "========================================================\n\n";

    AnchorEngine engine;

    while (!shutdown_requested) {
        engine.sweep(ml_focus_signal);
        sleep(1);
    }

    std::cout << "\n[!] Shutdown requested. Restoring thread priorities...\n";
    engine.restore_all();
    std::cout << "[+] Priorities restored cleanly. Exiting.\n";

    return 0;
}
