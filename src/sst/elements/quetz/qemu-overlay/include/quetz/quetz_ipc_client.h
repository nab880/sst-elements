#ifndef QUETZ_IPC_CLIENT_H
#define QUETZ_IPC_CLIENT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct QuetzIpcClient QuetzIpcClient;

QuetzIpcClient *quetz_ipc_attach(const char *shmname);
void quetz_ipc_detach(QuetzIpcClient *client);
uint64_t quetz_ipc_mmio_read(QuetzIpcClient *client, unsigned vcpu,
                             uint64_t addr, unsigned size);
void quetz_ipc_mmio_write(QuetzIpcClient *client, unsigned vcpu,
                          uint64_t addr, unsigned size, uint64_t value);

/* One IRQ-line level change drained from the reverse mailbox. */
typedef struct QuetzIrqChange {
    uint32_t vcore;  /* mailbox row the change was posted to */
    uint32_t line;   /* machine IRQ line number */
    uint32_t level;  /* 1 = raise, 0 = lower */
} QuetzIrqChange;

/* Scan the per-(vcore, line) IRQ slots for seq changes since the last drain
 * and append the corresponding level changes to `out` (at most max_out).
 * `max_lines` bounds the polled line range (<= QUETZ_MAX_IRQ_LINES). The
 * client keeps the per-slot seq shadow, so each change is reported once.
 * Returns the number of entries written. */
unsigned quetz_ipc_irq_drain(QuetzIpcClient *client, unsigned max_lines,
                             QuetzIrqChange *out, unsigned max_out);

#ifdef __cplusplus
}
#endif

#endif
