/* SPDX-License-Identifier: Apache-2.0 */

/* APPL_* C 回调：只做转发，业务逻辑在 SscStack/domain。 */

#include "adapters/ethercat_ssc/ssc_context.hpp"
#include "adapters/ethercat_ssc/ssc_stack.hpp"

extern "C" {

void APPL_AckErrorInd(UINT16 state) {
    (void)state;
    if (ecat::g_ctx.stack) {
        ecat::g_ctx.stack->AckError();
    }
}

UINT16 APPL_StartMailboxHandler(void) {
    return ALSTATUSCODE_NOERROR;
}

UINT16 APPL_StopMailboxHandler(void) {
    return ALSTATUSCODE_NOERROR;
}

UINT16 APPL_GenerateMapping(UINT16* input, UINT16* output) {
    return ecat::g_ctx.stack
               ? ecat::g_ctx.stack->GenerateMapping(input, output)
               : ALSTATUSCODE_INVALIDOUTPUTMAPPING;
}

UINT16 APPL_StartInputHandler(UINT16* mask) {
    return ecat::g_ctx.stack ? ecat::g_ctx.stack->StartInput(mask)
                             : ALSTATUSCODE_INVALIDOUTPUTMAPPING;
}

UINT16 APPL_StopInputHandler(void) {
    return ecat::g_ctx.stack ? ecat::g_ctx.stack->StopInput()
                             : ALSTATUSCODE_NOERROR;
}

UINT16 APPL_StartOutputHandler(void) {
    return ecat::g_ctx.stack ? ecat::g_ctx.stack->StartOutput()
                             : ALSTATUSCODE_INVALIDOUTPUTMAPPING;
}

UINT16 APPL_StopOutputHandler(void) {
    return ecat::g_ctx.stack ? ecat::g_ctx.stack->StopOutput()
                             : ALSTATUSCODE_NOERROR;
}

void APPL_OutputMapping(UINT16* data) {
    if (ecat::g_ctx.stack) {
        ecat::g_ctx.stack->OutputMapping(data);
    }
}

void APPL_InputMapping(UINT16* data) {
    if (ecat::g_ctx.stack) {
        ecat::g_ctx.stack->InputMapping(data);
    }
}

void APPL_Application(void) {
    if (ecat::g_ctx.stack) {
        ecat::g_ctx.stack->Application();
    }
}

}  // extern "C"
