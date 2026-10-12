// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "rpc.hpp"
#include "usb_client.hpp"
#include <functional>
#include <map>
namespace vci {
class Engine {
    struct Job {
        j::Msg msg{};
        uint32_t request = 0, code = 0;
        uint64_t batch = 0, deadline = 0;
        bool done = false, cancel = false;
    };
    std::unique_ptr<Client> usb;
    bool simulation;
    std::function<void(const std::string &)> log;
    std::mutex operations, state;
    std::condition_variable cv;
    std::atomic<bool> running{true};
    std::thread worker;
    uint64_t owner = 0, batch = 0;
    DWORD pid = 0;
    uint32_t device = 0, channel = 0, generation = 0, nextHandle = 0, protocol = 0, serial = 0;
    uint32_t pins = 0x060e, loopback = 0, bs = 0, stmin = 0, filter = 0, txid = 0, routeflags = 0;
    uint8_t txext = 0;
    uint32_t barrier = 0, txBarrier = 0, lastSeq = 0, readError = 0;
    size_t rxBytes = 0, txBytes = 0;
    std::atomic<uint32_t> ignition{13500}, battery{13500};
    std::string firmware;
    std::deque<j::Msg> rx;
    std::deque<std::shared_ptr<Job>> tx;
    std::shared_ptr<Job> active;
    std::map<uint32_t, uint32_t> periodic;
    void check(uint64_t token, DWORD process) {
        if (!owner || token != owner || process != pid)
            throw Error(j::INVALID_DEVICE, "Device ownership is invalid; reopen");
        if (!usb->healthy())
            throw Error(j::DEVICE_NOT_CONNECTED, "CANable USB session is lost");
    }
    void check_channel(uint32_t id) {
        if (!channel || id != channel)
            throw Error(j::INVALID_CHANNEL, "Invalid channel handle");
    }
    uint32_t handle_id() {
        if (++nextHandle == 0)
            throw Error(j::EXCEEDED_LIMIT, "Handle IDs exhausted");
        return nextHandle;
    }
    void finish(const std::shared_ptr<Job> &job, uint32_t code) {
        job->done = true;
        job->code = code;
        cv.notify_all();
    }
    void cancel_queue(uint32_t code) {
        for (auto &job : tx)
            if (!job->done)
                finish(job, code);
        tx.clear();
        txBytes = 0;
        active.reset();
    }
    void add_rx(const j::Msg &m) {
        if (rx.size() >= 64 || rxBytes + m.DataSize > 262144) {
            readError = j::BUFFER_OVERFLOW;
            cv.notify_all();
            log("RX queue overflow; new message rejected explicitly");
            return;
        }
        rx.push_back(m);
        rxBytes += m.DataSize;
        cv.notify_all();
    }
    void events() {
        Packet p;
        uint32_t ack = 0;
        while (usb->pop(p)) {
            Reader r{p.body};
            auto seq = r.u32();
            if (!seq || seq <= lastSeq)
                throw Error(j::FAILED, "Non-increasing event sequence");
            lastSeq = seq;
            ack = seq;
            std::lock_guard<std::mutex> l(state);
            if (p.generation != generation || !channel)
                continue;
            if (p.op >= 256 && p.op <= 258) {
                auto m = event_message(p, protocol);
                if (seq > barrier && (p.op != 256 || seq > txBarrier))
                    add_rx(m);
                if (p.op == 256 && active && p.request == active->request) {
                    auto job = active;
                    finish(job, j::OK);
                    if (loopback && seq > barrier && seq > txBarrier) {
                        auto echo = job->msg;
                        echo.RxStatus = 1 | (echo.TxFlags & 0x180);
                        echo.TxFlags = 0;
                        echo.Timestamp = uint32_t(p.time);
                        echo.ExtraDataIndex = echo.DataSize;
                        add_rx(echo);
                    }
                    active.reset();
                }
            } else if (p.op == 259) {
                r.u32();
                r.u32();
                r.u32();
                r.u32();
                r.u8();
                r.u8();
                r.u16();
                auto reason = r.u32();
                auto raw = int32_t(r.u32());
                r.end();
                log("TRANSFER_ERROR request=" + std::to_string(p.request) +
                    " reason=" + std::to_string(reason) + " raw=" + std::to_string(raw));
                if (active && p.request == active->request) {
                    auto job = active;
                    finish(job,
                           (reason == 1 || reason == 2 || reason == 3) ? j::TIMEOUT : j::FAILED);
                    if (!job->deadline)
                        readError = job->code;
                    for (auto &pending : tx)
                        if (pending->batch == job->batch && !pending->done)
                            finish(pending, job->code);
                    active.reset();
                } else if (reason != 7 && seq > barrier && (!p.request || seq > txBarrier)) {
                    readError = j::FAILED;
                    cv.notify_all();
                }
            } else if (p.op == 261) {
                readError = j::BUFFER_OVERFLOW;
                cv.notify_all();
            }
        }
        if (ack) {
            Writer w;
            w.u32(ack);
            usb->command(80, w.b);
        }
    }
    void tick() {
        std::lock_guard<std::mutex> operation(operations);
        if (!owner)
            return;
        if (!usb->healthy()) {
            std::lock_guard<std::mutex> l(state);
            cancel_queue(j::DEVICE_NOT_CONNECTED);
            cv.notify_all();
            return;
        }
        try {
            events();
            std::shared_ptr<Job> job;
            {
                std::lock_guard<std::mutex> l(state);
                while (!tx.empty() && tx.front()->done) {
                    txBytes -= tx.front()->msg.DataSize;
                    tx.pop_front();
                }
                if (!active && !tx.empty()) {
                    job = tx.front();
                    if (job->cancel || (job->deadline && now_ms() >= job->deadline)) {
                        finish(job, j::TIMEOUT);
                        return;
                    }
                }
            }
            if (job) {
                Writer w;
                w.u32(filter);
                w.u32(job->msg.TxFlags);
                auto offset = (job->msg.TxFlags & 0x80) ? 5u : 4u;
                w.u16(uint16_t(job->msg.DataSize - offset));
                w.u16(0);
                const auto stamp = now_ms();
                if (job->deadline && stamp >= job->deadline) {
                    std::lock_guard<std::mutex> l(state);
                    finish(job, j::TIMEOUT);
                    return;
                }
                w.u32(job->deadline ? uint32_t(job->deadline - stamp) : 0);
                w.raw(job->msg.Data + offset, job->msg.DataSize - offset);
                try {
                    auto result = usb->command(48, w.b, generation);
                    std::lock_guard<std::mutex> l(state);
                    job->request = result.request;
                    active = job;
                } catch (const Error &e) {
                    if (e.code != j::BUFFER_FULL && e.code != j::EXCEEDED_LIMIT) {
                        std::lock_guard<std::mutex> l(state);
                        finish(job, e.code);
                        if (!job->deadline)
                            readError = e.code;
                        for (auto &other : tx)
                            if (other->batch == job->batch && !other->done)
                                finish(other, e.code);
                    }
                }
            }
        } catch (const std::exception &e) {
            log(std::string("USB worker failure: ") + e.what());
            // A malformed owned stream invalidates the session; do not resume TX.
            usb->close();
            std::lock_guard<std::mutex> l(state);
            readError = j::DEVICE_NOT_CONNECTED;
            cancel_queue(readError);
            cv.notify_all();
        }
    }
    void run() {
        uint64_t heartbeat = now_ms();
        while (running) {
            tick();
            if (now_ms() - heartbeat >= 1000) {
                std::lock_guard<std::mutex> l(operations);
                if (owner && usb->healthy())
                    try {
                        usb->command(3);
                        heartbeat = now_ms();
                    } catch (const std::exception &e) {
                        log(e.what());
                    }
            }
            Sleep(10);
        }
    }
    void barrier_result(const UsbReply &result, uint32_t mask) {
        Reader r{result.body};
        r.u32();
        auto last = r.u32();
        r.end();
        {
            std::lock_guard<std::mutex> l(state);
            if (mask & 2) {
                barrier = last;
                rx.clear();
                rxBytes = 0;
                readError = 0;
            }
            if (mask & 1) {
                txBarrier = last;
                cancel_queue(j::FAILED);
            }
        }
        events();
    }
    void disconnect() {
        if (!channel)
            return;
        auto result = usb->command(17, {}, generation);
        barrier_result(result, 3);
        std::lock_guard<std::mutex> l(state);
        channel = 0;
        filter = 0;
        periodic.clear();
        cv.notify_all();
    }
    void validate_msg(const j::Msg &m, bool data = true) {
        if (m.ProtocolID != protocol)
            throw Error(j::MSG_PROTOCOL_ID, "Message protocol differs from channel");
        if (m.TxFlags & ~0x1c0u)
            throw Error(j::INVALID_FLAGS, "Unsupported TX flags");
        auto offset = m.TxFlags & 0x80 ? 5u : 4u;
        if (m.DataSize < offset + (data ? 1u : 0u) || m.DataSize > offset + 4095)
            throw Error(j::INVALID_MSG, "Invalid ISO-TP message length");
        auto id = be_id(m);
        if (id > ((m.TxFlags & 0x100) ? 0x1fffffffu : 0x7ffu))
            throw Error(j::INVALID_MSG, "Invalid CAN identifier");
        if (data && (!filter || id != txid || (m.TxFlags & 0x180) != routeflags ||
                     ((routeflags & 0x80) && m.Data[4] != txext)))
            throw Error(j::NO_FLOW_CONTROL, "No matching exact flow-control route");
    }
    Rpc read(const Rpc &request, DWORD process) {
        Reader b{request.body};
        auto id = b.u32(), count = b.u32(), timeout = b.u32();
        b.end();
        if (!count || count > 64 || timeout > 60000)
            throw Error(j::INVALID_IOCTL_VALUE, "Invalid read bound");
        log("PassThruReadMsgs channel=" + std::to_string(id) +
            " requested=" + std::to_string(count) + " timeout_ms=" + std::to_string(timeout));
        std::unique_lock<std::mutex> l(state);
        check(request.owner, process);
        check_channel(id);
        auto end = now_ms() + timeout;
        Writer w;
        std::vector<j::Msg> found;
        uint32_t result = 0;
        for (;;) {
            while (!rx.empty() && found.size() < count) {
                found.push_back(rx.front());
                rxBytes -= rx.front().DataSize;
                rx.pop_front();
            }
            if (found.size() == count)
                break;
            if (!owner || owner != request.owner || !usb->healthy()) {
                result = j::DEVICE_NOT_CONNECTED;
                break;
            }
            if (channel != id) {
                result = j::INVALID_CHANNEL;
                break;
            }
            if (readError) {
                result = readError;
                readError = 0;
                break;
            }
            if (!timeout || now_ms() >= end) {
                result = timeout ? j::TIMEOUT : j::BUFFER_EMPTY;
                break;
            }
            cv.wait_for(l, std::chrono::milliseconds(std::min<uint64_t>(100, end - now_ms())));
        }
        if (!timeout && !found.empty())
            result = 0;
        w.u32(uint32_t(found.size()));
        for (auto &m : found)
            w.msg(m);
        log("PassThruReadMsgs returned=" + std::to_string(found.size()) +
            " result=" + std::to_string(result));
        return {request.op, request.request, result, request.owner, w.b};
    }

  public:
    Engine(std::unique_ptr<Client> c, bool sim, std::function<void(const std::string &)> logger)
        : usb(std::move(c)), simulation(sim), log(std::move(logger)) {
        nextHandle = uint32_t(random64() & 0x7fffffff);
        worker = std::thread([this] { run(); });
    }
    ~Engine() {
        shutdown();
    }
    void shutdown() {
        if (!running.exchange(false))
            return;
        if (worker.joinable())
            worker.join();
        std::lock_guard<std::mutex> op(operations);
        if (owner && usb->healthy())
            try {
                usb->command(4);
            } catch (...) {
            }
        usb->close();
        std::lock_guard<std::mutex> l(state);
        owner = 0;
        channel = 0;
        cancel_queue(j::DEVICE_NOT_CONNECTED);
        cv.notify_all();
    }
    void release(uint64_t token) {
        std::lock_guard<std::mutex> op(operations);
        if (owner != token)
            return;
        log("Owner released; stop all device work");
        if (usb->healthy())
            try {
                usb->command(4);
            } catch (const std::exception &e) {
                log(e.what());
            }
        usb->close();
        std::lock_guard<std::mutex> l(state);
        owner = 0;
        channel = 0;
        filter = 0;
        periodic.clear();
        rx.clear();
        rxBytes = 0;
        cancel_queue(j::DEVICE_NOT_CONNECTED);
        cv.notify_all();
    }
    Rpc call(const Rpc &request, DWORD process);
};
} // namespace vci
