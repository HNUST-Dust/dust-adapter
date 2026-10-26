#include "usb_session_policy.hpp"
/* SPDX-License-Identifier: Apache-2.0 */
#include <cstdlib>
#include <iostream>

#include "usb_protocol.hpp"
#define CHECK(c)                                      \
    do {                                              \
        if (!(c)) {                                   \
            std::cerr << "line " << __LINE__ << '\n'; \
            std::abort();                             \
        }                                             \
    } while (0)
int main() {
    namespace w = dust::wire;
    w::SessionPolicy policy;
    CHECK(!policy.Arm(1, 0));
    policy.Open(42);
    CHECK(!policy.Arm(1, 0));
    CHECK(!policy.Configure(0));
    CHECK(!policy.Configure(1000001));
    CHECK(policy.Configure(20000));
    policy.Observe(1);
    CHECK(!policy.Arm(1, 0));
    CHECK(policy.Arm(2, 0xfffffff0U));
    CHECK(!policy.Configure(10000));
    CHECK(!policy.Accept(2, 100));  // replay cannot keep the lease alive
    CHECK(!policy.Accept(1, 100));
    CHECK(!policy.Expired(uint32_t(0xfffffff0U + 19999)));
    CHECK(policy.Expired(uint32_t(0xfffffff0U + 20000)));
    CHECK(!policy.Accept(3, uint32_t(0xfffffff0U + 20000)));
    policy.Close();
    policy.Open(43);
    CHECK(policy.Configure(20000));
    CHECK(policy.Arm(3, 0));
    CHECK(policy.Expired(20000));
    CHECK(
        !policy.Accept(4, 20001));  // expiry is terminal until a fresh session
    policy.Close();
    CHECK(!policy.Armed() && !policy.Configured() && !policy.Session());
    CHECK(!policy.Accept(4, 0));
    CHECK(w::NewSequence(0, 0xffffffffU));
    w::CanFrame frame{};
    frame.channel = 0;
    frame.id = 0x7ff;
    frame.length = 8;
    frame.data[7] = 0x81;
    dust::wire::UsbOutRequest request{};
    w::CanTransmitRequest can_request{};
    can_request.header = {1, 42};
    can_request.frame_count = 1;
    can_request.frames[0] = frame;
    CHECK(w::Encode(can_request, request));
    CHECK(w::HasHeader(request.data(), 128));
    CHECK(w::ValidRecords(request));
    CHECK(w::Decode(request.data() + 32).data[7] == 0x81);
    request[32 + 3] = 1;
    CHECK(!w::ValidRecords(request));
    request[32 + 3] = 0;
    request[10] = 7;
    CHECK(!w::ValidRecords(request));
    frame.id = 0x1fffffff;
    frame.flags = w::kExtended | w::kRemote;
    can_request.header.request_sequence = 2;
    can_request.frames[0] = frame;
    CHECK(w::Encode(can_request, request));
    CHECK(w::ValidRecords(request));
    request[32 + 2] = 0;
    CHECK(!w::ValidRecords(request));
    request = {};
    request[8] = 2;  // 拒绝旧版 v2 包。
    CHECK(!w::HasHeader(request.data(), 128));
    frame.flags = w::kUartAction;
    frame.channel = 2;
    frame.id = 0;
    w::ConfigureSafetyRequest safety{};
    safety.header = {3, 42};
    safety.lease_us = 20000;
    safety.uart_stop_count = 1;
    safety.uart_stop_actions[0].channel = frame.channel;
    safety.uart_stop_actions[0].length = frame.length;
    CHECK(w::Encode(safety, request));
    CHECK(w::ValidRecords(request, true));
    CHECK(!w::ValidRecords(request));
    request[20] = 1;
    CHECK(!w::ValidRecords(request, true));
    uint8_t bytes[96]{};
    w::UartTransmitRequest uart_request{};
    uart_request.header = {4, 42};
    uart_request.channel = 3;
    uart_request.byte_count = sizeof(bytes);
    CHECK(w::Encode(uart_request, request));
    CHECK(request[11] == 96);
    dust::wire::UsbOutRequest invalid_request{};
    uart_request.byte_count = 97;
    CHECK(!w::Encode(uart_request, invalid_request));
    std::cout
        << "Generic protocol bounds, flags, session gating and lease wrap "
           "contracts passed\n";
}
