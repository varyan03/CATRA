#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

/**
 * KeyboardSensor
 *
 * Headless keystroke telemetry sensor designed to read Linux evdev input streams
 * (/dev/input/event* or named FIFOs) asynchronously using non-blocking poll/read loops.
 *
 * Thread Safety:
 * start() and stop() are intended to be managed by a single controller thread,
 * and are additionally serialized via an internal lifecycle mutex.
 * get_and_reset_count() and running() are thread-safe and non-blocking.
 */
class KeyboardSensor {
public:
    KeyboardSensor() = default;
    ~KeyboardSensor() {
        stop();
    }

    KeyboardSensor(const KeyboardSensor&) = delete;
    KeyboardSensor& operator=(const KeyboardSensor&) = delete;
    KeyboardSensor(KeyboardSensor&&) = delete;
    KeyboardSensor& operator=(KeyboardSensor&&) = delete;

    bool start(const std::string& device_path);
    void stop();
    int get_and_reset_count() noexcept;
    bool running() const noexcept;

private:
    std::mutex lifecycle_mutex_;
    std::atomic<bool> is_running_{false};
    std::atomic<int> keystroke_count_{0};
    int fd_{-1};
    std::thread listener_thread_;

    void worker_loop(int worker_fd);
};
