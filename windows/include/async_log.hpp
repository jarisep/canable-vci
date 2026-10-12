// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <fstream>
#include <stdexcept>
#include <cstdio>

namespace vci {
// Disk I/O belongs exclusively to the worker. Producers never wait for disk.
class AsyncLog {
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<std::string> queue;
    const size_t capacity;
    bool stopping = false;
    uint64_t dropped = 0, failed = 0;
    std::function<void(const std::string &)> sink;
    std::function<void()> failure;
    std::thread worker;
    void output(const std::string &line) noexcept {
        try { sink(line); }
        catch (...) {
            { std::lock_guard<std::mutex> lock(mutex); ++failed; }
            try { if (failure) failure(); } catch (...) {}
        }
    }
    void run() {
        for (;;) {
            std::string line;
            uint64_t lost = 0;
            {
                std::unique_lock<std::mutex> lock(mutex);
                ready.wait(lock, [&] { return stopping || !queue.empty() || dropped; });
                lost = dropped; dropped = 0;
                if (!queue.empty()) { line = std::move(queue.front()); queue.pop_front(); }
                else if (stopping && !lost) break;
            }
            if (lost) output("LOGGER dropped=" + std::to_string(lost) + " (queue full)\n");
            if (!line.empty()) output(line);
        }
    }
public:
    explicit AsyncLog(std::function<void(const std::string &)> writer,
                      size_t limit = 256, std::function<void()> onFailure = {})
        : capacity(limit), sink(std::move(writer)), failure(std::move(onFailure)) {
        if (!capacity) throw std::invalid_argument("Log capacity must be positive");
        worker = std::thread([this] { run(); });
    }
    ~AsyncLog() { close(); }
    void write(uint64_t timestamp, const std::string &text) {
        // Bound retained memory to about 1 MiB at the default capacity.
        auto line = std::to_string(timestamp) + " " + text.substr(0, 4096);
        if (text.size() > 4096) line += " [truncated]";
        line += "\n";
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopping) return;
            if (queue.size() == capacity) ++dropped;
            else queue.push_back(std::move(line));
        }
        ready.notify_one();
    }
    void close() {
        { std::lock_guard<std::mutex> lock(mutex); stopping = true; }
        ready.notify_one();
        if (worker.joinable()) worker.join();
    }
    uint64_t failedWrites() {
        std::lock_guard<std::mutex> lock(mutex); return failed;
    }
};

class RotatingLogFile {
    std::string path;
    std::ofstream file;
    size_t bytes = 0;
    const size_t limit;
public:
    explicit RotatingLogFile(std::string name, size_t maxBytes = 1024 * 1024)
        : path(std::move(name)), limit(maxBytes) {}
    void operator()(const std::string &line) {
        if (!file.is_open()) {
            std::ifstream current(path, std::ios::binary | std::ios::ate);
            bytes = current ? static_cast<size_t>(current.tellg()) : 0;
            file.open(path, std::ios::binary | std::ios::app);
        }
        if (bytes && bytes + line.size() > limit) {
            file.close();
            auto old = path + ".old";
            std::remove(old.c_str());
            if (std::rename(path.c_str(), old.c_str()) != 0)
                throw std::runtime_error("Log rotation failed");
            bytes = 0;
            file.open(path, std::ios::binary | std::ios::app);
        }
        file << line;
        file.flush(); // Worker only; retain useful evidence after an unexpected exit.
        if (!file) {
            file.close(); file.clear();
            throw std::runtime_error("Log write failed");
        }
        bytes += line.size();
    }
};
}
