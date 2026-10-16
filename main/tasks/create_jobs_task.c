#include "work_queue.h"
#include "global_state.h"
#include "esp_log.h"
#include "esp_system.h"
#include "mining.h"
#include <limits.h>
#include "string.h"

#include <sys/time.h>

static const char *TAG = "create_jobs_task";

#define QUEUE_LOW_WATER_MARK 10 // Adjust based on your requirements

static bool should_generate_more_work(GlobalState *GLOBAL_STATE);
static bool generate_work(GlobalState *GLOBAL_STATE, mining_notify *notification, uint64_t extranonce_counter);

void create_jobs_task(void *pvParameters)
{
    GlobalState *GLOBAL_STATE = (GlobalState *)pvParameters;
    // One monotonically increasing 64-bit counter supplies the final 8 bytes.
    uint64_t extranonce_counter = 0;
    bool extranonce_counter_exhausted = false;

    while (1)
    {
        mining_notify *mining_notification = (mining_notify *)queue_dequeue(&GLOBAL_STATE->stratum_queue);
        if (mining_notification == NULL) {
            ESP_LOGE(TAG, "Failed to dequeue mining notification");
            vTaskDelay(100 / portTICK_PERIOD_MS); // Wait a bit before trying again
            continue;
        }

        ESP_LOGI(TAG, "New Work Dequeued %s", mining_notification->job_id);

        if (GLOBAL_STATE->new_stratum_version_rolling_msg) {
            ESP_LOGI(TAG, "Set chip version rolls %i", (int)(GLOBAL_STATE->version_mask >> 13));
            (GLOBAL_STATE->ASIC_functions.set_version_mask)(GLOBAL_STATE->version_mask);
            GLOBAL_STATE->new_stratum_version_rolling_msg = false;
        }

        while (GLOBAL_STATE->stratum_queue.count < 1 && GLOBAL_STATE->abandon_work == 0)
        {
            if (extranonce_counter_exhausted) {
                ESP_LOGE(TAG, "64-bit ExtraNonce counter exhausted; refusing to reuse an earlier value");
                vTaskDelay(1000 / portTICK_PERIOD_MS);
                continue;
            }
            if (should_generate_more_work(GLOBAL_STATE)) {
                if (generate_work(GLOBAL_STATE, mining_notification, extranonce_counter)) {
                    if (extranonce_counter == UINT64_MAX) extranonce_counter_exhausted = true;
                    else extranonce_counter++;
                }
            } else {
                vTaskDelay(100 / portTICK_PERIOD_MS);
            }
        }

        if (GLOBAL_STATE->abandon_work == 1)
        {
            GLOBAL_STATE->abandon_work = 0;
            ASIC_jobs_queue_clear(&GLOBAL_STATE->ASIC_jobs_queue);
            xSemaphoreGive(GLOBAL_STATE->ASIC_TASK_MODULE.semaphore);
        }

        STRATUM_V1_free_mining_notify(mining_notification);
    }
}

static bool should_generate_more_work(GlobalState *GLOBAL_STATE)
{
    return GLOBAL_STATE->ASIC_jobs_queue.count < QUEUE_LOW_WATER_MARK;
}

static bool generate_work(GlobalState *GLOBAL_STATE, mining_notify *notification, uint64_t extranonce_counter)
{
    char *extranonce = extranonce_generate(GLOBAL_STATE->extranonce, extranonce_counter);
    if (extranonce == NULL) {
        ESP_LOGE(TAG, "Failed to generate the single 12-byte ExtraNonce");
        return false;
    }
    ESP_LOGI(TAG, "ExtraNonce counter=%llu value=%s pool_job=%s",
             (unsigned long long)extranonce_counter, extranonce, notification->job_id);
    char *coinbase_tx = construct_coinbase_tx(notification->coinbase_1, notification->coinbase_2, extranonce);
    if (coinbase_tx == NULL) {
        ESP_LOGE(TAG, "Failed to construct coinbase_tx");
        free(extranonce);
        return false;
    }
    char *merkle_root = calculate_merkle_root_hash(coinbase_tx, (uint8_t(*)[32])notification->merkle_branches, notification->n_merkle_branches);
    if (merkle_root == NULL) {
        ESP_LOGE(TAG, "Failed to calculate merkle_root");
        free(extranonce);
        free(coinbase_tx);
        return false;
    }
    bm_job next_job = construct_bm_job(notification, merkle_root, GLOBAL_STATE->version_mask);
    bm_job *queued_next_job = malloc(sizeof(bm_job));
    if (queued_next_job == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for queued_next_job");
        free(extranonce);
        free(coinbase_tx);
        free(merkle_root);
        return false;
    }
    memcpy(queued_next_job, &next_job, sizeof(bm_job));
    queued_next_job->extranonce = extranonce;
    queued_next_job->jobid = strdup(notification->job_id);
    queued_next_job->version_mask = GLOBAL_STATE->version_mask;
    if (queued_next_job->jobid == NULL) {
        ESP_LOGE(TAG, "Failed to allocate job ID");
        free_bm_job(queued_next_job);
        free(coinbase_tx);
        free(merkle_root);
        return false;
    }
    queue_enqueue(&GLOBAL_STATE->ASIC_jobs_queue, queued_next_job);
    free(coinbase_tx);
    free(merkle_root);
    return true;
}