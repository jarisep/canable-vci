// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstddef>
#include <cstdint>
#include <windows.h>
namespace j {
using U = uint32_t;
enum : U {
    OK = 0,
    NOT_SUPPORTED = 1,
    INVALID_CHANNEL = 2,
    INVALID_PROTOCOL = 3,
    NULL_PARAMETER = 4,
    INVALID_IOCTL = 5,
    INVALID_FLAGS = 6,
    FAILED = 7,
    DEVICE_NOT_CONNECTED = 8,
    TIMEOUT = 9,
    INVALID_MSG = 10,
    INVALID_TIME_INTERVAL = 11,
    EXCEEDED_LIMIT = 12,
    INVALID_MSG_ID = 13,
    DEVICE_IN_USE = 14,
    INVALID_IOCTL_VALUE = 15,
    BUFFER_EMPTY = 16,
    BUFFER_FULL = 17,
    BUFFER_OVERFLOW = 18,
    PIN_INVALID = 19,
    CHANNEL_IN_USE = 20,
    MSG_PROTOCOL_ID = 21,
    INVALID_FILTER_ID = 22,
    NO_FLOW_CONTROL = 23,
    NOT_UNIQUE = 24,
    INVALID_BAUDRATE = 25,
    INVALID_DEVICE = 26
};
struct Msg {
    U ProtocolID, RxStatus, TxFlags, Timestamp, DataSize, ExtraDataIndex;
    uint8_t Data[4128];
};
struct Config {
    U Parameter, Value;
};
struct ConfigList {
    U NumOfParams;
    Config *ConfigPtr;
};
struct Param {
    U Parameter, Value, Supported;
};
struct ParamList {
    U NumOfParams;
    Param *ParamPtr;
};
static_assert(sizeof(Msg) == 4152 && offsetof(Msg, Data) == 24);
static_assert(sizeof(Config) == 8 && sizeof(Param) == 12);
#ifdef _WIN32
static_assert(sizeof(U) == sizeof(ULONG));
#if !defined(_WIN64)
static_assert(sizeof(ConfigList) == 8 && offsetof(ConfigList, ConfigPtr) == 4);
static_assert(sizeof(ParamList) == 8 && offsetof(ParamList, ParamPtr) == 4);
#endif
#endif
} // namespace j
