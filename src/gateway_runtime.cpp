/* SPDX-License-Identifier: Apache-2.0 */

/**
 * @file gateway_runtime.cpp
 * @brief USB 请求处理、会话保护及外设转发协调。
 */
#include "gateway_runtime.hpp"

#include <zephyr/kernel.h>

#include <cstring>

#include "drivers/board_indicators.hpp"
#include "drivers/can_port.hpp"
#include "drivers/firmware_can_profile.hpp"
#include "drivers/imu_heater.hpp"
#include "drivers/uart_port.hpp"
#include "drivers/usb_transport.hpp"
#include "imu_reports.hpp"
#include "usb_reports.hpp"
#include "usb_session_policy.hpp"

namespace {
namespace wire = dust::wire;
using wire::UsbInPacket;

constexpr int kRequestPriority = 7;

wire::SessionPolicy usb_session_policy;
wire::UartStopAction uart_stop_actions[wire::kMaxRecords]{};
size_t uart_stop_count = 0;
uint32_t usb_report_capabilities = 0;
uint32_t rejected_usb_requests = 0;
uint32_t stop_errors = 0;
uint32_t usb_connection_epoch = 0;
wire::StopReason last_stop = wire::StopReason::None;

/**
 * @brief 返回单调微秒时间的低 32 位。
 */
uint32_t NowUs() {
    return k_cyc_to_us_floor64(k_cycle_get_64());
}

/**
 * @brief 为指定请求排队发送结果码和转发授权状态。
 */
void QueueUsbRequestResult(const wire::UsbOutRequest& request, int result) {
    const wire::UsbOutRequestView fields(request);
    wire::RequestResultReport result_report{};
    result_report.header.session_id = usb_session_policy.Session();
    result_report.request_sequence = fields.Sequence();
    result_report.request_kind = fields.MessageKind();
    result_report.armed = usb_session_policy.Armed();
    result_report.result_code = result;
    UsbInPacket reply{};
    wire::Encode(result_report, reply);
    gateway::QueueUsbReply(reply);
}

/**
 * @brief 停止发布，以保护帧替换 CAN 业务流量，并执行 UART 停止动作。
 */
void CloseSession(wire::StopReason reason) {
    // 先清除发布会话，防止旧外设记录进入
    // 新会话。
    gateway::SetUsbReportSession(0);
    // 保护表属于 MCU 配置，不是可随会话或主机丢弃的状态。
    const auto& frames = dust::can_profile::kProtectionFrames;
    if (platform::ReplaceCanTraffic(frames.data(), frames.size())) {
        ++stop_errors;
    }
    gateway::DiscardUartWrites();
    for (size_t i = 0; i < uart_stop_count; ++i) {
        const auto& action = uart_stop_actions[i];
        if (gateway::SubmitUart(action.channel, action.data, action.length)) {
            ++stop_errors;
        }
    }
    uart_stop_count = 0;
    usb_session_policy.Close();
    last_stop = reason;
}

/**
 * @brief 校验并安装会话 UART 动作和租约；返回零或负 errno。
 */
int ConfigureSafety(const wire::UsbOutRequest& request) {
    if (!dust::can_profile::AcceptsSafetyRequest(request)) {
        return -EACCES;
    }
    wire::ConfigureSafetyRequest configuration{};
    if (!wire::Decode(request, configuration)) {
        return -EINVAL;
    }
    if (!usb_session_policy.Fresh(configuration.header.request_sequence)) {
        return -ESTALE;
    }
    for (size_t i = 0; i < configuration.uart_stop_count; ++i) {
        if (!gateway::UartAvailable(
                configuration.uart_stop_actions[i].channel)) {
            return -ENODEV;
        }
    }
    if (!usb_session_policy.Configure(configuration.lease_us)) {
        return -EINVAL;
    }
    uart_stop_count = configuration.uart_stop_count;
    std::memcpy(uart_stop_actions, configuration.uart_stop_actions.data(),
                sizeof(uart_stop_actions));
    usb_session_policy.Observe(configuration.header.request_sequence);
    return 0;
}

/**
 * @brief 接纳已授权且序号有效的请求，提交成功后续租。
 * @return 接纳成功返回零，否则返回负 errno；接纳不代表硬件传输完成。
 */
int ForwardUsbRequest(const wire::UsbOutRequest& request) {
    const wire::UsbOutRequestView fields(request);
    if (!usb_session_policy.Armed() || usb_session_policy.Expired(NowUs())) {
        return -EACCES;
    }
    if (!usb_session_policy.Fresh(fields.Sequence())) {
        return -ESTALE;
    }
    int result;
    if (fields.MessageKind() == wire::Kind::UartTransmitRequest) {
        wire::UartTransmitRequest uart_request{};
        if (!wire::Decode(request, uart_request)) {
            return -EINVAL;
        }
        result =
            gateway::SubmitUart(uart_request.channel, uart_request.bytes.data(),
                                uart_request.byte_count);
    } else {
        wire::CanTransmitRequest can_request{};
        if (!wire::Decode(request, can_request)) {
            return -EINVAL;
        }
        wire::CanFrame frames[wire::kMaxRecords]{};
        for (size_t i = 0; i < can_request.frame_count; ++i) {
            frames[i] = can_request.frames[i];
            if (!platform::CanAvailable(frames[i].channel)) {
                return -ENODEV;
            }
        }
        result = platform::SubmitCanRecords(frames, can_request.frame_count);
    }
    if (!result && !usb_session_policy.Accept(fields.Sequence(), NowUs())) {
        CloseSession(wire::StopReason::LeaseExpired);
        return -ETIMEDOUT;
    }
    return result;
}

/**
 * @brief 将请求的会话、CAN 或 UART 诊断快照排队发送。
 */
void QueueUsbStatusReply(const wire::UsbOutRequest& request) {
    wire::StatusRequest query{};
    if (!wire::Decode(request, query)) {
        return;
    }
    UsbInPacket reply{};
    if (query.target == wire::StatusTarget::Session) {
        const auto reports = gateway::ReadUsbReportStats();
        platform::UsbTransportStats transport{};
        platform::ReadUsbTransportStats(&transport);
        wire::SessionStatusReport status{};
        status.channel = query.channel;
        status.header.session_id = usb_session_policy.Session();
        status.armed = usb_session_policy.Armed();
        status.configured = usb_session_policy.Configured();
        status.stop_reason = last_stop;
        status.rejected_requests = rejected_usb_requests;
        status.can_report_drops = reports.can_rx_drops;
        status.sensor_report_drops = reports.sensor_report_drops;
        status.stop_errors = stop_errors;
        status.reply_drops = reports.reply_drops;
        status.usb_send_errors = reports.send_errors;
        status.usb_receive_drops = transport.receive_queue_drops;
        status.usb_endpoint_errors = transport.endpoint_errors;
        status.protection_signature = dust::can_profile::kProtectionSignature;
        status.max_lease_us = dust::can_profile::kCommandTimeoutUs;
        wire::Encode(status, reply);
    } else if (query.target == wire::StatusTarget::Can) {
        const auto stats = platform::ReadCanStats(query.channel);
        wire::CanStatusReport status{};
        status.header.session_id = usb_session_policy.Session();
        status.channel = query.channel;
        status.received = stats.received;
        status.submitted = stats.submitted;
        status.completed = stats.completed;
        status.errors = stats.errors;
        status.dropped = stats.dropped;
        status.queue_depth = stats.queue_depth;
        status.controller_state = stats.state;
        status.tx_errors = stats.tx_errors;
        status.rx_errors = stats.rx_errors;
        wire::Encode(status, reply);
    } else {
        const auto stats = gateway::ReadUartStats(query.channel);
        wire::UartStatusReport status{};
        status.header.session_id = usb_session_policy.Session();
        status.channel = query.channel;
        status.received_blocks = stats.received;
        status.dropped_blocks = stats.dropped;
        status.completed_writes = stats.tx_completed;
        status.tx_errors = stats.tx_errors;
        wire::Encode(status, reply);
    }
    gateway::QueueUsbReply(reply);
}

/**
 * @brief 在策略所属线程中校验并分发一次 OUT 传输。
 */
void HandleUsbRequest(const wire::UsbOutRequest& request, size_t length) {
    const wire::UsbOutRequestView fields(request);
    if (!wire::HasHeader(request.data(), length)) {
        ++rejected_usb_requests;
        return;
    }
    const auto kind = fields.MessageKind();
    wire::HelloRequest hello{};
    if (kind == wire::Kind::Hello && length == wire::kOutBytes &&
        wire::Decode(request, hello)) {
        const uint32_t session = hello.header.session_id;
        if (!session) {
            ++rejected_usb_requests;
            return;
        }
        if (usb_session_policy.Session()) {
            CloseSession(wire::StopReason::Replaced);
        }
        usb_session_policy.Open(session);
        usb_connection_epoch = platform::UsbConnectionEpoch();
        gateway::ResetUsbReportQueues();
        gateway::SetUsbReportSession(session);
        wire::ReadyReport ready_report{};
        ready_report.header.session_id = session;
        ready_report.capabilities = usb_report_capabilities;
        ready_report.can_channel_mask = platform::CanChannelMask();
        ready_report.protection_frame_count =
            dust::can_profile::kProtectionFrames.size();
        ready_report.protection_signature =
            dust::can_profile::kProtectionSignature;
        ready_report.max_lease_us = dust::can_profile::kCommandTimeoutUs;
        UsbInPacket ready{};
        wire::Encode(ready_report, ready);
        gateway::QueueUsbReply(ready);
        return;
    }
    if (length != wire::kOutBytes || !usb_session_policy.Session() ||
        fields.Session() != usb_session_policy.Session()) {
        ++rejected_usb_requests;
        return;
    }
    int rc = 0;
    switch (kind) {
        case wire::Kind::ConfigureSafety:
            rc = ConfigureSafety(request);
            QueueUsbRequestResult(request, rc);
            break;
        case wire::Kind::Arm: {
            wire::ArmRequest arm{};
            if (!wire::Decode(request, arm) ||
                !usb_session_policy.Arm(arm.header.request_sequence, NowUs())) {
                rc = -EINVAL;
            }
            QueueUsbRequestResult(request, rc);
            break;
        }
        case wire::Kind::CanTransmitRequest:
        case wire::Kind::UartTransmitRequest:
            rc = ForwardUsbRequest(request);
            // 成功的数据传输不需要 ACK。
            if (rc) {
                QueueUsbRequestResult(request, rc);
            }
            break;
        case wire::Kind::Heartbeat: {
            wire::HeartbeatRequest heartbeat{};
            if (!wire::Decode(request, heartbeat) ||
                !usb_session_policy.Accept(heartbeat.header.request_sequence,
                                           NowUs())) {
                rc = -EINVAL;
            }
            break;
        }
        case wire::Kind::Stop:
            CloseSession(wire::StopReason::Explicit);
            break;
        case wire::Kind::Status: {
            wire::StatusRequest status{};
            if (!wire::Decode(request, status)) {
                rc = -EINVAL;
            } else {
                QueueUsbStatusReply(request);
            }
            break;
        }
        default:
            rc = -ENOTSUP;
            QueueUsbRequestResult(request, rc);
            break;
    }
    if (rc) {
        ++rejected_usb_requests;
    }
    // 队列接纳失败时，必须处理仍获转发授权的控制器，
    // 不能让它静默地继续运行。
    if ((rc == -ENOSPC || rc == -EIO) && usb_session_policy.Armed()) {
        CloseSession(wire::StopReason::TransportError);
    }
}
}  // namespace

namespace gateway {

/**
 * @brief 初始化硬件并运行唯一的策略所有者；初始化失败返回 -1。
 */
int RunGateway() {
    k_thread_priority_set(k_current_get(), kRequestPriority);
    if (platform::InitializeCanPorts(gateway::QueueCanRxForUsb)) {
        return -1;
    }
    if (dust::can_profile::kProtectOnBoot) {
        const auto& frames = dust::can_profile::kProtectionFrames;
        if (platform::ReplaceCanTraffic(frames.data(), frames.size())) {
            ++stop_errors;
        }
    }
    if (platform::InitializeImuHeater() ||
        platform::InitializeBoardIndicators()) {
        return -1;
    }
    usb_report_capabilities =
        1 | gateway::StartUartPorts() | gateway::StartImuReports();
    if (platform::InitializeUsbTransport()) {
        return -1;
    }
    gateway::StartUsbReports();
    uint32_t last_usb_errors = 0;
    uint32_t last_out_drops = 0;
    uint32_t last_endpoint_errors = 0;

    for (;;) {
        // 即使持续收到流量也要检查租约，
        // 畸形数据包不能维持租约。
        if (usb_session_policy.Session() &&
            (!platform::IsUsbConfigured() ||
             platform::UsbConnectionEpoch() != usb_connection_epoch)) {
            CloseSession(wire::StopReason::UsbLost);
        } else if (usb_session_policy.Expired(NowUs())) {
            CloseSession(wire::StopReason::LeaseExpired);
        }
        // 两个读取都会清除锁存故障，必须分别执行，不能短路。
        const bool can_output_fault = platform::TakeCanFault();
        const bool uart_output_fault = gateway::TakeUartFault();
        const bool output_fault = can_output_fault || uart_output_fault;
        if (output_fault && usb_session_policy.Armed()) {
            CloseSession(wire::StopReason::TransportError);
        }
        const uint32_t usb_errors = gateway::ReadUsbReportStats().send_errors;
        platform::UsbTransportStats usb_stats{};
        platform::ReadUsbTransportStats(&usb_stats);
        const bool transport_fault =
            usb_errors != last_usb_errors ||
            usb_stats.receive_queue_drops != last_out_drops ||
            usb_stats.endpoint_errors != last_endpoint_errors;
        if (transport_fault && usb_session_policy.Armed()) {
            CloseSession(wire::StopReason::TransportError);
        }
        last_usb_errors = usb_errors;
        last_out_drops = usb_stats.receive_queue_drops;
        last_endpoint_errors = usb_stats.endpoint_errors;
        platform::SetBoardState(usb_session_policy.Armed(),
                                last_stop == wire::StopReason::TransportError ||
                                    platform::ImuHeaterFaulted());
        wire::UsbOutRequest request{};
        size_t length = 0;
        if (!platform::ReceiveUsb(request.data(), request.size(), &length, 1)) {
            if (usb_session_policy.Session() &&
                platform::UsbConnectionEpoch() != usb_connection_epoch) {
                CloseSession(wire::StopReason::UsbLost);
            }
            if (usb_session_policy.Expired(NowUs())) {
                CloseSession(wire::StopReason::LeaseExpired);
            }
            HandleUsbRequest(request, length);
        }
    }
}

}  // namespace gateway
