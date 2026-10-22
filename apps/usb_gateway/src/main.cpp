/* SPDX-License-Identifier: Apache-2.0 */
#include "peripherals.h"
#include <cstring>
#include <dust_adapter/can_dispatch.hpp>
#include <dust_adapter/firmware_can_profile.hpp>
#include <dust_adapter/usb_session.hpp>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

namespace {
namespace wire = dust::wire;
using wire::Packet;
struct QueuedCan {
  platform::CanReceiveFrame frame;
  uint32_t session;
};
K_MSGQ_DEFINE(can_packets, sizeof(QueuedCan), 128, 4);
K_MSGQ_DEFINE(peripheral_packets, sizeof(Packet), 128, 4);
K_MSGQ_DEFINE(replies, sizeof(Packet), 16, 4);
K_SEM_DEFINE(uplink_event, 0, 1);
K_THREAD_STACK_DEFINE(uplink_stack, 2048);
k_thread uplink_thread;
atomic_t session_id{}, can_drops{}, peripheral_drops{}, reply_drops{},
    usb_send_errors{};
wire::SessionPolicy policy;
wire::Record uart_stop_actions[wire::kMaxRecords]{};
size_t uart_stop_count = 0;
uint32_t capabilities = 0, rejected = 0, stop_errors = 0, session_epoch = 0;
wire::StopReason last_stop = wire::StopReason::None;
uint32_t NowUs() { return k_cyc_to_us_floor64(k_cycle_get_64()); }

void QueueReply(const Packet &packet) {
  if (k_msgq_put(&replies, &packet, K_NO_WAIT))
    atomic_inc(&reply_drops);
  k_sem_give(&uplink_event);
}
void Result(const wire::Request &request, int result) {
  Packet reply{};
  wire::Header(reply.data(), wire::Kind::Result, 0, policy.Session());
  wire::Put32(reply.data() + 16, wire::U32(request.data() + 4));
  reply[20] = request[9];
  reply[21] = policy.Armed();
  wire::Put32(reply.data() + 24, uint32_t(result));
  QueueReply(reply);
}
void Close(wire::StopReason reason) {
  // Clear publication first; old peripheral records cannot cross into a new
  // session.
  atomic_clear(&session_id);
  // Protection is MCU configuration, not disposable session/host state.
  const auto &frames = dust::can_profile::kProtectionFrames;
  if (platform::ReplaceCanTraffic(frames.data(), frames.size()))
    ++stop_errors;
  gateway::DiscardUartWrites();
  for (size_t i = 0; i < uart_stop_count; ++i) {
    const auto &action = uart_stop_actions[i];
    if (gateway::SubmitUart(action.channel, action.data, action.length))
      ++stop_errors;
  }
  uart_stop_count = 0;
  policy.Close();
  last_stop = reason;
}
void ReceiveCan(const platform::CanReceiveFrame &frame) {
  const uint32_t session = gateway::Session();
  if (!session)
    return;
  const QueuedCan queued{frame, session};
  if (k_msgq_put(&can_packets, &queued, K_NO_WAIT))
    atomic_inc(&can_drops);
  else
    k_sem_give(&uplink_event);
}
bool TakeCan(Packet &packet) {
  const uint32_t session = gateway::Session();
  if (!session)
    return false;
  QueuedCan queued{};
  unsigned count = 0;
  while (count < 2 && !k_msgq_get(&can_packets, &queued, K_NO_WAIT)) {
    if (queued.session != session)
      continue;
    auto *p = packet.data() + 16 + count * 24;
    wire::Encode(p, queued.frame.record);
    wire::Put32(p + 16, queued.frame.timestamp_us);
    wire::Put32(p + 20, queued.frame.sequence);
    ++count;
  }
  if (!count)
    return false;
  wire::Header(packet.data(), wire::Kind::Can, 0, session);
  packet[10] = count;
  return true;
}
void Uplink(void *, void *, void *) {
  bool peripheral_turn = false;
  for (;;) {
    Packet packet{};
    bool found = !k_msgq_get(&replies, &packet, K_NO_WAIT);
    if (!found && peripheral_turn)
      found = !k_msgq_get(&peripheral_packets, &packet, K_NO_WAIT);
    if (!found)
      found = TakeCan(packet);
    if (!found)
      found = !k_msgq_get(&peripheral_packets, &packet, K_NO_WAIT);
    if (!found) {
      k_sem_take(&uplink_event, K_FOREVER);
      continue;
    }
    peripheral_turn = !peripheral_turn;
    if (!gateway::Session() ||
        wire::U32(packet.data() + 12) != gateway::Session())
      continue;
    wire::Put32(packet.data() + 4, NowUs());
    if (platform::SendUsb(packet.data(), packet.size()) != int(packet.size()) ||
        platform::WaitUsbTxComplete(20))
      atomic_inc(&usb_send_errors);
  }
}
int ConfigureSafety(const wire::Request &request) {
  if (!dust::can_profile::AcceptsSafetyRequest(request))
    return -EACCES;
  if (!policy.Fresh(wire::U32(request.data() + 4)))
    return -ESTALE;
  wire::Record uart_actions[wire::kMaxRecords]{};
  size_t uart_count = 0;
  for (unsigned i = 0; i < request[10]; ++i) {
    const auto action = wire::Decode(request.data() + 32 + i * 16);
    if (action.flags != wire::kUartAction)
      continue;
    if (!gateway::UartAvailable(action.channel))
      return -ENODEV;
    uart_actions[uart_count++] = action;
  }
  if (!policy.Configure(wire::U32(request.data() + 16)))
    return -EINVAL;
  uart_stop_count = uart_count;
  std::memcpy(uart_stop_actions, uart_actions, sizeof(uart_actions));
  policy.Observe(wire::U32(request.data() + 4));
  return 0;
}
int Forward(const wire::Request &request) {
  if (!policy.Armed() || policy.Expired(NowUs()))
    return -EACCES;
  if (!policy.Fresh(wire::U32(request.data() + 4)))
    return -ESTALE;
  int result;
  if (request[9] == uint8_t(wire::Kind::UartWrite)) {
    const size_t size = request[11];
    if (!size || size > wire::kMaxUartWrite ||
        !wire::Zero(request.data() + 16, 16) ||
        !wire::Zero(request.data() + 32 + size, request.size() - 32 - size))
      return -EINVAL;
    result = gateway::SubmitUart(request[10], request.data() + 32, size);
  } else {
    if (!wire::ValidRecords(request))
      return -EINVAL;
    wire::Record frames[wire::kMaxRecords]{};
    for (unsigned i = 0; i < request[10]; ++i) {
      frames[i] = wire::Decode(request.data() + 32 + i * 16);
      if (!platform::CanAvailable(frames[i].channel))
        return -ENODEV;
    }
    result = platform::SubmitCanRecords(frames, request[10]);
  }
  if (!result && !policy.Accept(wire::U32(request.data() + 4), NowUs())) {
    Close(wire::StopReason::LeaseExpired);
    return -ETIMEDOUT;
  }
  return result;
}
void Status(const wire::Request &request) {
  Packet reply{};
  wire::Header(reply.data(), wire::Kind::Status, 0, policy.Session());
  reply[10] = request[10];
  reply[11] = request[11];
  if (!request[10]) {
    reply[16] = policy.Armed();
    reply[17] = policy.Configured();
    reply[18] = uint8_t(last_stop);
    wire::Put32(reply.data() + 20, rejected);
    wire::Put32(reply.data() + 24, atomic_get(&can_drops));
    wire::Put32(reply.data() + 28, atomic_get(&peripheral_drops));
    wire::Put32(reply.data() + 32, stop_errors);
    wire::Put32(reply.data() + 36, atomic_get(&reply_drops));
    wire::Put32(reply.data() + 40, atomic_get(&usb_send_errors));
    platform::UsbSessionStats stats{};
    platform::ReadUsbSessionStats(&stats);
    wire::Put32(reply.data() + 44, stats.receive_queue_drops);
    wire::Put32(reply.data() + 48, stats.endpoint_errors);
    wire::Put32(reply.data() + 52, dust::can_profile::kProtectionSignature);
    wire::Put32(reply.data() + 56, dust::can_profile::kCommandTimeoutUs);
  } else if (request[10] == 1) {
    const auto stats = platform::ReadCanStats(request[11]);
    const uint32_t values[] = {
        stats.received, stats.submitted, stats.completed,
        stats.errors,   stats.dropped,   stats.queue_depth,
        stats.state,    stats.tx_errors, stats.rx_errors};
    for (unsigned i = 0; i < 9; ++i)
      wire::Put32(reply.data() + 16 + 4 * i, values[i]);
  } else if (request[10] == 2) {
    const auto stats = gateway::ReadUartStats(request[11]);
    wire::Put32(reply.data() + 16, stats.received);
    wire::Put32(reply.data() + 20, stats.dropped);
    wire::Put32(reply.data() + 24, stats.tx_completed);
    wire::Put32(reply.data() + 28, stats.tx_errors);
  }
  QueueReply(reply);
}
void Handle(const wire::Request &request, size_t length) {
  if (!wire::HasHeader(request.data(), length)) {
    ++rejected;
    return;
  }
  const auto kind = wire::Kind(request[9]);
  if (kind == wire::Kind::Hello && length == wire::kOutBytes && !request[10] &&
      !request[11] && wire::Zero(request.data() + 16, 112)) {
    const uint32_t session = wire::U32(request.data() + 12);
    if (!session) {
      ++rejected;
      return;
    }
    if (policy.Session())
      Close(wire::StopReason::Replaced);
    policy.Open(session);
    session_epoch = platform::UsbConnectionEpoch();
    k_msgq_purge(&can_packets);
    k_msgq_purge(&peripheral_packets);
    k_msgq_purge(&replies);
    atomic_set(&session_id, session);
    Packet ready{};
    wire::Header(ready.data(), wire::Kind::Ready, 0, session);
    wire::Put32(ready.data() + 16, capabilities);
    wire::Put32(ready.data() + 20, wire::kOutBytes);
    wire::Put32(ready.data() + 24, wire::kInBytes);
    wire::Put32(ready.data() + 28, platform::CanChannelMask());
    ready[32] = wire::kMaxRecords;
    ready[33] = dust::can_profile::kProtectionFrames.size();
    ready[34] = wire::kFirmwareCanProtection;
    wire::Put32(ready.data() + 36, dust::can_profile::kProtectionSignature);
    wire::Put32(ready.data() + 40, dust::can_profile::kCommandTimeoutUs);
    QueueReply(ready);
    return;
  }
  if (length != wire::kOutBytes || !policy.Session() ||
      wire::U32(request.data() + 12) != policy.Session()) {
    ++rejected;
    return;
  }
  int rc = 0;
  switch (kind) {
  case wire::Kind::ConfigureSafety:
    rc = ConfigureSafety(request);
    Result(request, rc);
    break;
  case wire::Kind::Arm:
    if (request[10] || request[11] || !wire::Zero(request.data() + 16, 112) ||
        !policy.Arm(wire::U32(request.data() + 4), NowUs()))
      rc = -EINVAL;
    Result(request, rc);
    break;
  case wire::Kind::Transmit:
  case wire::Kind::UartWrite:
    rc = Forward(request);
    if (rc)
      Result(request, rc); // successful data traffic has no periodic ACK
    break;
  case wire::Kind::Heartbeat:
    if (request[10] || request[11] || !wire::Zero(request.data() + 16, 112) ||
        !policy.Accept(wire::U32(request.data() + 4), NowUs()))
      rc = -EINVAL;
    break;
  case wire::Kind::Stop:
    Close(wire::StopReason::Explicit);
    break;
  case wire::Kind::Status:
    if (request[10] > 2 || !wire::Zero(request.data() + 16, 112))
      rc = -EINVAL;
    else
      Status(request);
    break;
  default:
    rc = -ENOTSUP;
    Result(request, rc);
    break;
  }
  if (rc)
    ++rejected;
  // Queue admission failure must never silently leave an armed controller
  // running.
  if ((rc == -ENOSPC || rc == -EIO) && policy.Armed())
    Close(wire::StopReason::TransportError);
}
} // namespace

namespace gateway {
uint32_t Session() { return uint32_t(atomic_get(&session_id)); }
bool PublishPeripheral(const Packet &packet) {
  if (!Session())
    return false;
  if (k_msgq_put(&peripheral_packets, &packet, K_NO_WAIT)) {
    atomic_inc(&peripheral_drops);
    return false;
  }
  k_sem_give(&uplink_event);
  return true;
}
} // namespace gateway

int main() {
  k_thread_priority_set(k_current_get(), 7);
  if (platform::InitializeCanDispatch(ReceiveCan))
    return -1;
  if (dust::can_profile::kProtectOnBoot) {
    const auto &frames = dust::can_profile::kProtectionFrames;
    if (platform::ReplaceCanTraffic(frames.data(), frames.size()))
      ++stop_errors;
  }
  capabilities = 1 | gateway::StartPeripherals();
  if (platform::InitializeUsbSession())
    return -1;
  k_thread_create(&uplink_thread, uplink_stack,
                  K_THREAD_STACK_SIZEOF(uplink_stack), Uplink, nullptr, nullptr,
                  nullptr, 7, 0, K_NO_WAIT);
  for (;;) {
    // Check even under continuous incoming traffic; malformed packets cannot
    // keep a lease alive.
    if (policy.Session() && (!platform::IsUsbConfigured() ||
                             platform::UsbConnectionEpoch() != session_epoch))
      Close(wire::StopReason::UsbLost);
    else if (policy.Expired(NowUs()))
      Close(wire::StopReason::LeaseExpired);
    const bool output_fault =
        platform::TakeCanFault() | gateway::TakePeripheralFault();
    if (output_fault && policy.Armed())
      Close(wire::StopReason::TransportError);
    static uint32_t last_usb_errors = 0, last_out_drops = 0,
                    last_endpoint_errors = 0;
    const uint32_t usb_errors = atomic_get(&usb_send_errors);
    platform::UsbSessionStats usb_stats{};
    platform::ReadUsbSessionStats(&usb_stats);
    const bool transport_fault =
        usb_errors != last_usb_errors ||
        usb_stats.receive_queue_drops != last_out_drops ||
        usb_stats.endpoint_errors != last_endpoint_errors;
    if (transport_fault && policy.Armed())
      Close(wire::StopReason::TransportError);
    last_usb_errors = usb_errors;
    last_out_drops = usb_stats.receive_queue_drops;
    last_endpoint_errors = usb_stats.endpoint_errors;
    wire::Request request{};
    size_t length = 0;
    if (!platform::ReceiveUsb(request.data(), request.size(), &length, 1)) {
      if (policy.Session() && platform::UsbConnectionEpoch() != session_epoch)
        Close(wire::StopReason::UsbLost);
      if (policy.Expired(NowUs()))
        Close(wire::StopReason::LeaseExpired);
      Handle(request, length);
    }
  }
}
