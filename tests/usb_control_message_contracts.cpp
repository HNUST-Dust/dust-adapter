/* SPDX-License-Identifier: Apache-2.0 */
#include <cassert>

#include "usb_protocol.hpp"

int main() {
    namespace w = dust::wire;
    w::UsbOutRequest request{};
    w::HelloRequest hello{{7, 9}};
    assert(w::Encode(hello, request));
    assert(request[9] == 1 && w::U32(request.data() + 4) == 7);
    w::HelloRequest parsed{};
    assert(w::Decode(request, parsed) && parsed.header.session_id == 9);
    request[127] = 1;
    assert(!w::Decode(request, parsed));
    w::ConfigureSafetyRequest safety{};
    safety.header = {8, 9};
    safety.lease_us = 20000;
    safety.uart_stop_count = 1;
    safety.uart_stop_actions[0].channel = 6;
    safety.uart_stop_actions[0].length = 1;
    safety.uart_stop_actions[0].data[0] = 0xa5;
    assert(w::Encode(safety, request));
    assert(request[9] == 8 && request[32] == 6 && request[34] == 4);
    w::ConfigureSafetyRequest decoded{};
    assert(w::Decode(request, decoded));
    assert(decoded.lease_us == 20000 && decoded.uart_stop_count == 1);
    assert(decoded.uart_stop_actions[0].data[0] == 0xa5);
    safety.uart_stop_count = 7;
    assert(!w::Encode(safety, request));
    w::UsbInPacket packet{};
    w::ReadyReport ready{};
    ready.header.session_id = 9;
    ready.capabilities = 255;
    ready.protection_signature = 0x12345678;
    assert(w::Encode(ready, packet));
    assert(packet[9] == 2 && w::U32(packet.data() + 24) == 160);
    assert(w::U32(packet.data() + 36) == 0x12345678);
    w::ImuSampleReport imu{};
    imu.temperature_c = 45.0f;
    imu.spi_bytes[24] = 0xa5;
    imu.sample_sequence = 11;
    assert(w::Encode(imu, packet));
    assert(packet[9] == 7 && packet[10] == 25 && packet[44] == 0xa5);
    assert(w::U32(packet.data() + 48) == 0x42340000);  // IEEE754 45℃。
    assert(w::U32(packet.data() + 52) == 11);
    w::RequestResultReport result{};
    result.request_sequence = 13;
    result.request_kind = w::Kind::Arm;
    result.result_code = -22;
    assert(w::Encode(result, packet));
    assert(packet[20] == 9 && w::U32(packet.data() + 24) == uint32_t(-22));
    w::SessionStatusReport session{};
    session.rejected_requests = 17;
    session.max_lease_us = 20000;
    assert(w::Encode(session, packet));
    assert(packet[10] == 0 && w::U32(packet.data() + 20) == 17);
    assert(w::U32(packet.data() + 56) == 20000);
    w::CanStatusReport can{};
    can.channel = 3;
    can.rx_errors = 19;
    assert(w::Encode(can, packet));
    assert(packet[10] == 1 && packet[11] == 3);
    assert(w::U32(packet.data() + 48) == 19);
    w::UartStatusReport uart{};
    uart.channel = 6;
    uart.tx_errors = 23;
    assert(w::Encode(uart, packet));
    assert(packet[10] == 2 && packet[11] == 6);
    assert(w::U32(packet.data() + 28) == 23);
}
