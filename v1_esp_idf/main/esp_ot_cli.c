/*
 * ESP32 OpenThread UDP Communication with LED Control
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

// ============================================================================
// CONFIGURATION CONSTANTS
// ============================================================================

#define TAG "ot_esp_cli"

#define LED_GPIO_PIN 8
#define OT_CONNECTION_LED_PORT 1234
#define HELLO_INTERVAL_MS 1000
#define LED_STRIP_LED_NUM 1

// ============================================================================
// GLOBAL VARIABLES
// ============================================================================

static esp_timer_handle_t hello_timer;
static bool streaming = false;
static otUdpSocket udpSocket;
static led_strip_handle_t led_strip;

// ============================================================================
// OPENTHREAD NETWORK INITIALIZATION
// ============================================================================

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

    esp_netif_t *openthread_netif;
    // Initialize the esp_netif bindings
    openthread_netif = init_openthread_netif(&config);
    esp_netif_set_default_netif(openthread_netif);

    // Run the main loop
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

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

static void get_mac_suffix(char *buf, size_t buflen) {
    otInstance *instance = esp_openthread_get_instance();
    const otExtAddress *ext_addr = otLinkGetExtendedAddress(instance);
    snprintf(buf, buflen, "%02X%02X", ext_addr->m8[6], ext_addr->m8[7]);
}

// ============================================================================
// LED CONTROL FUNCTIONS
// ============================================================================

// Turn on LED strip (white color)
static void led_on(void) {
    led_strip_set_pixel(led_strip, 0, 16, 16, 16); // white
    led_strip_refresh(led_strip);
}

// Turn off LED strip
static void led_off(void) {
    led_strip_clear(led_strip);
}

// ============================================================================
// UDP MESSAGING FUNCTIONS
// ============================================================================

// Send periodic hello messages with device MAC suffix
static void send_hello(void *arg) {
    otInstance *instance = esp_openthread_get_instance();
    char mac[5];
    get_mac_suffix(mac, sizeof(mac));
    char msg[32];
    snprintf(msg, sizeof(msg), "hello world %s", mac);

    otMessage *message = otUdpNewMessage(instance, NULL);
    if (!message) return;
    otMessageAppend(message, msg, strlen(msg));

    otMessageInfo msgInfo = {0};
    otIp6AddressFromString("ff03::1", &msgInfo.mPeerAddr);
    msgInfo.mPeerPort = OT_CONNECTION_LED_PORT;

    otUdpSend(instance, &udpSocket, message, &msgInfo);
}

// Handle incoming UDP messages - respond to "start" and "stop" commands
static void udp_receive_cb(void *aContext, otMessage *aMessage, const otMessageInfo *aMessageInfo) {
    char buf[32];
    int len = otMessageRead(aMessage, 0, buf, sizeof(buf) - 1);
    buf[len] = 0;

    if (strstr(buf, "start") && !streaming) {
        streaming = true;
        led_on();
        esp_timer_start_periodic(hello_timer, HELLO_INTERVAL_MS * 1000);
    } else if (strstr(buf, "stop") && streaming) {
        streaming = false;
        led_off();
        esp_timer_stop(hello_timer);
    }
}

// ============================================================================
// THREAD NETWORK CONFIGURATION
// ============================================================================

// Automatically enable IPv6 interface and start Thread protocol
static void configure_thread_network(otInstance *instance) {
    // Enable IPv6 interface (equivalent to "ifconfig up")
    otIp6SetEnabled(instance, true);
    
    // Start Thread protocol (equivalent to "thread start")
    otThreadSetEnabled(instance, true);
    
    // Wait for network to establish (up to 30 seconds)
    for (int i = 0; i < 30; i++) {
        otDeviceRole role = otThreadGetDeviceRole(instance);
        if (role == OT_DEVICE_ROLE_CHILD || role == OT_DEVICE_ROLE_ROUTER || role == OT_DEVICE_ROLE_LEADER) {
            return; // Network established successfully
        }
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}

// ============================================================================
// MAIN TASK FUNCTIONS
// ============================================================================

// UDP task: Configure Thread network and set up UDP communication
static void udp_task(void *pvParameters) {
    // Wait for OpenThread stack to be ready
    vTaskDelay(5000 / portTICK_PERIOD_MS);

    otInstance *instance = esp_openthread_get_instance();
    configure_thread_network(instance);

    // Create timer for periodic hello messages
    esp_timer_create_args_t timer_args = {
        .callback = &send_hello,
        .name = "hello_timer"
    };
    esp_timer_create(&timer_args, &hello_timer);

    // Set up UDP socket and bind to listening port
    otSockAddr listen_addr = {0};
    otIp6AddressFromString("::", &listen_addr.mAddress);
    listen_addr.mPort = OT_CONNECTION_LED_PORT;
    otUdpOpen(instance, &udpSocket, udp_receive_cb, NULL);
    otUdpBind(instance, &udpSocket, &listen_addr, OT_NETIF_THREAD_INTERNAL);

    // Task sleeps - UDP callbacks handle all communication
    while (1) {
        vTaskDelay(10000 / portTICK_PERIOD_MS);
    }
}

// ============================================================================
// HARDWARE INITIALIZATION
// ============================================================================

// Configure addressable LED strip on GPIO pin
static void configure_led_strip(void) {
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

// ============================================================================
// MAIN APPLICATION ENTRY POINT
// ============================================================================

void app_main(void)
{
    // LED startup sequence - visual feedback that device is booting
    configure_led_strip();
    led_on();
    vTaskDelay(500 / portTICK_PERIOD_MS);
    led_off();
    vTaskDelay(500 / portTICK_PERIOD_MS);
    led_on();
    vTaskDelay(500 / portTICK_PERIOD_MS);
    led_off();

    // Initialize ESP-IDF components
    esp_vfs_eventfd_config_t eventfd_config = { .max_fds = 3 };
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_vfs_eventfd_register(&eventfd_config));
    
    // Start OpenThread and UDP communication tasks
    xTaskCreate(ot_task_worker, "ot_cli_main", 10240, xTaskGetCurrentTaskHandle(), 5, NULL);
    xTaskCreate(udp_task, "udp_task", 8192, NULL, 5, NULL);
}
