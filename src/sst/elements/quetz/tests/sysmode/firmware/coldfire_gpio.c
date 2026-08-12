/*
 * coldfire_gpio.c -- exercises the Raptor mcf-gpio device the way the BSP
 * GPIO driver does, and reports an error count (the GPIO_test() oracle:
 * errors=0 on success).
 *
 * This reproduces raptor-bsp GPIO_driver.c write semantics with raw MMIO so it
 * validates the device without the BSP source:
 *   - registers are 16-bit; byte offsets are 2x the driver's short-offsets
 *     (value +0x10, direction +0x18);
 *   - direction writes are read-modify-write, plain 8-bit result (no mask in
 *     the high byte); 1 = input, 0 = output;
 *   - value writes are mask-in-high-byte: (mask << 8) | data as one 16-bit
 *     store; unmasked pins retain their prior value;
 *   - setters re-read and compare (readback & mask) == (written & mask).
 *
 * Mirrors GPIO_test(false): for each bank, for each pin mask, drive direction
 * OUTPUT then INPUT (verify readback), then as an output drive HIGH then LOW
 * (verify readback), restoring the original direction. Skips bank0 pin0 (the
 * core-1 reset strap), exactly like GPIO_test.
 */

#include <stdint.h>
#include "coldfire_uart.h"

#define GPIO_BASE0      0xFC084000UL
#define GPIO_BANK_SPAN  0x4000UL
#define GPIO_VALUE_OFF      0x10
#define GPIO_DIRECTION_OFF  0x18

#define GPIO_OUTPUT 0x00
#define GPIO_INPUT  0x01

static inline volatile uint16_t *gpio_reg(uint8_t bank, uint32_t byte_off)
{
    return (volatile uint16_t *)(GPIO_BASE0 + (uint32_t)bank * GPIO_BANK_SPAN
                                 + byte_off);
}

static uint8_t gpio_get_direction(uint8_t bank)
{
    return (uint8_t)(*gpio_reg(bank, GPIO_DIRECTION_OFF));
}

/* Read-modify-write, plain 8-bit store (matches GPIO_set_direction). Returns
 * 1 if the masked readback matches. */
static uint8_t gpio_set_direction(uint8_t bank, uint8_t mask, uint8_t dir_vec)
{
    volatile uint16_t *p = gpio_reg(bank, GPIO_DIRECTION_OFF);
    uint8_t read_back = (uint8_t)(*p);
    uint8_t inv = mask ^ 0xFF;
    *p = (uint16_t)((read_back & inv) | (dir_vec & mask));
    read_back = (uint8_t)(*p);
    return (read_back & mask) == (dir_vec & mask);
}

static uint8_t gpio_set_direction_bit(uint8_t bank, uint8_t mask, uint8_t dir)
{
    return gpio_set_direction(bank, mask, (uint8_t)(mask * dir));
}

static uint8_t gpio_get_direction_bit(uint8_t bank, uint8_t mask)
{
    return (gpio_get_direction(bank) & mask) == mask;
}

/* mask-in-high-byte value store (matches GPIO_set_value). */
static uint8_t gpio_set_value(uint8_t bank, uint8_t mask, uint8_t out)
{
    volatile uint16_t *p = gpio_reg(bank, GPIO_VALUE_OFF);
    *p = (uint16_t)(((uint16_t)mask << 8) + out);
    uint8_t read_back = (uint8_t)(*p & 0xFF);
    return (read_back & mask) == (out & mask);
}

static uint8_t gpio_set_value_bit(uint8_t bank, uint8_t mask, uint8_t out)
{
    return gpio_set_value(bank, mask, (uint8_t)(mask * out));
}

static uint8_t gpio_get_value_bit(uint8_t bank, uint8_t mask)
{
    return (uint8_t)((*gpio_reg(bank, GPIO_VALUE_OFF)) & mask);
}

static uint32_t g_errors;

static void expect(int cond, const char *msg)
{
    if (!cond) {
        g_errors++;
        uart_puts("  FAIL: ");
        uart_puts(msg);
        uart_putc('\n');
    }
}

static void test_direction(uint8_t bank, uint8_t mask)
{
    uint8_t orig = gpio_get_direction_bit(bank, mask);

    gpio_set_direction_bit(bank, mask, GPIO_OUTPUT);
    expect(gpio_get_direction_bit(bank, mask) == GPIO_OUTPUT,
           "set direction OUTPUT");

    gpio_set_direction_bit(bank, mask, GPIO_INPUT);
    expect(gpio_get_direction_bit(bank, mask) == GPIO_INPUT,
           "set direction INPUT");

    expect(gpio_set_direction_bit(bank, mask, orig) == 1,
           "restore direction");
}

static void test_output(uint8_t bank, uint8_t mask)
{
    uint8_t orig = gpio_get_direction_bit(bank, mask);

    if (gpio_set_direction_bit(bank, mask, GPIO_OUTPUT) != 1) {
        expect(0, "set direction OUTPUT (for output test)");
        return;
    }

    gpio_set_value_bit(bank, mask, 1 /* HIGH */);
    expect(gpio_get_value_bit(bank, mask) == mask, "set value HIGH");

    gpio_set_value_bit(bank, mask, 0 /* LOW */);
    expect(gpio_get_value_bit(bank, mask) == 0x00, "set value LOW");

    expect(gpio_set_direction_bit(bank, mask, orig) == 1,
           "restore direction (after output)");
}

void kernel_main(void)
{
    uart_init();
    uart_puts("GPIO test: Raptor mcf-gpio direction/value readback\n");

    g_errors = 0;
    for (uint8_t bank = 0; bank <= 3; bank++) {
        uint8_t bit = 0x80;
        for (uint8_t i = 0; i < 8; i++) {
            uint8_t mask = (uint8_t)(bit >> i);
            /* Skip bank0 pin0 (core-1 reset strap), like GPIO_test(). */
            if (bank == 0 && i == 0) {
                continue;
            }
            test_direction(bank, mask);
            test_output(bank, mask);
        }
    }

    uart_puts("GPIO test: errors=");
    uart_put_u32_dec(g_errors);
    uart_putc('\n');

    testdev_done(g_errors == 0 ? TESTDEV_PASS : TESTDEV_FAIL);
}
