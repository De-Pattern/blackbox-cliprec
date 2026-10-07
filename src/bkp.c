/*
 * Backup SRAM access (STM32H7): cliprec keeps its take state in the 4 KB backup SRAM at 0x38800000. Its clock and
 * the backup-domain write protection are off after reset; turn them on before the first access.
 */
#include <stdint.h>

#define RCC_AHB4ENR (*(volatile uint32_t *)0x580244e0u)
#define BKPRAMEN    (1u << 28)
#define PWR_CR1     (*(volatile uint32_t *)0x58024800u)
#define DBP         (1u << 8)

void bkp_enable(void)
{
    if (!(RCC_AHB4ENR & BKPRAMEN)) {
        RCC_AHB4ENR |= BKPRAMEN;
        (void)RCC_AHB4ENR;                                   /* let the clock settle before the first access */
    }
    if (!(PWR_CR1 & DBP))
        PWR_CR1 |= DBP;                                      /* backup domain writes */
}
