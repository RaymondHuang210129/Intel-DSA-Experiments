#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <linux/idxd.h>
#include <accel-config/libaccel_config.h>
#include <x86intrin.h>

#define BLEN            2097152
#define WQ_PORTAL_SIZE  4096

static inline  unsigned int
enqcmd(void *dst, const void *src)
{
    uint8_t retry;
    asm volatile(".byte 0xf2, 0x0f, 0x38, 0xf8, 0x02\t\n"
                 "setz %0\t\n"
                 : "=r"(retry) : "a" (dst), "d" (src));
    return (unsigned int)retry;
}

static uint8_t
op_status(uint8_t status)
{
    return status & DSA_COMP_STATUS_MASK;
}

static void *
map_wq(void)
{
    void *wq_portal;
    struct accfg_ctx *ctx;
    struct accfg_wq *wq;
    struct accfg_device *device;
    char path[PATH_MAX];
    int fd;
    int wq_found;
    accfg_new(&ctx);
    accfg_device_foreach(ctx, device) {
        /* Use accfg_device_(*) functions to select enabled device
         * based on name, numa node
         */
        accfg_wq_foreach(device, wq) {
            if (accfg_wq_get_user_dev_path(wq, path, sizeof(path)))
                continue;
            /* Use accfg_wq_(*) functions select WQ of type
             * ACCFG_WQT_USER and desired mode
             */
            wq_found = accfg_wq_get_type(wq) == ACCFG_WQT_USER &&
                        accfg_wq_get_mode(wq) == ACCFG_WQ_SHARED;
            if (wq_found)
                break;
}
        if (wq_found)
            break;
}
    accfg_unref(ctx);
    if (!wq_found)
        return MAP_FAILED;
    fd = open(path, O_RDWR);
    if (fd < 0)
        return MAP_FAILED;
    wq_portal = mmap(NULL, WQ_PORTAL_SIZE, PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, 0);
    close(fd);
    return wq_portal;
}

#define ENQ_RETRY_MAX   1000
#define POLL_RETRY_MAX  10000

int main(int argc, char *argv[])
{
    void *wq_portal;
    struct dsa_hw_desc desc = { };
    char src[BLEN];
    char dst[BLEN];
    struct dsa_completion_record comp __attribute__((aligned(32)));
    int rc;
    int poll_retry, enq_retry;
    wq_portal = map_wq();
    if (wq_portal == MAP_FAILED)
        return EXIT_FAILURE;
    memset(src, 0xaa, BLEN);
    desc.opcode = DSA_OPCODE_MEMMOVE;
    /*
     * Request a completion – since we poll on status, this flag
     * must be 1 for status to be updated on successful
     * completion
     */
    desc.flags = IDXD_OP_FLAG_RCR;
    /* CRAV should be 1 since RCR = 1 */
    desc.flags |= IDXD_OP_FLAG_CRAV;
    /* Hint to direct data writes to CPU cache */
    desc.flags |= IDXD_OP_FLAG_CC;
    desc.xfer_size = BLEN;
    desc.src_addr = (uintptr_t)src;
    desc.dst_addr = (uintptr_t)dst;
    desc.completion_addr = (uintptr_t)&comp;
retry:
    printf("%s", "put descriptor\n");
    comp.status = 0;
    /* Ensure previous writes are ordered with respect to ENQCMD */
    _mm_sfence();
    enq_retry = 0;
    while (enqcmd(wq_portal, &desc) && enq_retry++ < ENQ_RETRY_MAX) ;
    if (enq_retry == ENQ_RETRY_MAX) {
        printf("ENQCMD retry limit exceeded\n");
        rc = EXIT_FAILURE;
        goto done;
    }
    poll_retry = 0;
    while (comp.status == 0 && poll_retry++ < POLL_RETRY_MAX)
        _mm_pause();
    if (poll_retry == POLL_RETRY_MAX) {
        printf("Completion status poll retry limit exceeded\n");
        rc = EXIT_FAILURE;
        goto done;
    }
    if (comp.status != DSA_COMP_SUCCESS) {
        if (op_status(comp.status) == DSA_COMP_PAGE_FAULT_NOBOF) {
            int wr = comp.status & DSA_COMP_STATUS_WRITE;
            volatile char *t;
            t = (char *)comp.fault_addr;
            wr ? *t = *t : *t;
            desc.src_addr += comp.bytes_completed;
            desc.dst_addr += comp.bytes_completed;
            desc.xfer_size -= comp.bytes_completed;
            goto retry;
        } else {
            printf("desc failed status %u\n", comp.status);
            rc = EXIT_FAILURE;
        }
    } else {
        printf("desc successful\n");
        rc = memcmp(src, dst, BLEN);
        rc ? printf("memmove failed\n") : printf("memmove successful\n");
        rc = rc ? EXIT_FAILURE : EXIT_SUCCESS;
    }
done:
    munmap(wq_portal, WQ_PORTAL_SIZE);
    return rc;
}
