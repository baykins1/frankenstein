/*
 * Controller Application Main
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include <zephyr/net/openthread.h>
#include <openthread/udp.h>
#include <openthread/message.h>

#include <openthread/border_router.h>

LOG_MODULE_REGISTER(ot_controller, CONFIG_LOG_DEFAULT_LEVEL);

/* OpenThread networking definitions */
#define OT_CONNECTION_LED_PORT 1234
static const char *light_command = "toggle";

/* GPIO definitions for the button */
#define SW0_NODE DT_ALIAS(sw0)
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(SW0_NODE, gpios);
static struct gpio_callback button_cb_data;

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

/* Button press callback function */
void button_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    LOG_INF("Button pressed, sending command.");
    send_light_control_command();
}

int main(void)
{
    int ret;

    LOG_INF("Starting OpenThread Controller Application");

    /* --- GPIO Button Initialization --- */
    if (!device_is_ready(button.port)) {
        LOG_ERR("Button device not ready");
        return -1;
    }

    ret = gpio_pin_configure_dt(&button, GPIO_INPUT);
    if (ret < 0) {
        LOG_ERR("Failed to configure button");
        return -1;
    }

    ret = gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_TO_ACTIVE);
    if (ret < 0) {
        LOG_ERR("Failed to configure button interrupt");
        return -1;
    }

    gpio_init_callback(&button_cb_data, button_pressed, BIT(button.pin));
    gpio_add_callback(button.port, &button_cb_data);
    LOG_INF("Button initialized. Press to send command.");

    // --- Start OpenThread using the simple, built-in API ---
    // Because CONFIG_OPENTHREAD_FTD=y is set, this function will ensure
    // the device becomes a Leader or Router automatically.
    if (openthread_start(openthread_get_default_context()) != 0) {
        LOG_ERR("Failed to start OpenThread");
        return -1;
    }
    LOG_INF("OpenThread stack has been started.");

    otBorderRouterConfig config;
    otIp6Address prefix;
    otIp6AddressFromString("fd11:22::", &prefix);
    memset(&config, 0, sizeof(otBorderRouterConfig));
    config.mPrefix.mPrefix = prefix;
    config.mPrefix.mLength = 64;
    config.mPreferred = true;
    config.mSlaac = true;
    config.mOnMesh = true;
    config.mStable = true;
    config.mDefaultRoute = true;
    otBorderRouterAddOnMeshPrefix(openthread_get_default_instance(), &config);
    LOG_INF("Successfully configured as a Border Router.");

    while(1) {
        k_sleep(K_SECONDS(1));
    }

    return 0;
}