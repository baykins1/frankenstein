/*
 * ESP-IDF port of Zephyr OpenThread Transmitter Application
 */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "openthread/instance.h"
#include "openthread/thread_ftd.h"
#include "openthread/udp.h"
#include "openthread/dataset_ftd.h"
#include "esp_openthread.h"

#define LED_GPIO 8 // GPIO number for LED
#define OT_CONNECTION_LED_PORT 1234
#define HELLO_INTERVAL_MS 1000

static const char *TAG = "ot_end_device";
static bool streaming = false;
static otUdpSocket udpSocket;
static esp_timer_handle_t hello_timer;

static void get_mac_suffix(char *buf, size_t buflen) {
    otInstance *instance = esp_openthread_get_instance();
    const otExtAddress *ext_addr = otLinkGetExtendedAddress(instance);
    snprintf(buf, buflen, "%02X%02X", ext_addr->m8[6], ext_addr->m8[7]);
}

static void send_hello(void *arg) {
    otInstance *instance = esp_openthread_get_instance();
    char mac[5];
    get_mac_suffix(mac, sizeof(mac));
    char msg[32];
    snprintf(msg, sizeof(msg), "hello world %s", mac);

    ESP_LOGI(TAG, "Sending: %s", msg);

    otMessage *message = otUdpNewMessage(instance, NULL);
    if (!message) return;
    otMessageAppend(message, msg, strlen(msg));

    otMessageInfo msgInfo = {0};
    otIp6AddressFromString("ff03::1", &msgInfo.mPeerAddr);
    msgInfo.mPeerPort = OT_CONNECTION_LED_PORT;

    otUdpSend(instance, &udpSocket, message, &msgInfo);
}

static void udp_receive_cb(void *aContext, otMessage *aMessage, const otMessageInfo *aMessageInfo) {
    char buf[16];
    int len = otMessageRead(aMessage, 0, buf, sizeof(buf) - 1);
    buf[len] = 0;

    ESP_LOGI(TAG, "UDP received, payload=%s", buf);

    if (strcmp(buf, "start") == 0 && !streaming) {
        streaming = true;
        gpio_set_level(LED_GPIO, 1);
        esp_timer_start_periodic(hello_timer, HELLO_INTERVAL_MS * 1000);
        ESP_LOGI(TAG, "Received start, streaming...");
    } else if (strcmp(buf, "stop") == 0 && streaming) {
        streaming = false;
        gpio_set_level(LED_GPIO, 0);
        esp_timer_stop(hello_timer);
        ESP_LOGI(TAG, "Received stop, streaming stopped.");
    }
}

static void set_thread_network_config(otInstance *instance) {
    otOperationalDataset dataset;
    memset(&dataset, 0, sizeof(dataset));
    dataset.mComponents.mIsNetworkNamePresent = true;
    strncpy(dataset.mNetworkName.m8, "ot_zephyr", OT_NETWORK_NAME_MAX_SIZE);
    dataset.mComponents.mIsNetworkKeyPresent = true;
    uint8_t key[16] = {0x11, 0x11, 0x22, 0x22, 0x33, 0x33, 0x44, 0x44,
                       0x55, 0x55, 0x66, 0x66, 0x77, 0x77, 0x88, 0x88};
    memcpy(dataset.mNetworkKey.m8, key, 16);
    dataset.mComponents.mIsPanIdPresent = true;
    dataset.mPanId = 0xABCD;
    dataset.mComponents.mIsChannelPresent = true;
    dataset.mChannel = 11;
    otDatasetSetActive(instance, &dataset);
}

static void ot_task(void *pvParameter) {
    ESP_LOGI(TAG, "Starting OpenThread End Device");

    // LED init
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = 0,
        .pull_down_en = 0,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
    gpio_set_level(LED_GPIO, 0);

    // Wait longer for OpenThread CLI to be fully initialized
    vTaskDelay(pdMS_TO_TICKS(5000));
    
    otInstance *instance = esp_openthread_get_instance();
    if (!instance) {
        ESP_LOGE(TAG, "OpenThread instance not available - CLI may not be started");
        vTaskDelay(pdMS_TO_TICKS(5000));
        instance = esp_openthread_get_instance();
        if (!instance) {
            ESP_LOGE(TAG, "OpenThread instance still not available after waiting");
            abort();
        }
    }

    ESP_LOGI(TAG, "OpenThread instance obtained successfully");

    // Don't set network configuration automatically - let the CLI handle it
    ESP_LOGI(TAG, "OpenThread stack ready. Use CLI commands to configure network.");

    // Open UDP socket for multicast commands
    otSockAddr listen_addr = {0};
    listen_addr.mPort = OT_CONNECTION_LED_PORT;
    
    otError err = otUdpOpen(instance, &udpSocket, udp_receive_cb, NULL);
    if (err != OT_ERROR_NONE) {
        ESP_LOGE(TAG, "Failed to open UDP socket: %d", err);
        abort();
    }
    
    err = otUdpBind(instance, &udpSocket, &listen_addr, OT_NETIF_THREAD_HOST);
    if (err != OT_ERROR_NONE) {
        ESP_LOGE(TAG, "Failed to bind UDP socket: %d", err);
        abort();
    }

    ESP_LOGI(TAG, "UDP socket bound to port %d", OT_CONNECTION_LED_PORT);

    // Create hello timer (not started until 'start' command)
    const esp_timer_create_args_t timer_args = {
        .callback = &send_hello,
        .arg = NULL,
        .name = "hello_timer"
    };
    esp_err_t ret = esp_timer_create(&timer_args, &hello_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create timer: %d", ret);
        abort();
    }

    ESP_LOGI(TAG, "End device ready.");
    
    // Keep the task running to process OpenThread events
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void) {
    // Create task with higher priority and more stack space
    xTaskCreate(ot_task, "ot_task", 16384, NULL, 10, NULL);
}
