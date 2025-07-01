/*
 * Light Application Main
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include <zephyr/net/openthread.h>
#include <openthread/udp.h>
#include <openthread/message.h>

LOG_MODULE_REGISTER(ot_light, CONFIG_LOG_DEFAULT_LEVEL);

/* OpenThread networking definitions */
#define OT_CONNECTION_LED_PORT 1234
static otUdpSocket light_socket;

/* GPIO definition for the LED */
#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);


/* UDP Receive Callback function */
void udp_receive_callback(void *aContext, otMessage *aMessage, const otMessageInfo *aMessageInfo)
{
    OT_UNUSED_VARIABLE(aContext);
    OT_UNUSED_VARIABLE(aMessageInfo);

    char command[32];
    int length;

    // Read the message payload into the buffer
    length = otMessageRead(aMessage, otMessageGetOffset(aMessage), command, sizeof(command) - 1);
    command[length] = '\0'; // Null-terminate the string

    LOG_INF("Received UDP message: %s", command);

    // Check if the command is "toggle"
    if (strcmp(command, "toggle") == 0) {
        LOG_INF("Toggle command received, toggling LED.");
        gpio_pin_toggle_dt(&led);
    }

    // Free the message buffer
    otMessageFree(aMessage);
}


int main(void)
{
    int ret;
    otError error;
    otSockAddr sockaddr;
    otInstance *p_ot_instance = openthread_get_default_instance();

    LOG_INF("Starting OpenThread Light Application");

    /* --- GPIO LED Initialization --- */
    if (!device_is_ready(led.port)) {
        LOG_ERR("LED device not ready");
        return -1;
    }

    ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
    if (ret < 0) {
        LOG_ERR("Failed to configure LED");
        return -1;
    }
    LOG_INF("LED initialized.");


    /* --- OpenThread UDP Initialization --- */
    memset(&sockaddr, 0, sizeof(sockaddr));
    sockaddr.mPort = OT_CONNECTION_LED_PORT; // Set the port to listen on

    // Open the UDP socket
    error = otUdpOpen(p_ot_instance, &light_socket, udp_receive_callback, NULL);
    if (error != OT_ERROR_NONE) {
        LOG_ERR("Failed to open UDP socket: %d", error);
        return -1;
    }

    // Bind the socket to the specified port
    error = otUdpBind(p_ot_instance, &light_socket, &sockaddr, OT_NETIF_THREAD);
    if (error != OT_ERROR_NONE) {
        LOG_ERR("Failed to bind UDP socket: %d", error);
        return -1;
    }

    LOG_INF("UDP listener started on port %d", OT_CONNECTION_LED_PORT);

    return 0;
}