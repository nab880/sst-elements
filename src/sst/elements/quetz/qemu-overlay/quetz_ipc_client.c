/* Standalone Quetz IPC client for patched QEMU (C, no SST dependency). */
#include "quetz/quetz_ipc_client.h"
#include "quetz/quetz_ipc_types.h"

#include <fcntl.h>
#include <linux/futex.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

struct QuetzInternalSharedData {
    uint32_t expectedChildren;
    size_t   shmSegSize;
    size_t   numBuffers;
    size_t   offsets[1];
};

struct QuetzIpcClient {
    int               fd;
    void             *map;
    size_t            map_size;
    QuetzSharedData  *shared;
    /* Per-slot seq shadow for quetz_ipc_irq_drain (slots start at seq 0). */
    uint32_t          irq_seq[QUETZ_MAX_MMIO_VCORES][QUETZ_MAX_IRQ_LINES];
    /* Shadow of shared irq_generation as of the last COMPLETE drain scan;
     * an unchanged generation lets the drain skip the whole slot matrix. */
    uint32_t          irq_gen;
};

static int map_shmem(const char *shmname, QuetzIpcClient *c)
{
    c->fd = shm_open(shmname, O_RDWR, 0600);
    if (c->fd < 0)
        return -1;

    /* The real segment size comes from the fd, not from header fields we are
     * about to distrust. */
    struct stat st;
    if (fstat(c->fd, &st) != 0 || st.st_size <= 0) {
        close(c->fd);
        c->fd = -1;
        return -1;
    }
    size_t seg_size = (size_t)st.st_size;

    void *hdr = mmap(NULL, sizeof(struct QuetzInternalSharedData),
                     PROT_READ, MAP_SHARED, c->fd, 0);
    if (hdr == MAP_FAILED) {
        close(c->fd);
        c->fd = -1;
        return -1;
    }

    size_t map_size = ((struct QuetzInternalSharedData *)hdr)->shmSegSize;
    size_t shared_off = ((struct QuetzInternalSharedData *)hdr)->offsets[0];
    munmap(hdr, sizeof(struct QuetzInternalSharedData));

    /* QuetzInternalSharedData hand-mirrors SST-core's private tunnel header;
     * if that layout drifted, these values are garbage. Validate every
     * derived quantity against the fd-reported size BEFORE mapping/using it,
     * and then check the layout magic the SST master stamped at the END of
     * QuetzSharedData — a wrong shared_off or a QuetzSharedData layout skew
     * both land off-magic and fail here, loudly, instead of corrupting
     * guest-visible MMIO values. */
    if (map_size < sizeof(struct QuetzInternalSharedData) ||
        map_size > seg_size ||
        shared_off > map_size ||
        sizeof(QuetzSharedData) > map_size - shared_off) {
        fprintf(stderr,
                "quetz-ipc: shmem '%s' header rejected (map_size=%zu "
                "seg_size=%zu shared_off=%zu need=%zu) — SST-core tunnel "
                "layout skew?\n",
                shmname, map_size, seg_size, shared_off,
                sizeof(QuetzSharedData));
        close(c->fd);
        c->fd = -1;
        return -1;
    }

    c->map = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, c->fd, 0);
    if (c->map == MAP_FAILED) {
        close(c->fd);
        c->fd = -1;
        return -1;
    }
    c->map_size = map_size;
    c->shared = (QuetzSharedData *)((uint8_t *)c->map + shared_off);

    if (c->shared->magic != QUETZ_SHM_MAGIC) {
        fprintf(stderr,
                "quetz-ipc: shmem '%s' layout magic mismatch (got 0x%08x, "
                "want 0x%08x) — rebuild the QEMU overlay against the same "
                "quetz_ipc_types.h as the SST side.\n",
                shmname, c->shared->magic, QUETZ_SHM_MAGIC);
        munmap(c->map, c->map_size);
        c->map = NULL;
        close(c->fd);
        c->fd = -1;
        return -1;
    }
    return 0;
}

QuetzIpcClient *quetz_ipc_attach(const char *shmname)
{
    QuetzIpcClient *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->fd = -1;
    if (map_shmem(shmname, c) != 0) {
        free(c);
        return NULL;
    }
    return c;
}

void quetz_ipc_detach(QuetzIpcClient *client)
{
    if (!client)
        return;
    if (client->map)
        munmap(client->map, client->map_size);
    if (client->fd >= 0)
        close(client->fd);
    free(client);
}

static void clear_slot(QuetzSharedData *sd, unsigned vcpu)
{
    sd->mmio_slot[vcpu].ready = 0;
    sd->mmio_slot[vcpu].value = 0;
}

static void wait_slot(QuetzSharedData *sd, unsigned vcpu, uint64_t *out)
{
    /* Block (rather than busy-spin) until SST posts the response, so an offload
     * that stalls the vCPU doesn't burn a host core. FUTEX_WAIT returns at once
     * if ready != 0; the 1ms timeout is a safety re-poll so a missed wake
     * self-heals instead of hanging. The futex word lives in cross-process shmem. */
    uint32_t *ready = (uint32_t *)&sd->mmio_slot[vcpu].ready;
    /* Acquire-load pairs with the producer's release-store of `ready` (SST
     * postResponse), so observing ready!=0 happens-before reading `value`.
     * A plain load lets the value read float above the flag on weak-memory
     * hosts (e.g. ARM), yielding a stale MMIO result; x86-TSO hides it. */
    while (__atomic_load_n(ready, __ATOMIC_ACQUIRE) == 0) {
        struct timespec ts = { 0, 1000000 };
        syscall(SYS_futex, ready, FUTEX_WAIT, 0, &ts, NULL, 0);
    }
    *out = sd->mmio_slot[vcpu].value;
    /* Release-store the clear so the producer only reuses the slot after our
     * value read above has completed. */
    __atomic_store_n(ready, 0u, __ATOMIC_RELEASE);
}

uint64_t quetz_ipc_mmio_read(QuetzIpcClient *client, unsigned vcpu,
                             uint64_t addr, unsigned size)
{
    QuetzSharedData *sd = client->shared;
    if (!sd || vcpu >= QUETZ_MAX_MMIO_VCORES)
        return 0;

    clear_slot(sd, vcpu);
    QuetzMmioSyncRequest *req = &sd->mmio_req[vcpu];
    req->addr = addr;
    req->size = size;
    req->write_val = 0;
    req->cmd = QUETZ_CMD_MMIO_READ_REQ;
    __sync_synchronize();
    req->pending = 1;

    uint64_t value = 0;
    wait_slot(sd, vcpu, &value);
    return value;
}

void quetz_ipc_mmio_write(QuetzIpcClient *client, unsigned vcpu,
                          uint64_t addr, unsigned size, uint64_t value)
{
    QuetzSharedData *sd = client->shared;
    if (!sd || vcpu >= QUETZ_MAX_MMIO_VCORES)
        return;

    clear_slot(sd, vcpu);
    QuetzMmioSyncRequest *req = &sd->mmio_req[vcpu];
    req->addr = addr;
    req->size = size;
    req->write_val = value;
    req->cmd = QUETZ_CMD_MMIO_WRITE_REQ;
    __sync_synchronize();
    req->pending = 1;

    uint64_t ack = 0;
    wait_slot(sd, vcpu, &ack);
}

unsigned quetz_ipc_irq_drain(QuetzIpcClient *client, unsigned max_lines,
                             QuetzIrqChange *out, unsigned max_out)
{
    QuetzSharedData *sd = client->shared;
    unsigned n = 0;
    size_t ncores;
    size_t v;
    unsigned l;

    if (!sd || !out || max_out == 0)
        return 0;
    if (max_lines > QUETZ_MAX_IRQ_LINES)
        max_lines = QUETZ_MAX_IRQ_LINES;

    /* One acquire-load instead of scanning the whole (vcore, line) matrix on
     * every poll tick. Acquire pairs with SST's release-store of
     * irq_generation, which happens after the slot it describes — so a moved
     * generation guarantees the changed slot's seq/level are visible below.
     * The shadow only advances after a COMPLETE scan; an early return (out
     * full) leaves it stale so the caller's next drain rescans. */
    uint32_t gen = __atomic_load_n(&sd->irq_generation, __ATOMIC_ACQUIRE);
    if (gen == client->irq_gen)
        return 0;

    ncores = sd->numCores;
    if (ncores > QUETZ_MAX_MMIO_VCORES)
        ncores = QUETZ_MAX_MMIO_VCORES;

    for (v = 0; v < ncores; v++) {
        for (l = 0; l < max_lines; l++) {
            QuetzIrqSlot *slot = &sd->irq_slot[v][l];
            /* Acquire pairs with SST's release-store of seq, so the level it
             * published beforehand is visible; a level newer than the seq we
             * saw is harmless (seq will have moved again by the next drain
             * and we re-apply the same level — see quetz_ipc_types.h). */
            uint32_t seq = __atomic_load_n(&slot->seq, __ATOMIC_ACQUIRE);
            if (seq == client->irq_seq[v][l])
                continue;
            client->irq_seq[v][l] = seq;
            out[n].vcore = (uint32_t)v;
            out[n].line = l;
            out[n].level = slot->level;
            if (++n == max_out)
                return n;   /* scan incomplete: irq_gen stays stale */
        }
    }
    client->irq_gen = gen;
    return n;
}
