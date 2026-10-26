/* SPDX-License-Identifier: Apache-2.0 */
#include <algorithm>
#include <cassert>

#include "usb_protocol.hpp"

int main() {
    namespace w = dust::wire;
    w::CanFrame frame{};
    frame.channel = 2;
    frame.length = 1;
    frame.id = 0x201;
    frame.data[0] = 0xa5;
    dust::wire::UsbOutRequest request{};
    w::CanTransmitRequest command{};
    command.header = {7, 9};
    command.frame_count = 1;
    command.frames[0] = frame;
    assert(w::Encode(command, request));
    assert(request[9] == 3 && request[10] == 1);
    w::CanTransmitRequestView can_request(request);
    assert(can_request.IsValid() && can_request.FrameCount() == 1);
    assert(can_request.Frame(0).id == 0x201);
    w::CanTransmitRequest decoded_can{};
    assert(w::Decode(request, decoded_can));
    assert(decoded_can.header.request_sequence == 7);
    assert(decoded_can.frames[0].id == 0x201);
    w::UsbOutRequest encoded_can{};
    assert(w::Encode(decoded_can, encoded_can));
    assert(encoded_can == request);
    w::CanReceiveReport logical_report{};
    logical_report.header.session_id = 9;
    logical_report.frame_count = 6;
    for (auto& received : logical_report.frames) {
        received = {frame, 123, 456};
    }
    w::UsbInPacket encoded_report{};
    assert(w::Encode(logical_report, encoded_report));
    assert(w::UsbReportLength(encoded_report) == 160);
    assert(w::U32(encoded_report.data() + 16 + 5 * 24 + 16) == 123);
    w::UsbInPacket packet{};
    w::CanReceiveReportBuilder report(packet, 9);
    assert(report.AddFrame(frame, 0x12345678, 11));
    assert(w::UsbReportLength(packet) == 40);
    assert(packet[9] == 4 && packet[10] == 1);
    assert(packet[16] == 2 && packet[20] == 1 && packet[21] == 2);
    assert(packet[24] == 0xa5);
    assert(w::U32(packet.data() + 32) == 0x12345678);
    assert(w::U32(packet.data() + 36) == 11);
    assert(report.AddFrame(frame, 1, 12));
    assert(w::UsbReportLength(packet) == 64);
    for (unsigned i = 2; i < w::kMaxCanFramesPerPacket; ++i) {
        assert(report.AddFrame(frame, 2, 13 + i));
    }
    assert(w::UsbReportLength(packet) == 160);
    assert(!report.AddFrame(frame, 2, 20));
    const uint8_t bytes[] = {0xa5, 0x5a};
    w::UartTransmitRequest uart_command{};
    uart_command.header = {8, 9};
    uart_command.channel = 4;
    uart_command.byte_count = 2;
    std::copy_n(bytes, 2, uart_command.bytes.begin());
    assert(w::Encode(uart_command, request));
    w::UartTransmitRequestView uart_request(request);
    assert(uart_request.IsValid() && uart_request.Channel() == 4);
    assert(uart_request.ByteCount() == 2 && uart_request.Bytes()[1] == 0x5a);
    w::UartTransmitRequest decoded_uart{};
    assert(w::Decode(request, decoded_uart));
    w::UsbOutRequest encoded_uart{};
    assert(w::Encode(decoded_uart, encoded_uart));
    assert(encoded_uart == request);
    decoded_uart.byte_count = 97;
    assert(!w::Encode(decoded_uart, encoded_uart));
    for (uint8_t channel = 4; channel <= 6; ++channel) {
        uart_command.channel = channel;
        assert(w::Encode(uart_command, encoded_uart));
        assert(encoded_uart[10] == channel);
    }
    uart_command.channel = 7;
    assert(!w::Encode(uart_command, encoded_uart));
    request[127] = 1;
    assert(!uart_request.IsValid());
    assert(w::BuildUartReceiveReport(packet, 9, 4, 12, 34, bytes, 2));
    assert(packet[9] == 6 && packet[10] == 4 && packet[11] == 2);
    assert(w::U32(packet.data() + 16) == 12);
    assert(w::U32(packet.data() + 20) == 34);
    assert(packet[24] == 0xa5 && packet[25] == 0x5a && packet[26] == 0);
    assert(!w::BuildUartReceiveReport(packet, 9, 4, 12, 34, bytes, 41));
}

// Wire v4：固定检查编号和版本，避免两端仅改名而未同步编码。
static_assert(dust::wire::kVersion == 4);
static_assert(uint8_t(dust::wire::Kind::CanTransmitRequest) == 3);
static_assert(uint8_t(dust::wire::Kind::CanReceiveReport) == 4);
static_assert(uint8_t(dust::wire::Kind::UartTransmitRequest) == 5);
static_assert(uint8_t(dust::wire::Kind::UartReceiveReport) == 6);
static_assert(uint8_t(dust::wire::Kind::ImuSampleReport) == 7);
static_assert(uint8_t(dust::wire::Kind::Stop) == 10);
static_assert(uint8_t(dust::wire::Kind::Heartbeat) == 11);
static_assert(uint8_t(dust::wire::Kind::Result) == 12);
static_assert(uint8_t(dust::wire::Kind::Status) == 13);
