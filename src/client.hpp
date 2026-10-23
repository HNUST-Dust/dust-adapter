/* SPDX-License-Identifier: Apache-2.0 */
/** @file client.hpp
 * @brief Host request builders; caller owns transport and sequence allocation.
 */
#pragma once
#include <stdexcept>

#include "protocol.hpp"

namespace dust {
// Allocation-free packet builders. Transport ownership and sequence allocation
// belong to the caller.
/**
 * @brief Build a zero-padded request; caller supplies kind, sequence and
 * session.
 */
inline wire::Request Control(wire::Kind kind, uint32_t sequence,
                             uint32_t session) {
    wire::Request request{};
    wire::Header(request.data(), kind, sequence, session);
    return request;
}
/**
 * @brief Build a lease/protection request; actions may be null only when count
 * is zero.
 * @throws std::invalid_argument If lease, count or action encoding is invalid.
 */
inline wire::Request SafetyConfiguration(uint32_t sequence, uint32_t session,
                                         uint32_t lease_us,
                                         const wire::Record* actions,
                                         size_t count) {
    if (count > wire::kMaxRecords || (count && !actions) || lease_us < 1000 ||
        lease_us > 1000000)
        throw std::invalid_argument("Invalid adapter safety configuration");
    auto request = Control(wire::Kind::ConfigureSafety, sequence, session);
    request[10] = uint8_t(count);
    wire::Put32(request.data() + 16, lease_us);
    for (size_t i = 0; i < count; ++i)
        wire::Encode(request.data() + 32 + 16 * i, actions[i]);
    if (!wire::ValidRecords(request, true))
        throw std::invalid_argument("Invalid stop action");
    return request;
}
// Selects the MCU's compiled protection table; zero count does not disable it.
/**
 * @brief Select compiled MCU protection without replacing or deleting it.
 * @throws std::invalid_argument If lease_us is outside the wire range.
 */
inline wire::Request SelectFirmwareSafety(uint32_t sequence, uint32_t session,
                                          uint32_t lease_us) {
    return SafetyConfiguration(sequence, session, lease_us, nullptr, 0);
}
/**
 * @brief Build a CAN batch; records may be null only for an empty batch.
 * @throws std::invalid_argument If count, pointer or record encoding is
 * invalid.
 */
inline wire::Request CanTransmit(uint32_t sequence, uint32_t session,
                                 const wire::Record* records, size_t count) {
    if (count > wire::kMaxRecords || (count && !records))
        throw std::invalid_argument("Invalid CAN batch");
    auto request = Control(wire::Kind::Transmit, sequence, session);
    request[10] = uint8_t(count);
    for (size_t i = 0; i < count; ++i)
        wire::Encode(request.data() + 32 + 16 * i, records[i]);
    if (!wire::ValidRecords(request))
        throw std::invalid_argument("Invalid CAN record");
    return request;
}
/**
 * @brief Build a UART request by copying length bytes from bytes.
 * @throws std::invalid_argument If port, pointer or payload length is invalid.
 */
inline wire::Request UartWrite(uint32_t sequence, uint32_t session,
                               uint8_t port, const uint8_t* bytes,
                               size_t length) {
    if (port < 1 || port > 3 || !length || length > wire::kMaxUartWrite ||
        !bytes)
        throw std::invalid_argument("Invalid UART write");
    auto request = Control(wire::Kind::UartWrite, sequence, session);
    request[10] = port;
    request[11] = uint8_t(length);
    std::memcpy(request.data() + 32, bytes, length);
    return request;
}
}  // namespace dust
