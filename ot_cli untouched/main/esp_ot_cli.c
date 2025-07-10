/*
 * SPDX-FileCopyrightText: 2021-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * OpenThread Command Line Example
 *
 * This example code is in the Public Domain (or CC0 licensed, at your option.)
 *
 * Unless required by applicable law or agreed to in writing, this
 * software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied.
*/

#include <stdio.h>
#include <unistd.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_types.h"
#include "esp_openthread.h"
#include "esp_openthread_cli.h"
#include "esp_openthread_lock.h"
#include "esp_openthread_netif_glue.h"
#include "esp_openthread_types.h"
#include "esp_ot_config.h"
#include "esp_vfs_eventfd.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/uart_types.h"
#include "nvs_flash.h"
#include "openthread/cli.h"
#include "openthread/instance.h"
#include "openthread/logging.h"
#include "openthread/tasklet.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "openthread/udp.h"

#if CONFIG_OPENTHREAD_STATE_INDICATOR_ENABLE
#include "ot_led_strip.h"
#endif

#if CONFIG_OPENTHREAD_CLI_ESP_EXTENSION
#include "esp_ot_cli_extension.h"
#endif // CONFIG_OPENTHREAD_CLI_ESP_EXTENSION

#define TAG "ot_esp_cli"

#define LED_GPIO_PIN 8
#define OT_CONNECTION_LED_PORT 1234
#define HELLO_INTERVAL_MS 1000

static esp_timer_handle_t hello_timer;
static bool streaming = false;
static otUdpSocket udpSocket;

static esp_netif_t *init_openthread_netif(const esp_openthread_platform_config_t *config)
{
    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_OPENTHREAD();
    esp_netif_t *netif = esp_netif_new(&cfg);
    assert(netif != NULL);
    ESP_ERROR_CHECK(esp_netif_attach(netif, esp_openthread_netif_glue_init(config)));

    return netif;
}

static void ot_task_worker(void *aContext)
{
    esp_openthread_platform_config_t config = {
        .radio_config = ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG(),
        .host_config = ESP_OPENTHREAD_DEFAULT_HOST_CONFIG(),
        .port_config = ESP_OPENTHREAD_DEFAULT_PORT_CONFIG(),
    };

    // Initialize the OpenThread stack
    ESP_ERROR_CHECK(esp_openthread_init(&config));

#if CONFIG_OPENTHREAD_STATE_INDICATOR_ENABLE
    ESP_ERROR_CHECK(esp_openthread_state_indicator_init(esp_openthread_get_instance()));
#endif

#if CONFIG_OPENTHREAD_LOG_LEVEL_DYNAMIC
    // The OpenThread log level directly matches ESP log level
    (void)otLoggingSetLevel(CONFIG_LOG_DEFAULT_LEVEL);
#endif
    // Initialize the OpenThread cli
#if CONFIG_OPENTHREAD_CLI
    esp_openthread_cli_init();
#endif

    esp_netif_t *openthread_netif;
    // Initialize the esp_netif bindings
    openthread_netif = init_openthread_netif(&config);
    esp_netif_set_default_netif(openthread_netif);

#if CONFIG_OPENTHREAD_CLI_ESP_EXTENSION
    esp_cli_custom_command_init();
#endif // CONFIG_OPENTHREAD_CLI_ESP_EXTENSION

    // Run the main loop
#if CONFIG_OPENTHREAD_CLI
    esp_openthread_cli_create_task();
#endif
#if CONFIG_OPENTHREAD_AUTO_START
    otOperationalDatasetTlvs dataset;
    otError error = otDatasetGetActiveTlvs(esp_openthread_get_instance(), &dataset);
    ESP_ERROR_CHECK(esp_openthread_auto_start((error == OT_ERROR_NONE) ? &dataset : NULL));
#endif
    esp_openthread_launch_mainloop();

    // Clean up
    esp_openthread_netif_glue_deinit();
    esp_netif_destroy(openthread_netif);

    esp_vfs_eventfd_unregister();
    vTaskDelete(NULL);
}

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
    char buf[32];
    int len = otMessageRead(aMessage, 0, buf, sizeof(buf) - 1);
    buf[len] = 0;

    char addr_str[OT_IP6_ADDRESS_STRING_SIZE];
    otIp6AddressToString(&aMessageInfo->mPeerAddr, addr_str, sizeof(addr_str));

    ESP_LOGI(TAG, "UDP received, payload=%s", buf);
    ESP_LOGI(TAG, "From address: %s, port: %d", addr_str, aMessageInfo->mPeerPort);

    if (strcmp(buf, "start") == 0 && !streaming) {
        streaming = true;
        gpio_set_level(LED_GPIO_PIN, 1);
        esp_timer_start_periodic(hello_timer, HELLO_INTERVAL_MS * 1000);
        ESP_LOGI(TAG, "Received start, streaming...");
    } else if (strcmp(buf, "stop") == 0 && streaming) {
        streaming = false;
        gpio_set_level(LED_GPIO_PIN, 0);
        esp_timer_stop(hello_timer);
        ESP_LOGI(TAG, "Received stop, streaming stopped.");
    }
}

static void configure_thread_network(otInstance *instance) {
    ESP_LOGI(TAG, "Thread network configuration is expected to be set via CLI.");
}

void app_main(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << LED_GPIO_PIN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);

    esp_timer_create_args_t timer_args = {
        .callback = &send_hello,
        .name = "hello_timer"
    };
    esp_timer_create(&timer_args, &hello_timer);

    otInstance *instance = esp_openthread_get_instance();
    configure_thread_network(instance);

    otSockAddr listen_addr = {0};
    listen_addr.mPort = OT_CONNECTION_LED_PORT;
    otUdpOpen(instance, &udpSocket, udp_receive_cb, NULL);
    otUdpBind(instance, &udpSocket, &listen_addr, OT_NETIF_THREAD_INTERNAL);

    // Used eventfds:
    // * netif
    // * ot task queue
    // * radio driver
    esp_vfs_eventfd_config_t eventfd_config = {
        .max_fds = 3,
    };

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_vfs_eventfd_register(&eventfd_config));
    xTaskCreate(ot_task_worker, "ot_cli_main", 10240, xTaskGetCurrentTaskHandle(), 5, NULL);
}
