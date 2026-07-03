/*
 * coldfire_system.c — full embedded-system demo on ColdFire (m68k, big-endian).
 *
 * The reference deck for the primary quetz use case: functionally validate
 * embedded code for a ColdFire board — UART console, GPS receiver, sensor
 * stream, and a generic accelerator — with no physical hardware and no
 * cycle-accuracy requirements ("does my code work?", not "how fast?").
 *
 * Peripherals and where they come from:
 *   UART0 console  QEMU's mcf5208 UART model. TX -> stdout (gold-compared),
 *                  RX <- the NMEA fixture via appstdin. TX and RX are
 *                  independent directions, so console output and GPS input
 *                  share UART0 without interfering.
 *   GPS            NMEA sentences replayed into UART0 RX. The firmware
 *                  validates each "$GPRMC...*HH" checksum and counts
 *                  active ('A') fixes — i.e. real driver-level parsing of
 *                  recorded device data.
 *   Sensors        QuetzStreamDevice at SENSOR_BASE (SST-side MMIO): pops the
 *                  recorded sample stream 4 bytes per DATA read, verifies the
 *                  sum32 trailer, then exercises CTRL rewind.
 *   Accelerator    QuetzGpuDevice at GPU_BASE (synthetic latency model):
 *                  two doorbell "batch process" kernels, completion polled
 *                  via STATUS/KERNEL_ID.
 *
 * PASS iff: GPS checksums and fix count match the fixture, the sensor stream
 * sum and rewind check out, and both accelerator kernels retire.
 *
 * SDL: sysmode/basic_quetz_coldfire_system.py.
 * Fixtures: tests/sysmode/data/gps_nmea.txt, tests/sysmode/data/sensor_stream.bin.
 */

#include <stdint.h>

#include "coldfire_uart.h"

/* Both devices live in one sst-mmio-bridge window (0x70000000..0x7001FFFF),
 * routed to the right SST component by address (memHierarchy.Bus in the SDL). */
#define GPU_BASE          0x70000000UL
#define GPU_DOORBELL      (GPU_BASE + 0x00UL)   /* W: submit */
#define GPU_STATUS        (GPU_BASE + 0x08UL)   /* R: busy(1)/idle(0) */
#define GPU_KERNEL_ID     (GPU_BASE + 0x10UL)   /* R: completed counter */
#define GPU_LATENCY_OVR   (GPU_BASE + 0x18UL)   /* W: next-kernel cycles */

#define SENSOR_BASE       0x70010000UL
#define SENSOR_STATUS     (SENSOR_BASE + 0x00UL) /* R: bytes remaining */
#define SENSOR_DATA       (SENSOR_BASE + 0x08UL) /* R: pop 4 bytes (packed) */
#define SENSOR_SEQ        (SENSOR_BASE + 0x10UL) /* R: bytes consumed */
#define SENSOR_CTRL       (SENSOR_BASE + 0x18UL) /* W: 1 = rewind */

static inline void mmio_write32(uint32_t addr, uint32_t v)
{
    *(volatile uint32_t *)addr = v;
}

static inline uint32_t mmio_read32(uint32_t addr)
{
    return *(volatile uint32_t *)addr;
}

/* --- GPS: NMEA over a UART RX ----------------------------------------------
 * Default: UART0 (console port; RX fed via appstdin — TX/RX are independent).
 * Build with -DGPS_UART=1 for a dedicated GPS port on UART1, fed by a paced
 * `-serial pipe:` chardev (tools/serial_feeder.py). g_gps_waits counts
 * RXRDY-empty polls — the paced test asserts the guest actually waited. */

#ifndef GPS_UART
#define GPS_UART 0
#endif

#define GPS_TOTAL_LINES  10u   /* fixture line count — read exactly this many */
#define NMEA_MAX         96u

static uint32_t g_gps_waits;

static char gps_getc(void)
{
#if GPS_UART == 1
    while (!(UART1_SR & SR_RXRDY))
        g_gps_waits++;
    return (char)UART1_RB;
#else
    while (!(UART_SR & SR_RXRDY))
        g_gps_waits++;
    return (char)UART_RB;
#endif
}

static uint32_t hexval(char c)
{
    if (c >= '0' && c <= '9') return (uint32_t)(c - '0');
    if (c >= 'A' && c <= 'F') return (uint32_t)(c - 'A' + 10);
    if (c >= 'a' && c <= 'f') return (uint32_t)(c - 'a' + 10);
    return 0xFFu;
}

/* Read one CR/LF-terminated line into buf; returns its length. */
static uint32_t gps_read_line(char *buf, uint32_t max)
{
    uint32_t n = 0;
    for (;;) {
        char c = gps_getc();
        if (c == '\r')
            continue;
        if (c == '\n')
            break;
        if (n + 1 < max)
            buf[n++] = c;
    }
    buf[n] = '\0';
    return n;
}

/* "$...*HH": XOR of the bytes between '$' and '*' must equal HH. */
static int nmea_checksum_ok(const char *s, uint32_t len)
{
    if (len < 4 || s[0] != '$')
        return 0;
    uint32_t sum = 0, i = 1;
    while (i < len && s[i] != '*')
        sum ^= (uint32_t)(uint8_t)s[i++];
    if (i + 2 >= len || s[i] != '*')
        return 0;
    uint32_t hi = hexval(s[i + 1]), lo = hexval(s[i + 2]);
    if (hi > 15 || lo > 15)
        return 0;
    return sum == (hi << 4 | lo);
}

/* $GPRMC field 2 is the status: 'A' = active fix, 'V' = void. */
static int gprmc_is_active(const char *s)
{
    uint32_t commas = 0, i = 0;
    while (s[i]) {
        if (s[i] == ',') {
            commas++;
            if (commas == 2)
                return s[i + 1] == 'A';
        }
        i++;
    }
    return 0;
}

/* --- Sensor stream: QuetzStreamDevice ------------------------------------- */

#define SENSOR_EOS        (SENSOR_BASE + 0x20UL) /* R: 1 = stream consumed */

/* The canonical stream-drain pattern (works paced and unpaced): poll STATUS
 * for readiness, distinguish "not ready yet" from "done" via REG_EOS, count
 * the waits. Sum all payload bytes; the last word of the stream is a sum32
 * trailer packed b0|b1<<8|b2<<16|b3<<24 — numeric packing on both ends, so
 * the same firmware is correct on big-endian ColdFire. */
static uint32_t g_sensor_waits;

static int sensor_check(uint32_t *first_word_out)
{
    uint32_t sum = 0, first = 0, prev = 0, prev_valid = 0;

    for (;;) {
        if (mmio_read32(SENSOR_STATUS) == 0) {
            if (mmio_read32(SENSOR_EOS))
                break;                     /* fully drained */
            g_sensor_waits++;              /* paced refill not here yet */
            continue;
        }
        uint32_t w = mmio_read32(SENSOR_DATA);
        if (prev_valid) {
            sum += (prev & 0xFFu) + ((prev >> 8) & 0xFFu)
                 + ((prev >> 16) & 0xFFu) + ((prev >> 24) & 0xFFu);
        } else {
            first = w;
        }
        prev = w;
        prev_valid = 1;
    }

    uint32_t total = mmio_read32(SENSOR_SEQ);
    *first_word_out = first;

    if (total < 8 || (total & 3u))
        return 0;
    if (mmio_read32(SENSOR_STATUS) != 0)   /* drained exactly */
        return 0;
    return prev_valid && sum == prev;      /* last word = trailer */
}

/* --- Accelerator: batch kernels on the synthetic GPU ---------------------- */

static void accel_run_batch(uint32_t cycles)
{
    mmio_write32(GPU_LATENCY_OVR, cycles);
    mmio_write32(GPU_DOORBELL, 0);
    while (mmio_read32(GPU_STATUS))
        ;
}

/* --------------------------------------------------------------------------- */

void kernel_main(void)
{
    uart_init();
#if GPS_UART == 1
    uart1_init();
#endif
    uart_puts("ColdFire system demo: uart + gps + sensors + accelerator\n");

    /* GPS: consume the whole NMEA fixture from UART0 RX. */
    char line[NMEA_MAX];
    uint32_t gps_valid = 0, gps_active = 0;
    for (uint32_t i = 0; i < GPS_TOTAL_LINES; i++) {
        uint32_t len = gps_read_line(line, NMEA_MAX);
        if (!nmea_checksum_ok(line, len))
            continue;
        gps_valid++;
        if (gprmc_is_active(line))
            gps_active++;
    }
    uart_puts("gps: valid=");
    uart_put_u32_dec(gps_valid);
    uart_puts(" active_fixes=");
    uart_put_u32_dec(gps_active);
    uart_puts(" waits=");
    uart_put_u32_dec(g_gps_waits);
    uart_putc('\n');

    /* Sensors: drain + verify the recorded stream, then prove rewind works
     * (paced: the replayed first word needs a refill, so poll STATUS). */
    uint32_t first_word = 0;
    int sensor_ok = sensor_check(&first_word);
    mmio_write32(SENSOR_CTRL, 1);          /* rewind */
    while (mmio_read32(SENSOR_STATUS) == 0)
        g_sensor_waits++;
    int rewind_ok = (mmio_read32(SENSOR_DATA) == first_word);
    uart_puts("sensors: stream=");
    uart_puts(sensor_ok ? "ok" : "BAD");
    uart_puts(" rewind=");
    uart_puts(rewind_ok ? "ok" : "BAD");
    uart_puts(" waits=");
    uart_put_u32_dec(g_sensor_waits);
    uart_putc('\n');

    /* Accelerator: two processing batches, confirm both retired. */
    accel_run_batch(1000);
    accel_run_batch(2000);
    uint32_t kernels = mmio_read32(GPU_KERNEL_ID);
    uart_puts("accel: kernels_completed=");
    uart_put_u32_dec(kernels);
    uart_putc('\n');

    int pass = (gps_valid == 8) && (gps_active == 6) &&
               sensor_ok && rewind_ok && (kernels == 2);
    uart_puts(pass ? "SYSTEM DEMO PASS\n" : "SYSTEM DEMO FAIL\n");
    testdev_done(pass ? TESTDEV_PASS : TESTDEV_FAIL);
}
