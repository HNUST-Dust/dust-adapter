/* SPDX-License-Identifier: Apache-2.0 */
#include <cassert>
#include <cstdio>
#include "ethercat/protocol.hpp"
#include "ethercat/pdo_schema.hpp"
int main() {
    using namespace dust::ecat;
    // Build the fixed production wiring from the single schema source.
    const auto production = dust::pdo::ProductionConfig();
    Configuration config;
    config.cycle_us = dust::pdo::kProductionCycleUs;
    config.can_tx = production.can_tx;
    config.can_rx = production.can_rx;
    for (unsigned i = 0; i < 6; ++i) {
        config.uart[i] = {production.uart_tx[i], production.uart_rx[i]};
    }
    config.imu = production.imu;
    Limits limits{4096, 4096, 24576, 15, 63, 63, true};
    Layout mapped;
    assert(BuildLayout(config, limits, mapped) == Error::None);
    assert(mapped.rx.bytes == 131 && mapped.tx.bytes == 410);
    // Locked production signature (single source of truth: pdo_schema.yaml).
    assert(mapped.signature == 0xf7babb63U);
    const uint32_t values[] = {
        0x78dabd14U, 125, 20000, 6, 0, 0, 0, 3, 3,
        0, 0, 0, 128, 0, 0, 0, 0, 0, 32, 0, 0, 1,
    };
    ConfigurationTransaction transaction;
    for (unsigned i = 0; i < 22; ++i)
        assert(transaction.Write(uint8_t(i + 1), values[i]) == Error::None);
    Layout proposed;
    // A different startup signature must not configure the production layout.
    assert(transaction.Commit(limits, mapped.rx, mapped.tx, proposed) == Error::Signature);
    assert(transaction.Write(1, 0xca865654U) == Error::None);
    assert(transaction.Commit(limits, mapped.rx, mapped.tx, proposed) == Error::Signature);
    assert(transaction.Write(1, mapped.signature) == Error::None);
    assert(transaction.Commit(limits, mapped.rx, mapped.tx, proposed) == Error::None);
}
