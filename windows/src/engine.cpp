// SPDX-License-Identifier: Apache-2.0
#include "engine.hpp"
namespace vci {
Rpc Engine::call(const Rpc &request, DWORD process) {
    if (request.op == READ)
        return read(request, process);
    std::unique_lock<std::mutex> operation(operations);
    Reader r{request.body};
    Writer w;
    Rpc response{request.op, request.request, 0, request.owner, {}};
    if (request.op == CONTROL) {
        auto what = r.u32(), value = r.u32();
        r.end();
        if (what == 1) {
            if (value != 0 && value != 13500)
                throw Error(j::INVALID_IOCTL_VALUE, "Ignition must be 0 or 13500 mV");
            ignition = value;
            log("Operator ignition override=" + std::to_string(value));
        } else if (what == 2) {
            if (value > 30000)
                throw Error(j::INVALID_IOCTL_VALUE, "Battery value exceeds bound");
            battery = value;
        } else if (what != 0)
            throw Error(j::INVALID_IOCTL_VALUE, "Unknown control action");
        w.u32(usb->healthy());
        w.u32(ignition);
        w.u32(battery);
        w.u32(channel);
        w.u32(pins);
        if (simulation)
            w.u64(pid == process ? owner : 0);
        response.body = w.b;
        return response;
    }
    if (request.op == OPEN) {
        r.end();
        if (owner)
            throw Error(j::DEVICE_IN_USE, "CANable already has an owner");
        usb->open();
        try {
            auto info = usb->command(5);
            Reader i{info.body};
            i.u32();
            auto uid = i.raw(12);
            serial = 2166136261u;
            for (auto b : uid)
                serial = (serial ^ b) * 16777619u;
            if (!serial)
                serial = 1;
            auto f = i.raw(20);
            firmware = std::string(reinterpret_cast<char *>(f.data()),
                                   strnlen(reinterpret_cast<char *>(f.data()), 20));
        } catch (...) {
            usb->close();
            throw;
        }
        {
            std::lock_guard<std::mutex> l(state);
            owner = random64();
            pid = process;
            device = handle_id();
            channel = 0;
            generation = 0;
            lastSeq = barrier = txBarrier = 0;
            readError = 0;
            rx.clear();
            rxBytes = 0;
            cancel_queue(j::DEVICE_NOT_CONNECTED);
        }
        response.owner = owner;
        w.u32(device);
        w.u32(serial);
        response.body = w.b;
        log("OPEN pid=" + std::to_string(process) + " own_serial=" + std::to_string(serial));
        return response;
    }
    check(request.owner, process);
    if (request.op == TRACE) {
        auto id = r.u32(), ioctl = r.u32(), input = r.u32(), output = r.u32();
        r.end();
        log("PassThruIoctl id=" + std::to_string(id) + " ioctl=" + std::to_string(ioctl) +
            " input_present=" + std::to_string(input) +
            " output_present=" + std::to_string(output));
        return response;
    }
    if (request.op == CLOSE) {
        auto id = r.u32();
        r.end();
        if (id != device)
            throw Error(j::INVALID_DEVICE, "Invalid device handle");
        usb->command(4);
        usb->close();
        {
            std::lock_guard<std::mutex> l(state);
            owner = 0;
            channel = 0;
            filter = 0;
            rx.clear();
            rxBytes = 0;
            periodic.clear();
            cancel_queue(j::DEVICE_NOT_CONNECTED);
            cv.notify_all();
        }
        return response;
    }
    if (request.op == CONNECT) {
        auto id = r.u32(), p = r.u32(), flags = r.u32(), rate = r.u32();
        r.end();
        log("PassThruConnect device=" + std::to_string(id) + " protocol=" + std::to_string(p) +
            " flags=" + std::to_string(flags) + " bitrate=" + std::to_string(rate));
        if (id != device)
            throw Error(j::INVALID_DEVICE, "Invalid device handle");
        if (channel)
            throw Error(j::CHANNEL_IN_USE, "Only one channel is available");
        if (p != 7 && p != 0x8005)
            throw Error(j::INVALID_PROTOCOL, "Only ISO15765 / ISO15765_PS is implemented");
        if (flags)
            throw Error(j::INVALID_FLAGS, "Connect flags must be zero");
        if (rate != 500000)
            throw Error(j::INVALID_BAUDRATE, "Only 500 kbit/s is implemented");
        if (generation == 0xffffffffu)
            throw Error(j::EXCEEDED_LIMIT, "Channel generations exhausted");
        auto gen = generation + 1;
        Writer b;
        b.u32(p);
        b.u32(0);
        b.u32(rate);
        usb->command(16, b.b, gen);
        {
            std::lock_guard<std::mutex> l(state);
            generation = gen;
            channel = handle_id();
            protocol = p;
            pins = 0x060e;
            loopback = bs = stmin = filter = routeflags = readError = 0;
            periodic.clear();
        }
        w.u32(channel);
        log("CHANNEL id=" + std::to_string(channel) + " generation=" + std::to_string(generation) +
            " virtual_pins=1550 physical_pins=3/8");
    } else if (request.op == DISCONNECT) {
        auto id = r.u32();
        r.end();
        check_channel(id);
        log("PassThruDisconnect channel=" + std::to_string(id));
        disconnect();
    } else if (request.op == VERSION || request.op == INFO || request.op == VOLTAGE ||
               request.op == PROGRAM) {
        auto id = r.u32();
        if (id != device)
            throw Error(j::INVALID_DEVICE, "Invalid device handle");
        if (request.op == VERSION) {
            r.end();
            for (auto s : {firmware, std::string("CanableVci 0.3.0"), std::string("04.04")}) {
                w.u32(uint32_t(s.size()));
                w.raw(s.data(), s.size());
            }
        } else if (request.op == INFO) {
            r.end();
            w.u32(serial);
        } else if (request.op == VOLTAGE) {
            auto type = r.u32();
            r.end();
            if (type == 3)
                w.u32(battery);
            else if (type == 0x800e)
                w.u32(ignition);
            else
                throw Error(j::NOT_SUPPORTED, "Programming voltage sensing is unavailable");
        } else {
            auto pin = r.u32(), voltage = r.u32();
            r.end();
            if (pin > 16)
                throw Error(j::PIN_INVALID, "Invalid programming pin");
            if (voltage != 0xffffffffu)
                throw Error(j::NOT_SUPPORTED, "Programming voltage hardware is unavailable");
        }
    } else if (request.op == CONFIG) {
        auto id = r.u32(), set = r.u32(), count = r.u32();
        check_channel(id);
        if (count > 64 || set > 1)
            throw Error(j::INVALID_IOCTL_VALUE, "Configuration count/operation is invalid");
        std::vector<j::Config> items;
        uint32_t newPins = pins, newLoop = loopback, newBs = bs, newStmin = stmin;
        for (uint32_t n = 0; n < count; n++) {
            j::Config c{r.u32(), r.u32()};
            log("CONFIG set=" + std::to_string(set) + " parameter=" + std::to_string(c.Parameter) +
                " value=" + std::to_string(c.Value));
            if (set) {
                switch (c.Parameter) {
                case 1:
                    if (c.Value != 500000)
                        throw Error(j::INVALID_BAUDRATE, "Data rate is fixed at 500 kbit/s");
                    break;
                case 3:
                    if (c.Value > 1)
                        throw Error(j::INVALID_IOCTL_VALUE, "LOOPBACK must be 0 or 1");
                    newLoop = c.Value;
                    break;
                case 0x8001:
                    if (c.Value != 0x0308 && c.Value != 0x060e)
                        throw Error(j::INVALID_IOCTL_VALUE, "Virtual pins must be 3/8 or 6/14");
                    newPins = c.Value;
                    break;
                case 0x1e:
                    if (c.Value > 255)
                        throw Error(j::INVALID_IOCTL_VALUE, "BS exceeds one byte");
                    newBs = c.Value;
                    break;
                case 0x1f:
                    if (c.Value > 0x7f && (c.Value < 0xf1 || c.Value > 0xf9))
                        throw Error(j::INVALID_IOCTL_VALUE, "Reserved STmin code");
                    newStmin = c.Value;
                    break;
                case 0x25:
                    if (c.Value != 10)
                        throw Error(j::NOT_SUPPORTED, "WFT maximum is fixed at 10");
                    break;
                default:
                    throw Error(j::NOT_SUPPORTED, "Unsupported timing/configuration parameter " +
                                                      std::to_string(c.Parameter));
                }
            } else {
                switch (c.Parameter) {
                case 1:
                    c.Value = 500000;
                    break;
                case 3:
                    c.Value = loopback;
                    break;
                case 0x8001:
                    c.Value = pins;
                    break;
                case 0x1e:
                    c.Value = bs;
                    break;
                case 0x1f:
                    c.Value = stmin;
                    break;
                case 0x25:
                    c.Value = 10;
                    break;
                default:
                    throw Error(j::NOT_SUPPORTED, "Unsupported GET_CONFIG parameter");
                }
            }
            items.push_back(c);
        }
        r.end();
        if (set && (bs != newBs || stmin != newStmin)) {
            Writer b;
            b.u32(1000);
            b.u32(1000);
            b.u32(1000);
            b.u16(10);
            b.u8(newBs);
            b.u8(newStmin);
            b.u32(0);
            usb->command(18, b.b, generation);
        }
        pins = newPins;
        loopback = newLoop;
        bs = newBs;
        stmin = newStmin;
        log("CONFIG resulting_virtual_pins=" + std::to_string(pins) + " physical_pins=3/8");
        w.u32(count);
        for (auto c : items) {
            w.u32(c.Parameter);
            w.u32(c.Value);
        }
    } else if (request.op == FILTER_ADD) {
        auto id = r.u32(), type = r.u32();
        check_channel(id);
        auto mask = r.msg(), pattern = r.msg(), flow = r.msg();
        r.end();
        if (type != 3)
            throw Error(j::NOT_SUPPORTED, "Only exact FLOW_CONTROL_FILTER is implemented");
        if (filter)
            throw Error(j::EXCEEDED_LIMIT, "One flow-control route is available");
        validate_msg(pattern, false);
        validate_msg(flow, false);
        auto offset = pattern.TxFlags & 0x80 ? 5u : 4u;
        if (mask.ProtocolID != protocol || mask.DataSize != offset || pattern.DataSize != offset ||
            flow.DataSize != offset || (pattern.TxFlags & 0x180) != (flow.TxFlags & 0x180))
            throw Error(j::INVALID_MSG, "Filter addressing or length differs");
        auto full = pattern.TxFlags & 0x100 ? 0x1fffffffu : 0x7ffu;
        auto masked = be_id(mask);
        if (masked != 0xffffffffu && masked != full)
            throw Error(j::NOT_SUPPORTED, "Partial CAN masks are unsupported");
        if (offset == 5 && mask.Data[4] != 0xff)
            throw Error(j::NOT_SUPPORTED, "Partial address masks are unsupported");
        auto handle = handle_id();
        Writer b;
        b.u32(handle);
        b.u32(be_id(pattern));
        b.u32(be_id(flow));
        b.u32((pattern.TxFlags & 0x80 ? 1 : 0) | (pattern.TxFlags & 0x100 ? 2 : 0));
        b.u32(flow.TxFlags);
        b.u8(offset == 5 ? pattern.Data[4] : 0);
        b.u8(offset == 5 ? flow.Data[4] : 0);
        b.u8(bs);
        b.u8(stmin);
        usb->command(32, b.b, generation);
        filter = handle;
        txid = be_id(flow);
        txext = offset == 5 ? flow.Data[4] : 0;
        routeflags = flow.TxFlags & 0x180;
        w.u32(handle);
        log("FLOW_CONTROL_FILTER rx=" + std::to_string(be_id(pattern)) +
            " tx=" + std::to_string(txid) + " flags=" + std::to_string(routeflags) +
            " mask=" + std::to_string(masked) +
            " rx_address=" + std::to_string(offset == 5 ? pattern.Data[4] : 0) +
            " tx_address=" + std::to_string(txext) + " fc_txflags=" + std::to_string(flow.TxFlags));
    } else if (request.op == FILTER_STOP) {
        auto id = r.u32(), handle = r.u32();
        r.end();
        check_channel(id);
        if (!filter || handle != filter)
            throw Error(j::INVALID_FILTER_ID, "Invalid filter handle");
        if (!periodic.empty())
            throw Error(j::CHANNEL_IN_USE, "Stop periodic jobs before removing their route");
        Writer b;
        b.u32(9);
        barrier_result(usb->command(50, b.b, generation), 1);
        filter = 0;
    } else if (request.op == PERIODIC_ADD) {
        auto id = r.u32(), interval = r.u32();
        auto m = r.msg();
        r.end();
        log("PassThruStartPeriodicMsg channel=" + std::to_string(id) +
            " interval_ms=" + std::to_string(interval) +
            " data_size=" + std::to_string(m.DataSize) + " txflags=" + std::to_string(m.TxFlags));
        check_channel(id);
        validate_msg(m);
        if (interval < 5 || interval > 65535)
            throw Error(j::INVALID_TIME_INTERVAL, "Periodic interval must be 5..65535 ms");
        auto offset = m.TxFlags & 0x80 ? 5u : 4u;
        if (m.DataSize - offset > 64)
            throw Error(j::EXCEEDED_LIMIT, "Periodic payload exceeds 64 bytes");
        if (periodic.size() >= 2)
            throw Error(j::EXCEEDED_LIMIT, "Only two periodic slots are available");
        auto handle = handle_id();
        Writer b;
        b.u32(handle);
        b.u32(filter);
        b.u32(interval * 1000);
        b.u32(m.TxFlags);
        b.u16(uint16_t(m.DataSize - offset));
        b.u16(0);
        b.raw(m.Data + offset, m.DataSize - offset);
        auto result = usb->command(64, b.b, generation);
        periodic[handle] = result.request;
        w.u32(handle);
    } else if (request.op == PERIODIC_STOP) {
        auto id = r.u32(), handle = r.u32();
        r.end();
        check_channel(id);
        if (!periodic.count(handle))
            throw Error(j::INVALID_MSG_ID, "Invalid periodic handle");
        Writer b;
        b.u32(handle);
        usb->command(65, b.b, generation);
        periodic.erase(handle);
    } else if (request.op == CLEAR) {
        auto id = r.u32(), mask = r.u32();
        r.end();
        check_channel(id);
        if (!mask || (mask & ~15u))
            throw Error(j::INVALID_IOCTL_VALUE, "Invalid clear mask");
        if (mask & 8)
            mask |= 1;
        Writer b;
        b.u32(mask);
        barrier_result(usb->command(50, b.b, generation), mask);
        if (mask & 4)
            periodic.clear();
        if (mask & 8)
            filter = 0;
    } else if (request.op == WRITE) {
        auto id = r.u32(), count = r.u32(), timeout = r.u32();
        log("PassThruWriteMsgs channel=" + std::to_string(id) +
            " requested=" + std::to_string(count) + " timeout_ms=" + std::to_string(timeout));
        check_channel(id);
        if (!count || count > 64 || timeout > 60000)
            throw Error(j::INVALID_IOCTL_VALUE, "Invalid write bound");
        std::vector<j::Msg> messages;
        for (uint32_t n = 0; n < count; n++) {
            auto m = r.msg();
            validate_msg(m);
            log("TX message id=" + std::to_string(be_id(m)) + " txflags=" +
                std::to_string(m.TxFlags) + " data_size=" + std::to_string(m.DataSize));
            messages.push_back(m);
        }
        r.end();
        std::vector<std::shared_ptr<Job>> jobs;
        auto end = timeout ? now_ms() + timeout : 0;
        uint32_t result = 0;
        {
            std::lock_guard<std::mutex> l(state);
            auto group = ++batch;
            for (auto m : messages) {
                if (tx.size() >= 64 || txBytes + m.DataSize > 262144) {
                    result = j::BUFFER_FULL;
                    break;
                }
                auto job = std::make_shared<Job>();
                job->msg = m;
                job->batch = group;
                job->deadline = end;
                tx.push_back(job);
                txBytes += m.DataSize;
                jobs.push_back(job);
            }
        }
        operation.unlock();
        uint32_t written = uint32_t(jobs.size());
        if (timeout) {
            std::unique_lock<std::mutex> l(state);
            written = 0;
            for (auto job : jobs) {
                while (!job->done && now_ms() < end && owner == request.owner && channel == id &&
                       usb->healthy())
                    cv.wait_for(l, std::chrono::milliseconds(10));
                if (!job->done) {
                    result = usb->healthy() ? j::TIMEOUT : j::DEVICE_NOT_CONNECTED;
                    break;
                }
                if (job->code) {
                    result = job->code;
                    break;
                }
                written++;
            }
            if (result) {
                for (auto &job : jobs)
                    if (!job->done)
                        job->cancel = true;
                l.unlock();
                operation.lock();
                std::shared_ptr<Job> current;
                {
                    std::lock_guard<std::mutex> s(state);
                    if (active && active->cancel)
                        current = active;
                }
                if (current && usb->healthy()) {
                    Writer b;
                    b.u32(current->request);
                    try {
                        auto result = usb->command(49, b.b, generation);
                        Reader cancelled{result.body};
                        cancelled.u32();
                        auto last = cancelled.u32();
                        cancelled.end();
                        {
                            std::lock_guard<std::mutex> s(state);
                            txBarrier = last;
                            finish(current, j::TIMEOUT);
                            active.reset();
                        }
                        events();
                    } catch (const Error &e) {
                        log(std::string("TX cancellation: ") + e.what());
                    }
                }
            }
        }
        w.u32(written);
        response.result = result;
        log("PassThruWriteMsgs returned=" + std::to_string(written) +
            " result=" + std::to_string(result));
    } else if (request.op == TEST_INJECT) {
        if (!simulation)
            throw Error(j::NOT_SUPPORTED, "Test injection is absent in the production backend");
        Packet p;
        p.kind = 3;
        p.op = r.u16();
        r.u16();
        p.session = 1;
        p.generation = generation;
        p.request = r.u32();
        p.time = r.u64();
        p.body = r.raw(r.u32());
        r.end();
        usb->inject(p);
    } else if (request.op == TEST_MODE) {
        if (!simulation)
            throw Error(j::NOT_SUPPORTED, "Simulation is disabled");
        auto what = r.u32(), value = r.u32();
        r.end();
        // Simulation-only barrier: process queued packets before queue-limit checks.
        if (what == 3 && value == 0)
            events();
        else
            usb->mode(what, value);
    } else
        throw Error(j::NOT_SUPPORTED, "Unknown service operation");
    response.body = w.b;
    return response;
}
} // namespace vci
