/*
 * =================================================================
 * ==          FINAL, VERIFIED Border Router Main                 ==
 * =================================================================
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

// --- NEW: Required for USB CDC ACM console ---
#include <zephyr/drivers/uart.h>
#include <zephyr/usb/usb_device.h>

#include <zephyr/net/openthread.h>
#include <openthread/udp.h>
#include <openthread/message.h>
#include <openthread/border_router.h>
#include <openthread/thread_ftd.h>

LOG_MODULE_REGISTER(ot_border_router, CONFIG_LOG_DEFAULT_LEVEL);

// --- Network Port Definitions ---
#define NODE_COMMAND_PORT   1236
#define HELLO_LISTENER_PORT 1235

// --- State management ---
static bool reporting_is_active = false;
static struct openthread_state_changed_cb ot_state_changed_cb_obj;

// --- GPIO device definitions ---
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static struct gpio_callback button_cb_data;

// --- UDP listener ---
static otUdpSocket hello_socket;

void hello_receive_callback(void *aContext, otMessage *aMessage, const otMessageInfo *aMessageInfo) {
    OT_UNUSED_VARIABLE(aContext);
    char message_buffer[32];
    char sender_addr_str[40];
    int length = otMessageRead(aMessage, otMessageGetOffset(aMessage), message_buffer, sizeof(message_buffer) - 1);
    message_buffer[length] = '\0';
    otIp6AddressToString(&aMessageInfo->mPeerAddr, sender_addr_str, sizeof(sender_addr_str));
    LOG_INF("Received data: '%s' from node %s", message_buffer, sender_addr_str);
    otMessageFree(aMessage);
}

void setup_hello_listener(void) {
    otError error;
    otSockAddr sockaddr;
    otInstance *p_ot_instance = openthread_get_default_instance();
    memset(&sockaddr, 0, sizeof(sockaddr));
    sockaddr.mPort = HELLO_LISTENER_PORT;
    error = otUdpOpen(p_ot_instance, &hello_socket, hello_receive_callback, NULL);
    if (error != OT_ERROR_NONE) { LOG_ERR("Failed to open hello UDP socket: %d", error); return; }
    error = otUdpBind(p_ot_instance, &hello_socket, &sockaddr, OT_NETIF_THREAD);
    if (error != OT_ERROR_NONE) { LOG_ERR("Failed to bind hello UDP socket: %d", error); return; }
    LOG_INF("Hello message listener started successfully on port %d", HELLO_LISTENER_PORT);
}

// --- UDP command sender ---
void send_node_command(const char *command) {
    otError error;
    otMessage *message;
    otMessageInfo messageInfo;
    otInstance *p_ot_instance = openthread_get_default_instance();
    otUdpSocket udpSocket;
    memset(&messageInfo, 0, sizeof(messageInfo));
    otIp6AddressFromString("ff03::1", &messageInfo.mPeerAddr);
    messageInfo.mPeerPort = NODE_COMMAND_PORT;
    message = otUdpNewMessage(p_ot_instance, NULL);
    if (message == NULL) { LOG_ERR("Failed to allocate new UDP message."); return; }
    error = otMessageAppend(message, command, strlen(command));
    if (error != OT_ERROR_NONE) { otMessageFree(message); return; }
    error = otUdpOpen(p_ot_instance, &udpSocket, NULL, NULL);
    if (error != OT_ERROR_NONE) { otMessageFree(message); return; }
    error = otUdpSend(p_ot_instance, &udpSocket, message, &messageInfo);
    if (error != OT_ERROR_NONE) {
        LOG_ERR("Failed to send UDP command: %d", error);
    } else {
        LOG_INF("Command '%s' sent to all nodes.", command);
    }
    otUdpClose(p_ot_instance, &udpSocket);
}

// --- Button press callback ---
void button_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins) {
    reporting_is_active = !reporting_is_active;
    if (reporting_is_active) {
        LOG_INF("Button pressed: STARTING data collection.");
        gpio_pin_set_dt(&led, 1);
        send_node_command("start");
    } else {
        LOG_INF("Button pressed: STOPPING data collection.");
        gpio_pin_set_dt(&led, 0);
        send_node_command("stop");
    }
}

// --- OpenThread state change callback ---
static void on_thread_state_changed(otChangedFlags flags, struct openthread_context *ot_context) {
    if (flags & OT_CHANGED_THREAD_ROLE) {
        otDeviceRole role = otThreadGetDeviceRole(ot_context->instance);
        if ((role == OT_DEVICE_ROLE_LEADER) || (role == OT_DEVICE_ROLE_ROUTER)) {
            LOG_INF("Network is UP! Device role: %d. Initializing UDP listener.", role);
            setup_hello_listener();
        }
    }
}

int main(void)
{
    // =================================================================
    // ==               THE FINAL, DEFINITIVE FIX                   ==
    // =================================================================
    // This logic is taken from the working Nordic CLI sample.
    // It ensures that the main application logic does not start until
    // the USB CDC ACM (Virtual COM Port) is fully connected to the host PC.
    // This solves the "silent" CLI problem.
    int ret;
	const struct device *cons = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
	uint32_t dtr = 0;

	if (usb_enable(NULL) != 0) {
		LOG_ERR("Failed to enable USB");
		return 0;
	}

	/* Wait for DTR flag to be set before proceeding */
	while (!dtr) {
		uart_line_ctrl_get(cons, UART_LINE_CTRL_DTR, &dtr);
		k_sleep(K_MSEC(100));
	}
    // =================================================================

    LOG_INF("Starting Border Router Application");

    // --- Initialize GPIO ---
    if (!device_is_ready(led.port)) { LOG_ERR("LED device not ready"); return -1; }
    if (gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE) < 0) { LOG_ERR("Failed to configure LED"); return -1; }

    if (!device_is_ready(button.port)) { LOG_ERR("Button device not ready"); return -1; }
    if (gpio_pin_configure_dt(&button, GPIO_INPUT) < 0) { LOG_ERR("Failed to configure button"); return -1; }
    if (gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_TO_ACTIVE) < 0) { LOG_ERR("Failed to configure button interrupt"); return -1; }
    
    gpio_init_callback(&button_cb_data, button_pressed, BIT(button.pin));
    gpio_add_callback(button.port, &button_cb_data);
    LOG_INF("Button and LED initialized.");

    // --- Register state change handler ---
    // The simplified API is correct for this SDK version.
    openthread_state_changed_cb_register(openthread_get_default_context(), on_thread_state_changed);

    // --- Start OpenThread ---
    if (openthread_start(openthread_get_default_context()) != 0) {
        LOG_ERR("Failed to start OpenThread");
        return -1;
    }

    // --- Configure Border Router Prefix ---
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
    LOG_INF("Border Router configured. Waiting for network to form...");

    // --- Main Loop ---
    while(1) {
        k_sleep(K_SECONDS(5));
    }

    return 0; // Unreachable
}