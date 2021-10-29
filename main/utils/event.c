#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "event.h"
#include "jade_assert.h"
#include "utils/malloc_ext.h"

ESP_EVENT_DEFINE_BASE(JADE_EVENT);

struct wait_event_data_t {
    SemaphoreHandle_t triggered;

    esp_event_base_t register_event_base;
    int32_t register_event_id;

    esp_event_base_t trigger_event_base;
    int32_t trigger_event_id;
    void* trigger_event_data;
};

// Make a new event-data
wait_event_data_t* make_wait_event_data(void)
{
    wait_event_data_t* const wait_event_data = JADE_CALLOC(1, sizeof(wait_event_data_t));

    wait_event_data->triggered = xSemaphoreCreateBinary();

    // Not yet registered
    wait_event_data->register_event_base = NULL;
    wait_event_data->register_event_id = 0;

    // Not yet triggered
    wait_event_data->trigger_event_base = NULL;
    wait_event_data->trigger_event_id = 0;
    wait_event_data->trigger_event_data = NULL;

    return wait_event_data;
}

// Unregister the event and free the event-data structure
void free_wait_event_data(wait_event_data_t* data)
{
    JADE_ASSERT(data);

    // Free the underlying event data struct
    JADE_LOGD("Freeing event data for %s/%u (%p)", data->register_event_base, data->register_event_id, data);
    vSemaphoreDelete(data->triggered);
    free(data);
}

// Handler called by the event loop if the event fires
void sync_wait_event_handler(void* handler_arg, esp_event_base_t base, int32_t id, void* event_data)
{
    JADE_ASSERT(handler_arg);
    wait_event_data_t* data = handler_arg;
    JADE_LOGD("Event-handler called for event %s/%u (%p)", base, id, data);

    // Trigger the waiting task via the semaphore
    data->trigger_event_base = base;
    data->trigger_event_id = id;
    data->trigger_event_data = event_data;
    xSemaphoreGive(data->triggered);
}

// This function waits for the passed event to be triggered.
// NOTE: DOES NOT register the event handler - assumes it is already registered.
// Returns ESP_OK if event triggered (output id params populated), or ESP_NO_EVENT if not
// (ie. timed-out).
esp_err_t sync_wait_event(esp_event_base_t event_base, int32_t event_id, wait_event_data_t* wait_event_data,
    esp_event_base_t* trigger_event_base, int32_t* trigger_event_id, void** trigger_event_data, TickType_t max_wait)
{
    JADE_ASSERT(wait_event_data);

    // we will use that to un-register the handler later
    wait_event_data->register_event_base = event_base;
    wait_event_data->register_event_id = event_id;

    JADE_LOGD("Awaiting event %s/%u (%p) (timeout = %u)", event_base, event_id, wait_event_data, max_wait);
    if (!max_wait) {
        while (xSemaphoreTake(wait_event_data->triggered, portMAX_DELAY) != pdTRUE) {
            // wait for the event to be triggered
        }
    } else {
        if (xSemaphoreTake(wait_event_data->triggered, max_wait) != pdTRUE) {
            JADE_LOGD("Event %s/%u (%p) timed-out", event_base, event_id, wait_event_data);
            return ESP_NO_EVENT;
        }
    }

    // ESP_OK means the event was fired, so copy the ids into the output params
    JADE_LOGD("Event %s/%u (%p) received in waiting task", event_base, event_id, wait_event_data);
    if (trigger_event_base) {
        *trigger_event_base = wait_event_data->trigger_event_base;
    }
    if (trigger_event_id) {
        *trigger_event_id = wait_event_data->trigger_event_id;
    }
    if (trigger_event_data) {
        *trigger_event_data = wait_event_data->trigger_event_data;
    }

    return ESP_OK;
}

// Function to block waiting for a specific event.
// Registers the event, then waits for it, and then unregisters.
// Returns ESP_OK if event triggered (output id params populated), or ESP_NO_EVENT if not
// (ie. timed-out).
esp_err_t sync_await_single_event(esp_event_base_t event_base, int32_t event_id, esp_event_base_t* trigger_event_base,
    int32_t* trigger_event_id, void** trigger_event_data, TickType_t max_wait)
{
    // Register for event
    esp_event_handler_instance_t ctx;
    wait_event_data_t* wait_data = make_wait_event_data();
    esp_event_handler_instance_register(event_base, event_id, sync_wait_event_handler, wait_data, &ctx);

    // Block awaiting the event
    const esp_err_t retval = sync_wait_event(
        event_base, event_id, wait_data, trigger_event_base, trigger_event_id, trigger_event_data, max_wait);

    // Unregister and free data
    esp_event_handler_instance_unregister(event_base, event_id, ctx);
    free_wait_event_data(wait_data);

    return retval;
}
