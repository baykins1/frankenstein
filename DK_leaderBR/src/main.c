/*
 * =================================================================
 * ==    FINAL and CORRECTED main.c for Leader/Border Router      ==
 * =================================================================
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include <openthread/thread_ftd.h>
#include <zephyr/net/openthread.h>
#include <openthread/udp.h>
#include <openthread/message.h>
#include <openthread/border_router.h>

LOG_MODULE_REGISTER(ot_leader, CONFIG_LOG_DEFAULT_LEVEL);

// --- Port Definitions ---
#define NODE_COMMAND_PORT 1236
#define HELLO_LISTENER_PORT 1235

// --- Global State ---
static struct openthread_state_changed_cb ot_state_changed_cb_obj; // Callback struct

// --- GPIO Definitions from Devicetree ---
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static struct gpio_callback button_cb_data;


// --- Node Command Sender ---
void send_node_command(const char *command)
{
    otError error;
    otMessage *message;
    otMessageInfo messageInfo;
    otInstance *p_ot_instance = openthread_get_default_instance();
    otUdpSocket udpSocket;

    memset(&messageInfo, 0, sizeof(messageInfo));
    otIp6AddressFromString("ff03::1", &messageInfo.mPeerAddr);
    messageInfo.mPeerPort = NODE_COMMAND_PORT;

    message = otUdpNewMessage(p_ot_instance, NULL);
    if (message) {
        otMessageAppend(message, command, strlen(command));
        otUdpOpen(p_ot_instance, &udpSocket, NULL, NULL);
        error = otUdpSend(p_ot_instance, &udpSocket, message, &messageInfo);
        if (error != OT_ERROR_NONE) {
            LOG_ERR("Failed to send command: %d", error);
        } else {
            LOG_INF("Sent command to all nodes: '%s'", command);
        }
        otUdpClose(p_ot_instance, &udpSocket);
    }
}

// --- Button Press Callback ---
void button_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    // This variable is not used in this final version, but is kept for reference
    // static bool reporting_is_active = false;
    // reporting_is_active = !reporting_is_active;

    // A button press simply toggles the LED and sends a command.
    // We assume the first press is "start", second is "stop", etc.
    if (gpio_pin_get_dt(&led) == 0) {
        send_node_command("start");
        gpio_pin_set_dt(&led, 1);
    } else {
        send_node_command("stop");
        gpio_pin_set_dt(&led, 0);
    }
}

// --- Hello Message Listener ---
static otUdpSocket hello_socket;

void hello_receive_callback(void *aContext, otMessage *aMessage, const otMessageInfo *aMessageInfo)
{
    OT_UNUSED_VARIABLE(aContext);
    char message_buffer[32];
    char sender_addr_str[40];
    int length;

    otIp6AddressToString(&aMessageInfo->mPeerAddr, sender_addr_str, sizeof(sender_addr_str));
    length = otMessageRead(aMessage, otMessageGetOffset(aMessage), message_buffer, sizeof(message_buffer) - 1);
    message_buffer[length] = '\0';
    LOG_INF("Received message '%s' from %s", message_buffer, sender_addr_str);
    otMessageFree(aMessage);
}

void setup_hello_listener(void)
{
    otError error;
    otSockAddr sockaddr;
    otInstance *p_ot_instance = openthread_get_default_instance();

    memset(&sockaddr, 0, sizeof(sockaddr));
    sockaddr.mPort = HELLO_LISTENER_PORT;

    error = otUdpOpen(p_ot_instance, &hello_socket, hello_receive_callback, NULL);
    if (error != OT_ERROR_NONE) { LOG_ERR("Failed to open hello UDP socket: %d", error); return; }

    error = otUdpBind(p_ot_instance, &hello_socket, &sockaddr, OT_NETIF_THREAD);
    if (error != OT_ERROR_NONE) { LOG_ERR("Failed to bind hello UDP socket: %d", error); return; }

    LOG_INF("Hello message listener started on port %d", HELLO_LISTENER_PORT);
}


static void on_thread_state_changed(otChangedFlags flags, struct openthread_context *ot_context)
{
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
    LOG_INF("Starting OpenThread Leader/Border Router Application");

    // --- GPIO Initializations ---
    if (!device_is_ready(button.port)) { return -1; }
    gpio_pin_configure_dt(&button, GPIO_INPUT);
    gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_TO_ACTIVE);
    gpio_init_callback(&button_cb_data, button_pressed, BIT(button.pin));
    gpio_add_callback(button.port, &button_cb_data);
    LOG_INF("Button initialized.");

    if (!device_is_ready(led.port)) { return -1; }
    gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
    LOG_INF("LED initialized.");

    // =================================================================
    // ==                      THE FIX IS HERE                        ==
    // =================================================================
    // FIX: The member name is 'state_changed_handler', not 'handler'.
    ot_state_changed_cb_obj.state_changed_handler = on_thread_state_changed;
    openthread_state_changed_cb_register(openthread_get_default_context(), &ot_state_changed_cb_obj);

    // --- Start OpenThread Network ---
    if (openthread_start(openthread_get_default_context()) != 0) {
        LOG_ERR("Failed to start OpenThread");
        return -1;
    }

    // --- CONFIGURE AS PREFERRED LEADER AND BORDER ROUTER ---
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

    // --- Keep the main thread alive ---
    while (1) {
        k_sleep(K_SECONDS(1));
    }

    return 0;
}