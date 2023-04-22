#include <accel-config/libaccel_config.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/idxd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>
#include <x86intrin.h>

#define WQ_PORTAL_SIZE 4096
#define ENQ_RETRY_MAX 1000

static void *get_wq_portal(void) {
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
            if (accfg_wq_get_user_dev_path(wq, path, sizeof(path))) continue;
            /* Use accfg_wq_(*) functions select WQ of type
             * ACCFG_WQT_USER and desired mode
             */
            wq_found = accfg_wq_get_type(wq) == ACCFG_WQT_USER &&
                       accfg_wq_get_mode(wq) == ACCFG_WQ_SHARED;
            if (wq_found) break;
        }
        if (wq_found) break;
    }
    accfg_unref(ctx);
    if (!wq_found) return MAP_FAILED;
    fd = open(path, O_RDWR);
    if (fd < 0) return MAP_FAILED;
    wq_portal = mmap(NULL, WQ_PORTAL_SIZE, PROT_WRITE,
                     MAP_SHARED | MAP_POPULATE, fd, 0);
    close(fd);
    return wq_portal;
}

static void put_wq_portal(void *p) { munmap(p, WQ_PORTAL_SIZE); }

static inline unsigned int enqcmd(void *dst, const void *src) {
    uint8_t retry;
    asm volatile(
        ".byte 0xf2, 0x0f, 0x38, 0xf8, 0x02\t\n"
        "setz %0\t\n"
        : "=r"(retry)
        : "a"(dst), "d"(src));
    return (unsigned int)retry;
}

int enqueue_descriptor(void *work_queue_portal, struct dsa_hw_desc *descriptor,
                       struct dsa_completion_record *completion_record) {
    _mm_sfence();
    completion_record->status = 0;
    unsigned int enqueue_retry_count = 0;
    while (enqcmd(work_queue_portal, descriptor) &&
           enqueue_retry_count++ < ENQ_RETRY_MAX)
        ;
    if (enqueue_retry_count == ENQ_RETRY_MAX) {
        printf("ENQCMD retry limit exceeded\n");
        return -1;
    }
    return 0;
}

void wait_result(struct dsa_completion_record *completion_record) {
    while (completion_record->status == 0) _mm_pause();
}

static uint8_t op_status(uint8_t status) {
    return status & DSA_COMP_STATUS_MASK;
}

#define HUGEPAGE_SIZE_1GB 1073741824
#define ALLOCATED_SIZE HUGEPAGE_SIZE_1GB

void check_result(unsigned char *data, unsigned char content) {
    for (unsigned int i = 0; i < ALLOCATED_SIZE; i++) {
        if (data[i] != content) {
            printf("content not match above offset %d\n", i);
            break;
        }
    }
    printf("content match\n");
}

int main(void) {
    unsigned char *p = NULL;
    int rc = 0;

    /*
     * Declare a space with 1GB huge page
     */
    p = mmap(NULL, ALLOCATED_SIZE, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    if (p == MAP_FAILED) {
        printf("allocation failed: %d\n", errno);
        rc = -1;
        goto mmap_failed;
    }

    void *work_queue_portal = get_wq_portal();
    struct dsa_completion_record completion_record __attribute__((aligned(32)));
    struct dsa_hw_desc default_descriptor = {
        .opcode = DSA_OPCODE_MEMFILL,
        .flags = IDXD_OP_FLAG_RCR | IDXD_OP_FLAG_CRAV,
        .xfer_size = ALLOCATED_SIZE,
        .pattern = 0xffffffffffffffff,
        .dst_addr = (uintptr_t)p,
        .completion_addr = (uintptr_t)&completion_record};

    p[0] = 0x00;

    printf("begin DSA operation\n");
    enqueue_descriptor(work_queue_portal, &default_descriptor,
                       &completion_record);
    wait_result(&completion_record);

    if (completion_record.status == DSA_COMP_SUCCESS) {
        printf("DSA operation successfull\n");
        check_result(p, 0xff);
    } else {
        if (op_status(completion_record.status) == DSA_COMP_PAGE_FAULT_NOBOF) {
            printf("DSA operation partially complete: %d\n",
                   completion_record.bytes_completed);
        } else {
            printf("desc failed status %u\n", completion_record.status);
        }
    }

clean_up:
    munmap(p, HUGEPAGE_SIZE_1GB);
mmap_failed:
    return 0;
}
