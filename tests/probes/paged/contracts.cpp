// SPDX-License-Identifier: Apache-2.0
#include <array>
#include <cstdlib>
#include <iostream>

#include "adapters/ethercat_ssc/layout_service.hpp"
#include "adapters/ethercat_ssc/paged_layout.hpp"
#include "common/paged_wire.hpp"
#include "domain/protocol/session.hpp"
#include "domain/protocol/wire.hpp"
#define Check(v)                                             \
    do {                                                     \
        if (!(v)) {                                          \
            std::cerr << "failed line " << __LINE__ << "\n"; \
            std::abort();                                    \
        }                                                    \
    } while (0)
namespace p = dust::paged;
using namespace ecat;
void Stage(ConfigurationTransaction& t, uint32_t sig) {
    for (unsigned i = 1; i <= 22; ++i) {
        uint32_t v = 0;
        if (i == 1) {
            v = sig;
        }
        if (i == 2) {
            v = 125;
        }
        if (i == 3) {
            v = 20000;
        }
        if (i == 4) {
            v = 6;
        }
        if (i == 6 || i == 9) {
            v = 3;
        }
        if (i == 13) {
            v = 128;
        }
        if (i == 19) {
            v = 32;
        }
        if (i == 22) {
            v = 1;
        }
        Check(t.Write(i, v) == Error::None);
    }
}
void Map(PdoDictionary& d, const ImageLayout& image, bool tx) {
    unsigned pos = 0;
    const unsigned assign = tx ? 0x1c13 : 0x1c12;
    Check(d.Write(assign, 0, 1, 0) == Error::None);
    for (unsigned m = 0; m < image.mapping_count; ++m) {
        const auto& map = image.mappings[m];
        Check(d.Write(map.index, 0, 1, 0) == Error::None);
        for (unsigned i = 0; i < map.count; ++i) {
            const auto& e = image.entries[pos++];
            Check(d.Write(map.index, i + 1, 4,
                          uint32_t(e.index) << 16 | uint32_t(e.subindex) << 8 |
                              e.bits) == Error::None);
        }
        Check(d.Write(map.index, 0, 1, map.count) == Error::None);
        Check(d.Write(assign, m + 1, 2, map.index) == Error::None);
    }
    Check(d.Write(assign, 0, 1, image.mapping_count) == Error::None);
}
int main() {
    std::array<uint8_t, 22> header{};
    p::Put32(header.data(), 7);
    p::Put32(header.data() + 4, 42);
    header[20] = 2;
    std::array<uint8_t, p::kBytes> page{};
    std::array<uint8_t, 143> payload{};
    payload.fill(0x5a);
    p::Receiver receiver;
    uint32_t seq = 0;
    for (auto kind :
         {p::Kind::Can, p::Kind::Imu, p::Kind::Uart, p::Kind::Diagnostics}) {
        uint8_t channel = kind == p::Kind::Uart ? 1 : 0;
        p::Put32(header.data() + 8, ++seq);
        Check(p::Encode(header.data(), kind, channel, payload.data(),
                        p::Length(kind, channel), page.data()));
        p::View view;
        Check(p::Decode(page.data(), page.size(), view));
        Check(receiver.Accept(page.data(), page.size()));
        Check(receiver.Accept(page.data(), page.size()));
        auto bad = page;
        bad[40] ^= 1;
        Check(!receiver.Accept(bad.data(), bad.size()));
        bad = page;
        bad[34] = 1;
        Check(!p::Decode(bad.data(), bad.size(), view));
        bad = page;
        p::Put16(bad.data() + 32, 153);
        Check(!p::Decode(bad.data(), bad.size(), view));
        bad = page;
        bad[26] = 99;
        Check(!p::Decode(bad.data(), bad.size(), view));
        Check(!p::Decode(page.data(), page.size() - 1, view));
    }
    Check(receiver.data()[22] == 0x5a && receiver.data()[344] == 0x5a &&
          receiver.data()[154] == 0x5a);
    header[20] = 3;
    p::Put32(header.data() + 8, ++seq);
    Check(p::Encode(header.data(), p::Kind::None, 0, nullptr, 0, page.data()));
    Check(receiver.Accept(page.data(), page.size()));
    Check(receiver.data()[20] == 3 && receiver.data()[22] == 0 &&
          receiver.data()[344] == 0);
    header[20] = 2;
    p::Put32(header.data(), 8);
    p::Put32(header.data() + 8, 1);
    Check(p::Encode(header.data(), p::Kind::Imu, 0, payload.data(), 38,
                    page.data()));
    Check(receiver.Accept(page.data(), page.size()));
    Check(receiver.data()[22] == 0);
    receiver.Reset();
    p::Put32(header.data() + 8, 0xffffffff);
    Check(p::Encode(header.data(), p::Kind::Imu, 0, payload.data(), 38,
                    page.data()));
    Check(receiver.Accept(page.data(), page.size()));
    p::Put32(header.data() + 8, 0);
    Check(p::Encode(header.data(), p::Kind::Imu, 0, payload.data(), 38,
                    page.data()));
    Check(receiver.Accept(page.data(), page.size()));

    Configuration c;
    c.cycle_us = 125;
    c.can_tx = {6, 0};
    c.can_rx = {3, 0, 0, 3};
    c.uart[1].rx_bytes = 128;
    c.uart[4].rx_bytes = 32;
    c.imu = true;
    Limits limits{1024, 1024, 8192, 9, 0, 18, true};
    Layout logical;
    Check(BuildLayout(c, limits, logical) == Error::None);
    Check(logical.tx.bytes == 410);
    LayoutService service;
    Stage(service.transaction(), logical.signature ^ p::kSignatureXor);
    Map(service.dictionary(), logical.rx, false);
    Map(service.dictionary(), PageCarrierLayout(), true);
    Check(service.GenerateMapping(limits) == Error::None);
    Check(service.physical_tx().bytes == 192 &&
          service.mapped_tx().bytes == 410);
    LayoutService bad;
    Stage(bad.transaction(), logical.signature);
    Map(bad.dictionary(), logical.rx, false);
    Map(bad.dictionary(), PageCarrierLayout(), true);
    Check(bad.GenerateMapping(limits) == Error::Signature);
    bad.Reset();
    Stage(bad.transaction(), logical.signature ^ p::kSignatureXor);
    Map(bad.dictionary(), logical.rx, false);
    Map(bad.dictionary(), logical.tx, true);
    Check(bad.GenerateMapping(limits) == Error::Mapping);

    Protection protection;
    protection.installed = true;
    protection.stop = [](void*, Error) {};
    protection.validate_can = [](void*, unsigned, const CanFrame&) {
        return true;
    };
    Session session(7, protection);
    Check(session.Configure(service.session_transaction(), limits, logical.rx,
                            logical.tx) == Error::None);
    std::array<uint8_t, 1024> rx{}, tx{};
    Put32(rx.data(), 42);
    Put32(rx.data() + 4, 7);
    Put32(rx.data() + 8, 1);
    rx[12] = kArm;
    Actions actions;
    Check(session.Process({rx.data(), logical.rx.bytes}, 1000, true, actions) ==
          Error::None);
    uint8_t bytes[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    Check(session.QueueUart(1, {bytes, 8}, 1000, 1000));
    bytes[0] = 9;
    Check(session.QueueUart(1, {bytes, 8}, 1000, 1000));
    Feedback feedback;
    Check(session.Publish({tx.data(), logical.tx.bytes}, 1100, feedback,
                          true) == Error::None);
    auto port = logical.uart_tx[1];
    auto first = U32(tx.data() + port);
    Check(first != 0);
    Check(session.Publish({tx.data(), logical.tx.bytes}, 1200, feedback,
                          true) == Error::None);
    Check(U32(tx.data() + port) ==
          first);  // Lost page cannot skip a UART record.
    Put32(rx.data() + 8, 2);
    auto ack = logical.uart_rx[1];
    Put32(rx.data() + ack + 4, first);
    rx[ack + 14] = 2;
    Check(session.Process({rx.data(), logical.rx.bytes}, 1300, true, actions) ==
          Error::None);
    Check(session.Publish({tx.data(), logical.tx.bytes}, 1400, feedback,
                          true) == Error::None);
    Check(U32(tx.data() + port) == first + 1);
    std::cout << "PASS: typed pages, isolation/replay/epoch, exact "
                 "mapping/signature, UART oldest-until-ACK\n";
}
