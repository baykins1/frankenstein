/*
 * Controller Application Main
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <openthread/thread_ftd.h>
#include <openthread/joiner.h>
#include <openthread/link.h>
#include <openthread/dataset_ftd.h>

#include <zephyr/net/openthread.h>
#include <openthread/udp.h>
#include <openthread/message.h>

LOG_MODULE_REGISTER(ot_controller, CONFIG_LOG_DEFAULT_LEVEL);

#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

/* OpenThread networking definitions */
#define OT_CONNECTION_LED_PORT 1234
static const char *light_command = "toggle";

static bool streaming = false;

static const char *CMD_START = "start";
static const char *CMD_STOP  = "stop";

/* GPIO definitions for the button */
#define SW0_NODE DT_ALIAS(sw0)
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(SW0_NODE, gpios);
static struct gpio_callback button_cb_data;

static void set_thread_network_config(otInstance *instance)
{
    otOperationalDataset dataset;
    memset(&dataset, 0, sizeof(dataset));

    // Network Name
    dataset.mComponents.mIsNetworkNamePresent = true;
    strncpy(dataset.mNetworkName.m8, "Campos", OT_NETWORK_NAME_MAX_SIZE);

    // Network Key
    dataset.mComponents.mIsNetworkKeyPresent = true;
    uint8_t key[16] = {0x11,0x11,0x22,0x22,0x33,0x33,0x44,0x44,0x55,0x55,0x66,0x66,0x77,0x77,0x88,0x88};
    memcpy(dataset.mNetworkKey.m8, key, 16);

    
    // PAN ID
    dataset.mComponents.mIsPanIdPresent = true;
    dataset.mPanId = 0xABCD;

    // Channel
    dataset.mComponents.mIsChannelPresent = true;
    dataset.mChannel = 15;

    // Apply dataset
    otDatasetSetActive(instance, &dataset);
}

void send_multicast_command(const char *cmd)
{
    otInstance *instance = openthread_get_default_instance();
    otMessage *message = otUdpNewMessage(instance, NULL);
    if (!message) return;

    otMessageAppend(message, cmd, strlen(cmd));

    otMessageInfo msgInfo = {0};
    otIp6AddressFromString("ff03::1", &msgInfo.mPeerAddr); // Mesh-local multicast
    msgInfo.mPeerPort = OT_CONNECTION_LED_PORT;

    otUdpSocket udpSocket;
    if (otUdpOpen(instance, &udpSocket, NULL, NULL) == OT_ERROR_NONE) {
        otUdpSend(instance, &udpSocket, message, &msgInfo);
        otUdpClose(instance, &udpSocket);
    } else {
        otMessageFree(message);
    }
}

void button_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    streaming = !streaming;
    if (streaming) {
        gpio_pin_set_dt(&led, 1);
        printk("Streaming started\n");
        LOG_INF("Streaming started");
        send_multicast_command(CMD_START);
    } else {
        gpio_pin_set_dt(&led, 0);
        printk("Streaming stopped\n");
        LOG_INF("Streaming stopped");
        send_multicast_command(CMD_STOP);
    }
}

/* UDP sending logic */
void send_light_control_command(void)
{
    otError error = OT_ERROR_NONE;
    otMessage *message;
    otMessageInfo messageInfo;
    otInstance *p_ot_instance = openthread_get_default_instance();
    otUdpSocket udpSocket;

    memset(&messageInfo, 0, sizeof(messageInfo));
    otIp6AddressFromString("ff03::1", &messageInfo.mPeerAddr);
    messageInfo.mPeerPort = OT_CONNECTION_LED_PORT;

    message = otUdpNewMessage(p_ot_instance, NULL);
    if (message == NULL) {
        LOG_ERR("Failed to allocate new UDP message.");
        return;
    }

    error = otMessageAppend(message, light_command, strlen(light_command));
    if (error != OT_ERROR_NONE) {
        otMessageFree(message);
        return;
    }

    error = otUdpOpen(p_ot_instance, &udpSocket, NULL, NULL);
    if (error != OT_ERROR_NONE) {
        otMessageFree(message);
        return;
    }

    error = otUdpSend(p_ot_instance, &udpSocket, message, &messageInfo);
    if (error != OT_ERROR_NONE) {
        LOG_ERR("Failed to send UDP message: %d", error);
    } else {
        LOG_INF("UDP message sent successfully!");
    }

    otUdpClose(p_ot_instance, &udpSocket);
}

/* Button press callback function 
void button_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    LOG_INF("Button pressed, sending command.");
    send_light_control_command();
}
*/
int main(void)
{
    k_sleep(K_MSEC(500));
    
    int ret;

    LOG_INF("Starting OpenThread Controller Application");

    // LED init
    if (!device_is_ready(led.port)) {
        LOG_ERR("LED device not ready");
        return -1;
    }
    gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);

    // Button init
    if (!device_is_ready(button.port)) {
        LOG_ERR("Button device not ready");
        return -1;
    }
    ret = gpio_pin_configure_dt(&button, GPIO_INPUT);
    if (ret < 0) return -1;
    ret = gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_TO_ACTIVE);
    if (ret < 0) return -1;
    gpio_init_callback(&button_cb_data, button_pressed, BIT(button.pin));
    gpio_add_callback(button.port, &button_cb_data);

    LOG_INF("Button and LED initialized.");

    // Start OpenThread
    otInstance *instance = openthread_get_default_instance();

    // Only set dataset if not already set
    otOperationalDataset currentDataset;
    otError err = otDatasetGetActive(instance, &currentDataset);
    if (err != OT_ERROR_NONE || !currentDataset.mComponents.mIsNetworkNamePresent) {
        set_thread_network_config(instance);
    }

    if (openthread_start(openthread_get_default_context()) != 0) {
        LOG_ERR("Failed to start OpenThread");
        return -1;
    }
    LOG_INF("OpenThread stack has been started.");

    return 0;
}