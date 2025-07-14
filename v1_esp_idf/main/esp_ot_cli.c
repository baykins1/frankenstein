/*
*/

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include "sdkconfig.h"
#include "hal/uart_types.h"
#include "nvs_flash.h"
#include "led_strip.h"
#include "driver/gpio.h"
#include "driver/uart.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_types.h"
#include "esp_vfs_eventfd.h"
#include "esp_timer.h"

#include "esp_openthread.h"
#include "esp_openthread_cli.h"
#include "esp_openthread_lock.h"
#include "esp_openthread_netif_glue.h"
#include "esp_openthread_types.h"
#include "esp_ot_config.h"

#include "openthread/cli.h"
#include "openthread/instance.h"
#include "openthread/logging.h"
#include "openthread/tasklet.h"
#include "openthread/udp.h"

#define TAG "ot_esp_cli"

#define LED_GPIO_PIN 8
#define OT_CONNECTION_LED_PORT 1234
#define HELLO_INTERVAL_MS 1000
#define LED_STRIP_LED_NUM 1

static esp_timer_handle_t hello_timer;
static bool streaming = false;
static otUdpSocket udpSocket;
static led_strip_handle_t led_strip;

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

/*
#if CONFIG_OPENTHREAD_CLI_ESP_EXTENSION
    esp_cli_custom_command_init();
#endif // CONFIG_OPENTHREAD_CLI_ESP_EXTENSION
*/
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

// LED control functions
static void led_on(void) {
    led_strip_set_pixel(led_strip, 0, 16, 16, 16); // white
    led_strip_refresh(led_strip);
}

static void led_off(void) {
    led_strip_clear(led_strip);
}

static void send_hello(void *arg) {
    ESP_LOGI(TAG, "send_hello timer fired");
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
    ESP_LOGI(TAG, "udp_receive_cb called");
    char buf[32];
    int len = otMessageRead(aMessage, 0, buf, sizeof(buf) - 1);
    buf[len] = 0;

    char addr_str[OT_IP6_ADDRESS_STRING_SIZE];
    otIp6AddressToString(&aMessageInfo->mPeerAddr, addr_str, sizeof(addr_str));

    ESP_LOGI(TAG, "UDP received, payload=%s", buf);
    ESP_LOGI(TAG, "From address: %s, port: %d", addr_str, aMessageInfo->mPeerPort);
    ESP_LOGI(TAG, "streaming=%d, strstr_start=%p, strstr_stop=%p", streaming, strstr(buf, "start"), strstr(buf, "stop"));

    if (strstr(buf, "start") && !streaming) {
        ESP_LOGI(TAG, "Entering start block");
        streaming = true;
        led_on();
        esp_timer_start_periodic(hello_timer, HELLO_INTERVAL_MS * 1000);
        ESP_LOGI(TAG, "Received start, streaming...");
    } else if (strstr(buf, "stop") && streaming) {
        ESP_LOGI(TAG, "Entering stop block");
        streaming = false;
        led_off();
        esp_timer_stop(hello_timer);
        ESP_LOGI(TAG, "Received stop, streaming stopped.");
    }
}

static void configure_thread_network(otInstance *instance) {
    ESP_LOGI(TAG, "Configuring Thread network automatically...");
    
    // Check if we already have an active dataset
    otOperationalDatasetTlvs dataset;
    otError error = otDatasetGetActiveTlvs(instance, &dataset);
    
    if (error != OT_ERROR_NONE) {
        ESP_LOGI(TAG, "No active dataset found. Starting with basic configuration...");
    } else {
        ESP_LOGI(TAG, "Active dataset found, proceeding with network startup...");
    }
    
    // Enable IPv6 interface (equivalent to "ifconfig up")
    ESP_LOGI(TAG, "Enabling IPv6 interface...");
    error = otIp6SetEnabled(instance, true);
    if (error == OT_ERROR_NONE) {
        ESP_LOGI(TAG, "IPv6 enabled successfully");
    } else {
        ESP_LOGE(TAG, "Failed to enable IPv6: %d", error);
        return;
    }
    
    // Start Thread (equivalent to "thread start")
    ESP_LOGI(TAG, "Starting Thread protocol...");
    error = otThreadSetEnabled(instance, true);
    if (error == OT_ERROR_NONE) {
        ESP_LOGI(TAG, "Thread started successfully");
    } else {
        ESP_LOGE(TAG, "Failed to start Thread: %d", error);
        return;
    }
    
    // Wait for the network to stabilize and check status
    ESP_LOGI(TAG, "Waiting for Thread network to establish...");
    int max_wait_seconds = 30;
    int wait_count = 0;
    
    while (wait_count < max_wait_seconds) {
        otDeviceRole role = otThreadGetDeviceRole(instance);
        
        if (role == OT_DEVICE_ROLE_CHILD || role == OT_DEVICE_ROLE_ROUTER || role == OT_DEVICE_ROLE_LEADER) {
            const char* roleStr;
            switch (role) {
                case OT_DEVICE_ROLE_CHILD: roleStr = "child"; break;
                case OT_DEVICE_ROLE_ROUTER: roleStr = "router"; break;
                case OT_DEVICE_ROLE_LEADER: roleStr = "leader"; break;
                default: roleStr = "attached"; break;
            }
            ESP_LOGI(TAG, "Thread network established! Device role: %s", roleStr);
            return;
        }
        
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        wait_count++;
        
        // Provide periodic status updates
        if (wait_count % 5 == 0) {
            const char* roleStr;
            switch (otThreadGetDeviceRole(instance)) {
                case OT_DEVICE_ROLE_DISABLED: roleStr = "disabled"; break;
                case OT_DEVICE_ROLE_DETACHED: roleStr = "detached"; break;
                case OT_DEVICE_ROLE_CHILD: roleStr = "child"; break;
                case OT_DEVICE_ROLE_ROUTER: roleStr = "router"; break;
                case OT_DEVICE_ROLE_LEADER: roleStr = "leader"; break;
                default: roleStr = "unknown"; break;
            }
            ESP_LOGI(TAG, "Still connecting... Current role: %s (%d/%d seconds)", roleStr, wait_count, max_wait_seconds);
        }
    }
    
    // Network didn't fully establish, but log the final state
    ESP_LOGW(TAG, "Thread network setup completed, but device may still be connecting after %d seconds", max_wait_seconds);
    const char* roleStr;
    otDeviceRole finalRole = otThreadGetDeviceRole(instance);
    switch (finalRole) {
        case OT_DEVICE_ROLE_DISABLED: roleStr = "disabled"; break;
        case OT_DEVICE_ROLE_DETACHED: roleStr = "detached"; break;
        case OT_DEVICE_ROLE_CHILD: roleStr = "child"; break;
        case OT_DEVICE_ROLE_ROUTER: roleStr = "router"; break;
        case OT_DEVICE_ROLE_LEADER: roleStr = "leader"; break;
        default: roleStr = "unknown"; break;
    }
    ESP_LOGW(TAG, "Final Thread role: %s", roleStr);
    
    if (finalRole == OT_DEVICE_ROLE_DETACHED) {
        ESP_LOGW(TAG, "Device is detached. This may mean:");
        ESP_LOGW(TAG, "  - No dataset configured (need 'dataset init new; dataset commit active')");
        ESP_LOGW(TAG, "  - No other Thread devices in range to join");
        ESP_LOGW(TAG, "  - Use CLI to manually configure the network");
    }
}

static void udp_task(void *pvParameters) {
    ESP_LOGI(TAG, "udp_task started");
    // Wait for OpenThread stack and CLI to be ready
    vTaskDelay(5000 / portTICK_PERIOD_MS); // Wait 5 seconds for network up (adjust as needed)

    otInstance *instance = esp_openthread_get_instance();
    configure_thread_network(instance);
    ESP_LOGI(TAG, "Thread network configured (udp_task)");

    esp_timer_create_args_t timer_args = {
        .callback = &send_hello,
        .name = "hello_timer"
    };
    esp_timer_create(&timer_args, &hello_timer);
    ESP_LOGI(TAG, "Timer created (udp_task)");

    otSockAddr listen_addr = {0};
    otIp6AddressFromString("::", &listen_addr.mAddress);
    listen_addr.mPort = OT_CONNECTION_LED_PORT;
    otUdpOpen(instance, &udpSocket, udp_receive_cb, NULL);
    otUdpBind(instance, &udpSocket, &listen_addr, OT_NETIF_THREAD_INTERNAL);
    ESP_LOGI(TAG, "UDP socket opened and bound (udp_task)");

    // Task can sleep forever, UDP and timer callbacks do the work
    while (1) {
        vTaskDelay(10000 / portTICK_PERIOD_MS);
    }
}

static void configure_led_strip(void) {
    ESP_LOGI(TAG, "Configuring addressable LED strip!");
    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_GPIO_PIN,
        .max_leds = LED_STRIP_LED_NUM,
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
    led_strip_clear(led_strip);
}

void app_main(void)
{
    ESP_LOGI(TAG, "app_main started");
    configure_led_strip();
    led_on();
    vTaskDelay(500 / portTICK_PERIOD_MS);
    led_off();
    vTaskDelay(500 / portTICK_PERIOD_MS);
    led_on();
    vTaskDelay(500 / portTICK_PERIOD_MS);
    led_off();

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
    ESP_LOGI(TAG, "Before xTaskCreate");
    xTaskCreate(ot_task_worker, "ot_cli_main", 10240, xTaskGetCurrentTaskHandle(), 5, NULL);
    xTaskCreate(udp_task, "udp_task", 8192, NULL, 5, NULL);
    ESP_LOGI(TAG, "app_main finished");
}
