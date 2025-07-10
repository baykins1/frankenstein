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

// Add NVS support for OpenThread settings storage
#include "nvs_flash.h"

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

static void log_thread_network_info(otInstance *instance) {
    ESP_LOGI(TAG, "=== OpenThread Network Status ===");
    
    // Device role
    otDeviceRole role = otThreadGetDeviceRole(instance);
    const char* role_str = "Unknown";
    switch(role) {
        case OT_DEVICE_ROLE_DISABLED: role_str = "Disabled"; break;
        case OT_DEVICE_ROLE_DETACHED: role_str = "Detached"; break;
        case OT_DEVICE_ROLE_CHILD: role_str = "Child"; break;
        case OT_DEVICE_ROLE_ROUTER: role_str = "Router"; break;
        case OT_DEVICE_ROLE_LEADER: role_str = "Leader"; break;
    }
    ESP_LOGI(TAG, "Device Role: %s", role_str);
    
    // Network state
    if (otThreadGetDeviceRole(instance) >= OT_DEVICE_ROLE_CHILD) {
        ESP_LOGI(TAG, "Thread Network: CONNECTED");
        
        // Network name
        const char *networkName = otThreadGetNetworkName(instance);
        ESP_LOGI(TAG, "Network Name: %s", networkName);
        
        // PAN ID
        ESP_LOGI(TAG, "PAN ID: 0x%04X", otLinkGetPanId(instance));
        
        // Extended PAN ID
        const otExtendedPanId *extPanId = otThreadGetExtendedPanId(instance);
        ESP_LOGI(TAG, "Extended PAN ID: %02x%02x%02x%02x%02x%02x%02x%02x",
                 extPanId->m8[0], extPanId->m8[1], extPanId->m8[2], extPanId->m8[3],
                 extPanId->m8[4], extPanId->m8[5], extPanId->m8[6], extPanId->m8[7]);
        
        // Channel
        ESP_LOGI(TAG, "Channel: %d", otLinkGetChannel(instance));
        
        // Link mode
        otLinkModeConfig linkMode = otThreadGetLinkMode(instance);
        ESP_LOGI(TAG, "Link Mode: RxOnWhenIdle=%d, DeviceType=%d, NetworkData=%d", 
                 linkMode.mRxOnWhenIdle, linkMode.mDeviceType, linkMode.mNetworkData);
    } else {
        ESP_LOGI(TAG, "Thread Network: DISCONNECTED");
        
        // Check if we have operational dataset
        otOperationalDataset dataset;
        if (otDatasetGetActive(instance, &dataset) == OT_ERROR_NONE) {
            ESP_LOGI(TAG, "Has Active Dataset: YES");
            if (dataset.mComponents.mIsNetworkNamePresent) {
                ESP_LOGI(TAG, "Configured Network Name: %.*s", 
                         (int)strlen((char*)dataset.mNetworkName.m8), 
                         dataset.mNetworkName.m8);
            }
            if (dataset.mComponents.mIsPanIdPresent) {
                ESP_LOGI(TAG, "Configured PAN ID: 0x%04X", dataset.mPanId);
            }
            if (dataset.mComponents.mIsChannelPresent) {
                ESP_LOGI(TAG, "Configured Channel: %d", dataset.mChannel);
            }
        } else {
            ESP_LOGI(TAG, "Has Active Dataset: NO");
        }
    }
    
    // Extended address (EUI-64)
    const otExtAddress *extAddr = otLinkGetExtendedAddress(instance);
    ESP_LOGI(TAG, "Extended Address: %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
             extAddr->m8[0], extAddr->m8[1], extAddr->m8[2], extAddr->m8[3],
             extAddr->m8[4], extAddr->m8[5], extAddr->m8[6], extAddr->m8[7]);
    
    ESP_LOGI(TAG, "=== End Network Status ===");
}


static void ot_task(void *pvParameter) {
    ESP_LOGI(TAG, "Starting OpenThread End Device");

    // Initialize NVS - Required for OpenThread settings storage
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS initialized successfully");

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

    // Log initial network status
    log_thread_network_info(instance);

    // Wait a bit more for OpenThread CLI/NVS to be fully ready
    ESP_LOGI(TAG, "Waiting for OpenThread CLI/NVS subsystem to be ready...");
    vTaskDelay(pdMS_TO_TICKS(3000));

    // Only enable the basic OpenThread stack - configuration must be done via CLI
    ESP_LOGI(TAG, "Enabling OpenThread IPv6 and Thread interfaces...");
    
    // Enable IPv6 interface
    otError error = otIp6SetEnabled(instance, true);
    if (error != OT_ERROR_NONE) {
        ESP_LOGE(TAG, "Failed to enable IP6 interface: %d", error);
    } else {
        ESP_LOGI(TAG, "IPv6 interface enabled");
    }
    
    // Enable Thread protocol  
    error = otThreadSetEnabled(instance, true);
    if (error != OT_ERROR_NONE) {
        ESP_LOGE(TAG, "Failed to enable Thread: %d", error);
    } else {
        ESP_LOGI(TAG, "Thread protocol enabled");
    }

    // Log network status after enabling
    log_thread_network_info(instance);

    ESP_LOGI(TAG, "OpenThread stack ready. Use CLI commands to configure network:");
    ESP_LOGI(TAG, "  dataset networkname ot_zephyr");
    ESP_LOGI(TAG, "  dataset panid 0xabcd");
    ESP_LOGI(TAG, "  dataset channel 11");
    ESP_LOGI(TAG, "  dataset extpanid dead00beef00cafe");
    ESP_LOGI(TAG, "  dataset networkkey 11112222333344445555666677778888");
    ESP_LOGI(TAG, "  dataset meshlocalprefix fd00:db8:a0:0::/64");
    ESP_LOGI(TAG, "  dataset commit active");
    ESP_LOGI(TAG, "  ifconfig up");
    ESP_LOGI(TAG, "  thread start");

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
    esp_err_t timer_ret = esp_timer_create(&timer_args, &hello_timer);
    if (timer_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create timer: %d", timer_ret);
        abort();
    }

    ESP_LOGI(TAG, "End device ready.");
    
    // Log initial network info
    log_thread_network_info(instance);
    
    // Configure OpenThread using sdkconfig parameters
    configure_openthread_from_sdkconfig(instance);
    
    // Keep the task running to process OpenThread events
    // Log network status every 10 seconds for debugging
    int status_counter = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        status_counter++;
        
        // Log network status every 10 seconds
        if (status_counter >= 10) {
            log_thread_network_info(instance);
            status_counter = 0;
        }
    }
}

void app_main(void) {
    // Create task with higher priority and more stack space
    xTaskCreate(ot_task, "ot_task", 16384, NULL, 10, NULL);
}
