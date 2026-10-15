#ifndef COMMON_H_
#define COMMON_H_

#include <stdint.h>

typedef struct
{
    uint8_t job_id;
    uint32_t nonce;
    uint32_t rolled_version;

    /*
     * Optional immutable snapshot of the job associated with this result.
     * BM1370 populates this because its version bits must be combined with
     * the job metadata at result-receive time. The result consumer owns it.
     */
    void *job_snapshot;
} task_result;

unsigned char _reverse_bits(unsigned char num);
int _largest_power_of_two(int num);

#endif