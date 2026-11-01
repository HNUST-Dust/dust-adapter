/* SPDX-License-Identifier: Apache-2.0 */

#ifndef DUST_ECAT_ESC_HW_H_
#define DUST_ECAT_ESC_HW_H_

#include <stddef.h>
#include <stdint.h>
#include <zephyr/drivers/ethercat/hpm_esc.h>
#include <zephyr/sys/printk.h>

#ifdef __cplusplus
extern "C" {
#endif

uint16_t esc_hw_init(void);
void esc_hw_release(void);
void esc_read(volatile void *data, uint16_t address, uint16_t length);
void esc_write(const volatile void *data, uint16_t address,
                         uint16_t length);
uint16_t esc_al_event(void);
uint32_t esc_timer_get(void);
void esc_timer_clear(void);
uint16_t esc_eeprom_reload(void);
int esc_process_eeprom(void);

/* ESC/PHY/SII 启动序列与中断钩子（由应用主循环注册）。 */
int esc_init(void);
void esc_start(void);
void esc_stop(void);
void esc_update_link(void);
void esc_set_pdi_hook(void (*hook)(void));
void esc_set_reset_hook(void (*hook)(void));
void esc_irq_enable(void);
void esc_irq_disable(void);
void esc_enter_critical(void);
void esc_leave_critical(void);

#define ECAT_TIMER_INC_P_MS 1U
#define HW_Init() esc_hw_init()
#define HW_Release() esc_hw_release()
#define HW_GetTimer() esc_timer_get()
#define HW_ClearTimer() esc_timer_clear()
#define HW_GetALEventRegister() esc_al_event()
#define HW_GetALEventRegister_Isr() esc_al_event()

#define HW_EscRead(pData, Address, Len) \
    esc_read((pData), (uint16_t)(Address), (uint16_t)(Len))
#define HW_EscReadIsr(pData, Address, Len) HW_EscRead((pData), (Address), (Len))
#define HW_EscReadMbxMem(pData, Address, Len) \
    HW_EscRead((pData), (Address), (Len))
#define HW_EscWrite(pData, Address, Len) \
    esc_write((pData), (uint16_t)(Address), (uint16_t)(Len))
#define HW_EscWriteIsr(pData, Address, Len) \
    HW_EscWrite((pData), (Address), (Len))
#define HW_EscWriteMbxMem(pData, Address, Len) \
    HW_EscWrite((pData), (Address), (Len))

#define HW_EscReadByte(value, address) \
    esc_read(&(value), (address), 1U)
#define HW_EscReadByteIsr(value, address) HW_EscReadByte((value), (address))
#define HW_EscReadWord(value, address) \
    esc_read(&(value), (address), 2U)
#define HW_EscReadWordIsr(value, address) HW_EscReadWord((value), (address))
#define HW_EscReadDWord(value, address) \
    esc_read(&(value), (address), 4U)
#define HW_EscReadDWordIsr(value, address) HW_EscReadDWord((value), (address))
#define HW_EscWriteByte(value, address) \
    esc_write(&(value), (address), 1U)
#define HW_EscWriteByteIsr(value, address) HW_EscWriteByte((value), (address))
#define HW_EscWriteWord(value, address) \
    esc_write(&(value), (address), 2U)
#define HW_EscWriteWordIsr(value, address) HW_EscWriteWord((value), (address))
#define HW_EscWriteDWord(value, address) \
    esc_write(&(value), (address), 4U)
#define HW_EscWriteDWordIsr(value, address) HW_EscWriteDWord((value), (address))

#define HW_EepromReload() esc_eeprom_reload()
#define ENABLE_ESC_INT() esc_irq_enable()
#define DISABLE_ESC_INT() esc_irq_disable()

#ifdef __cplusplus
}
#endif

#endif /* DUST_ECAT_ESC_HW_H_ */
