/* SPDX-License-Identifier: Apache-2.0 */
/* 保持 SSC 的应用接口文件名；实际应用完全由仓库自有代码提供。 */
#include "ecat_def.h"
#ifndef DUST_SSC_APPLICATION_H
#define DUST_SSC_APPLICATION_H
void APPL_Application(void);
void APPL_AckErrorInd(UINT16 state);
UINT16 APPL_StartMailboxHandler(void);
UINT16 APPL_StopMailboxHandler(void);
UINT16 APPL_StartInputHandler(UINT16 *mask);
UINT16 APPL_StopInputHandler(void);
UINT16 APPL_StartOutputHandler(void);
UINT16 APPL_StopOutputHandler(void);
UINT16 APPL_GenerateMapping(UINT16 *input, UINT16 *output);
void APPL_InputMapping(UINT16 *data);
void APPL_OutputMapping(UINT16 *data);
#endif
#ifdef _OBJD_
/* COE 初始化后、启用 IRQ 前，gateway_init 注册动态对象。 */
TOBJECT ApplicationObjDic[] = {
    {NULL, NULL, 0xffff, {0, 0}, NULL, NULL, NULL, NULL, NULL, 0}};
#endif
