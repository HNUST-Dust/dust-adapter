/* SPDX-License-Identifier: Apache-2.0 */

/* SSC 适配器：把 SSC 的 APPL_* 回调翻译成 domain 调用。
 * 这是固件中唯一允许 include SSC 头与操作 ESC 过程影像的模块。 */
#pragma once

#include <cstdint>

#include "adapters/ethercat_ssc/io_coordinator.hpp"
#include "adapters/ethercat_ssc/layout_service.hpp"
#include "domain/protection/protection.hpp"
#include "domain/protocol/session.hpp"
#include "common/types.hpp"

namespace ecat {

class SscStack {
   public:
    SscStack(Session& session, LayoutService& layout, IoCoordinator& io,
             ProtectionService& protection,
             const SscBoardConfig& board);

    int Start();
    void Enable();
    void Stop();
    void Step();
    void Poll();
    StackEvents TakeEvents();
    bool TransportHealthy() const;
    AlState Al() const;
    bool Active() const;

    /* APPL_* 翻译入口（ssc_callbacks.cpp 转发）。 */
    uint8_t ReadObject(uint16_t index, uint8_t subindex, uint32_t size,
                       uint16_t* data, uint8_t complete);
    uint8_t WriteObject(uint16_t index, uint8_t subindex, uint32_t size,
                        uint16_t* data, uint8_t complete);
    uint16_t GenerateMapping(uint16_t* input, uint16_t* output);
    uint16_t StartInput(uint16_t* mask);
    uint16_t StopInput();
    uint16_t StartOutput();
    uint16_t StopOutput();
    void OutputMapping(uint16_t* data);
    void InputMapping(uint16_t* data);
    void Application();
    void AckError();

   private:
    void ResetRuntimeState();
    int ApplyLimits();
    void RefreshCanBeforeMapping();
    void PublishRegular(uint64_t now);
    void PublishCanOnly(uint64_t now);
    void HandleStatusRequest();

    Session& session_;
    LayoutService& layout_;
    IoCoordinator& io_;
    ProtectionService& protection_;
    const SscBoardConfig& board_;
    bool transport_healthy_ = false;
};

}  // namespace ecat
