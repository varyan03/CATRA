#include "telemetry_sensor.h"

#if defined(__linux__)
#include <linux/input.h>
#else
#include <sys/time.h>
#include <cstdint>
struct input_event {
    struct timeval time;
    uint16_t type;
    uint16_t code;
    int32_t value;
};
#ifndef EV_KEY
#define EV_KEY 0x01
#endif
#endif

#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <cerrno>
#include <iostream>

bool KeyboardSensor::start(const std::string& device_path) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);

    if (is_running_.load(std::memory_order_acquire)) {
        return false;
    }

    // Reap prior state if worker exited independently on EOF or error
    if (listener_thread_.joinable()) {
        listener_thread_.join();
    }
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }

    const int new_fd = open(device_path.c_str(), O_RDONLY | O_NONBLOCK);
    if (new_fd < 0) {
        return false;
    }

    fd_ = new_fd;
    keystroke_count_.store(0, std::memory_order_relaxed);
    is_running_.store(true, std::memory_order_release);

    // Pass new_fd by value to eliminate data race with this->fd_
    listener_thread_ = std::thread(&KeyboardSensor::worker_loop, this, new_fd);
    return true;
}

void KeyboardSensor::worker_loop(int worker_fd) {
    struct pollfd pfd{};
    pfd.fd = worker_fd;
    pfd.events = POLLIN;

    while (is_running_.load(std::memory_order_relaxed)) {
        pfd.revents = 0;
        const int ret = poll(&pfd, 1, 100);

        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            is_running_.store(false, std::memory_order_release);
            break;
        }

        if (ret == 0) {
            continue;
        }

        // Drain pending data before handling stream closure flags
        if (pfd.revents & POLLIN) {
            while (true) {
                struct input_event ev;
                const ssize_t bytes = read(worker_fd, &ev, sizeof(ev));

                if (bytes == static_cast<ssize_t>(sizeof(ev))) {
                    // EV_KEY value 1 represents key press (0 = release, 2 = autorepeat)
                    if (ev.type == EV_KEY && ev.value == 1) {
                        keystroke_count_.fetch_add(1, std::memory_order_relaxed);
                    }
                } else if (bytes == 0) {
                    is_running_.store(false, std::memory_order_release);
                    break;
                } else if (bytes < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    }
                    if (errno == EINTR) {
                        continue;
                    }
                    is_running_.store(false, std::memory_order_release);
                    break;
                } else {
                    // Short read indicates byte stream desynchronization
                    is_running_.store(false, std::memory_order_release);
                    break;
                }
            }
        }

        // Handle termination and socket/stream hangup conditions
        if (!is_running_.load(std::memory_order_relaxed) || (pfd.revents & (POLLHUP | POLLERR | POLLNVAL))) {
            is_running_.store(false, std::memory_order_release);
            break;
        }
    }
}

void KeyboardSensor::stop() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    is_running_.store(false, std::memory_order_release);

    if (listener_thread_.joinable()) {
        listener_thread_.join();
    }

    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
}

int KeyboardSensor::get_and_reset_count() noexcept {
    return keystroke_count_.exchange(0, std::memory_order_relaxed);
}

bool KeyboardSensor::running() const noexcept {
    return is_running_.load(std::memory_order_acquire);
}
