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
#include <sys/wait.h>
#include <unistd.h>
#include <x86intrin.h>

#define WQ_PORTAL_SIZE 4096
#define ENQ_RETRY_MAX 1000

#define IS_SENDER(pid) pid != 0
#define IS_RECEIVER(pid) pid == 0
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

typedef unsigned long long ulltime_t;

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
    volatile uint8_t *status = &(completion_record->status);
    while (*status == 0) _mm_lfence();
}

static uint8_t op_status(uint8_t status) {
    return status & DSA_COMP_STATUS_MASK;
}

#define HUGEPAGE_SIZE_1GB 1073741824
#define ALLOCATED_SIZE HUGEPAGE_SIZE_1GB

#define SEND_BITS_COUNT 100

int main(void) {
    unsigned char *p = NULL;
    int should_terminate = 1;

    // determine granularity

    printf("measure the execution duration of each dsa operation\n");
    _mm_lfence();

    p = mmap(NULL, ALLOCATED_SIZE, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    p[0] = 0x00;
    void *work_queue_portal = get_wq_portal();
    struct dsa_completion_record completion_record __attribute__((aligned(32)));
    struct dsa_hw_desc default_descriptor = {
        .opcode = DSA_OPCODE_MEMFILL,
        .flags = IDXD_OP_FLAG_RCR | IDXD_OP_FLAG_CRAV,  // | IDXD_OP_FLAG_CC,
        .xfer_size = ALLOCATED_SIZE,
        .pattern = 0xffffffffffffffff,
        .dst_addr = (uintptr_t)p,
        .completion_addr = (uintptr_t)&completion_record};

    ulltime_t time1, time2, execution_duration = ULLONG_MAX;

    for (int i = 0; i < 50; i++) {
        _mm_lfence();
        enqueue_descriptor(work_queue_portal, &default_descriptor,
                           &completion_record);
        _mm_lfence();
        time1 = __rdtsc();
        wait_result(&completion_record);
        _mm_lfence();
        time2 = __rdtsc();
        execution_duration = execution_duration > time2 - time1
                                 ? time2 - time1
                                 : execution_duration;
        _mm_lfence();
    }

    printf("the execution period is %llu\n", execution_duration);
    munmap(p, ALLOCATED_SIZE);

    ulltime_t memory_access_interval = execution_duration / 16ULL;
    printf("set memory access interval: %llu\n", memory_access_interval);

    sleep(1);

    // fork parent and child process

    pid_t pid = fork();
    if (IS_SENDER(pid)) {
        p = mmap(NULL, ALLOCATED_SIZE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
        p[0] = 0x00;

        ulltime_t time_slots[SEND_BITS_COUNT];
        for (int i = 0; i < SEND_BITS_COUNT; i++) {
            time_slots[i] = i * execution_duration;
        }

        ulltime_t start_time = __rdtsc();

        // send bit 0, 1, 0, 1 ...

        for (int target_slot = 0; target_slot < 100;) {
            _mm_lfence();
            ulltime_t current_time = __rdtsc();
            _mm_lfence();
            if (likely(current_time < start_time + time_slots[target_slot])) {
                // wait for next time slot to send a bit
                continue;
            } else if (current_time > start_time + time_slots[target_slot] &&
                       current_time <=
                           start_time + time_slots[target_slot + 1]) {
                // the correct time frame to send a bit
                if (target_slot & 0x1) {
                    enqueue_descriptor(work_queue_portal, &default_descriptor,
                                       &completion_record);
                    wait_result(&completion_record);
                    if (unlikely(completion_record.status != DSA_COMP_SUCCESS))
                        printf("dsa operation failed\n");
                }
                target_slot++;
            } else {
                // a time slot has expired but the bit is not sent. ignore this
                // bit
                target_slot++;
                continue;
            }
        }
        munmap(p, ALLOCATED_SIZE);
        wait(NULL);

    } else if (IS_RECEIVER(pid)) {
        ulltime_t time_slots[SEND_BITS_COUNT * 16ULL];

        return 0;
    }

    return 0;
}
