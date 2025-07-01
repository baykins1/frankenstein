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

LOG_MODULE_REGISTER(ot_controller, CONFIG_LOG_DEFAULT_LEVEL);

/* OpenThread networking definitions */
#define OT_CONNECTION_LED_PORT 1234
static const char *light_command = "toggle";

/* GPIO definitions for the button */
#define SW0_NODE DT_ALIAS(sw0)
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(SW0_NODE, gpios);
static struct gpio_callback button_cb_data;

/* Forward declaration of the UDP send function */
void send_light_control_command(void);

/* Button press callback function */
void button_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    LOG_INF("Button pressed, sending command.");
    send_light_control_command();
}

/* OpenThread UDP sending logic */
void send_light_control_command(void)
{
    otError error = OT_ERROR_NONE;
    otMessage *message;
    otMessageInfo messageInfo;
    otInstance *p_ot_instance = openthread_get_default_instance();
    otUdpSocket udpSocket;

    // Set up the destination address information
    memset(&messageInfo, 0, sizeof(messageInfo));
    // Use the Realm-Local All-Nodes multicast address.
    otIp6AddressFromString("ff03::1", &messageInfo.mPeerAddr);
    messageInfo.mPeerPort = OT_CONNECTION_LED_PORT;

    // Create a new message
    message = otUdpNewMessage(p_ot_instance, NULL);
    if (message == NULL) {
        LOG_ERR("Failed to allocate new UDP message.");
        return;
    }

    // Append the command string to the message payload
    error = otMessageAppend(message, light_command, strlen(light_command));
    if (error != OT_ERROR_NONE) {
        LOG_ERR("Failed to append to UDP message: %d", error);
        otMessageFree(message);
        return;
    }

    // Open a temporary UDP socket for sending this message
    error = otUdpOpen(p_ot_instance, &udpSocket, NULL, NULL);
    if (error != OT_ERROR_NONE) {
        LOG_ERR("Failed to open UDP socket: %d", error);
        otMessageFree(message);
        return;
    }

    // --- THIS IS THE CORRECTED LINE ---
    error = otUdpSend(p_ot_instance, &udpSocket, message, &messageInfo);

    if (error != OT_ERROR_NONE) {
        LOG_ERR("Failed to send UDP message: %d", error);
        // On error, otUdpSend frees the message.
    } else {
        LOG_INF("UDP message sent successfully!");
    }

    // Close the socket after sending
    otUdpClose(p_ot_instance, &udpSocket);
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


    LOG_INF("Starting OpenThread stack...");
    if (openthread_start(openthread_get_default_context()) != 0) {
        LOG_ERR("Failed to start OpenThread");
        return -1;
    }
    LOG_INF("OpenThread stack started.");


    while (1) {
        k_msleep(1000); // Sleep for 1 second to save power.
    }


    return 0;
}