// SPDX-License-Identifier: Apache-2.0
#include "async_log.hpp"
#include <filesystem>
#include <future>
#include <iostream>
#include <vector>
#include <atomic>
#include <sstream>
using namespace vci;
static void require(bool condition) {
    if (!condition) throw std::runtime_error("Logger regression check failed");
}
int main() {
    // A blocked disk must not block producers; overload must be explicit.
    std::promise<void> entered, release;
    auto gate = release.get_future().share();
    std::vector<std::string> lines;
    AsyncLog slow([&](const std::string &line) {
        if (lines.empty()) { entered.set_value(); gate.wait(); }
        lines.push_back(line);
    }, 2);
    slow.write(1, "first"); entered.get_future().wait();
    auto producer = std::async(std::launch::async, [&] {
        for (unsigned i=0; i<100; ++i) slow.write(i+2, "queued");
    });
    auto status = producer.wait_for(std::chrono::seconds(2));
    release.set_value(); producer.get(); slow.close();
    require(status == std::future_status::ready);
    require(lines.size() == 4);
    require(lines[0] == "1 first\n");
    require(lines[1] == "LOGGER dropped=98 (queue full)\n");
    require(lines[2] == "2 queued\n" && lines[3] == "3 queued\n");
    std::cout << "PASS blocked writer, bounded queue, drop reporting, shutdown drain\n";

    std::atomic<unsigned> records{0};
    AsyncLog concurrent([&](const std::string &) { ++records; }, 4096);
    std::vector<std::thread> producers;
    for (unsigned t=0; t<8; ++t) producers.emplace_back([&] {
        for (unsigned i=0; i<200; ++i) concurrent.write(i, "concurrent");
    });
    for (auto &thread : producers) thread.join();
    concurrent.close(); require(records == 1600);
    std::cout << "PASS concurrent producers and complete drain\n";

    unsigned failures=0, calls=0;
    AsyncLog failing([&](const std::string &) {
        if (!calls++) throw std::runtime_error("injected disk failure");
    }, 16, [&] { ++failures; });
    failing.write(1,"failure"); failing.write(2,"recovery"); failing.close();
    require(failures == 1 && failing.failedWrites() == 1 && calls == 2);
    std::cout << "PASS writer failure containment and recovery\n";

    auto path = std::filesystem::temp_directory_path() /
                ("canable-log-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    {
        RotatingLogFile disk(path.string(), 12);
        disk("first\n"); disk("second\n"); disk("third\n");
    }
    auto read = [](const std::string &name) {
        std::ifstream file(name, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), {});
    };
    require(read(path.string()+".old") == "second\n");
    require(read(path.string()) == "third\n");
    {
        RotatingLogFile disk(path.string(),12); disk("fourth\n");
    }
    require(read(path.string()+".old") == "third\n");
    require(read(path.string()) == "fourth\n");
    std::filesystem::remove(path); std::filesystem::remove(path.string()+".old");
    std::cout << "PASS rotation and reopen of existing logs\n";
    std::string retained;
    AsyncLog bounded([&](const std::string &line) { retained=line; });
    bounded.write(42,std::string(100000,'x')); bounded.close();
    require(retained.size() < 4200 && retained.find("[truncated]") != std::string::npos);
    std::cout << "PASS bounded record length\n";
}
