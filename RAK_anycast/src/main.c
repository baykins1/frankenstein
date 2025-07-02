/*
 * =================================================================
 * ==      FINAL main.c for End Node (Sending to Border Router)   ==
 * =================================================================
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>
#include <stdio.h>
#include <zephyr/drivers/gpio.h>

#include <zephyr/net/openthread.h>
#include <openthread/udp.h>
#include <openthread/message.h>
#include <openthread/instance.h>
#include <openthread/platform/radio.h>
#include <openthread/thread.h>
#include <openthread/netdata.h> // <-- NEW: Required for finding services

LOG_MODULE_REGISTER(ot_node, CONFIG_LOG_DEFAULT_LEVEL);

// --- Get the LED device spec from the devicetree ---
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

// --- Port Definitions ---
#define HELLO_PORT 1235
#define NODE_COMMAND_PORT 1236

// --- Global State & Sockets ---
static bool is_reporting = false;
static otUdpSocket command_socket;

// --- Unique ID Function (Unchanged) ---
void get_unique_id_string(char *id_buffer, size_t buffer_size)
{
    otInstance *p_ot_instance = openthread_get_default_instance();
    uint8_t mac_addr[8];
    otPlatRadioGetIeeeEui64(p_ot_instance, mac_addr);
    snprintf(id_buffer, buffer_size, "%02x%02x", mac_addr[6], mac_addr[7]);
}

// --- Hello Message Sender ---
// CHANGED: This function now robustly finds the Border Router's address
void send_hello_to_border_router(void)
{
    otError error = OT_ERROR_NONE;
    otMessage *message;
    otMessageInfo messageInfo;
    otInstance *p_ot_instance = openthread_get_default_instance();
    otUdpSocket udpSocket;

    otNetDataIterator iterator = OT_NET_DATA_ITERATOR_INIT;
    otBorderRouterConfig config;
    bool found_border_router = false;

    // Step 1: Iterate through all the services on the network
    while (otNetDataGetNextOnMeshPrefix(p_ot_instance, &iterator, &config) == OT_ERROR_NONE) {
        // Step 2: Find the service that is marked as the "Default" route
        if (config.mDefaultRoute) {
            // Step 3: We found it! Get the address of the device providing this service.
            // The config.mRloc16 is the short address of the Border Router.
            // We construct the full IPv6 address from the mesh-local prefix and the RLOC.
            const otMeshLocalPrefix *ml_prefix = otThreadGetMeshLocalPrefix(p_ot_instance);
            memcpy(&messageInfo.mPeerAddr.mFields.m8[0], ml_prefix->m8, 8);
            messageInfo.mPeerAddr.mFields.m8[8]  = 0x00;
            messageInfo.mPeerAddr.mFields.m8[9]  = 0x00;
            messageInfo.mPeerAddr.mFields.m8[10] = 0x00;
            messageInfo.mPeerAddr.mFields.m8[11] = 0xff;
            messageInfo.mPeerAddr.mFields.m8[12] = 0xfe;
            messageInfo.mPeerAddr.mFields.m8[13] = 0x00;
            messageInfo.mPeerAddr.mFields.m8[14] = (config.mRloc16 >> 8) & 0xff;
            messageInfo.mPeerAddr.mFields.m8[15] = config.mRloc16 & 0xff;
            
            found_border_router = true;
            break; // Exit the loop since we found what we need
        }
    }

    if (!found_border_router) {
        LOG_WRN("Could not find a default Border Router on the network yet.");
        return;
    }
    
    // Step 4: Send the "hello" message as before
    char unique_id[5];
    char hello_payload[20];
    get_unique_id_string(unique_id, sizeof(unique_id));
    snprintf(hello_payload, sizeof(hello_payload), "hello-%s", unique_id);

    messageInfo.mPeerPort = HELLO_PORT;

    message = otUdpNewMessage(p_ot_instance, NULL);
    if (message) {
        otMessageAppend(message, hello_payload, strlen(hello_payload));
        otUdpOpen(p_ot_instance, &udpSocket, NULL, NULL);
        otUdpSend(p_ot_instance, &udpSocket, message, &messageInfo);
        otUdpClose(p_ot_instance, &udpSocket);
    }
}


// --- Command Listener Callback (with LED logic) ---
void command_receive_callback(void *aContext, otMessage *aMessage, const otMessageInfo *aMessageInfo)
{
    OT_UNUSED_VARIABLE(aContext);
    OT_UNUSED_VARIABLE(aMessageInfo);

    char command_buffer[10];
    int length = otMessageRead(aMessage, otMessageGetOffset(aMessage), command_buffer, sizeof(command_buffer) - 1);
    command_buffer[length] = '\0';

    if (strcmp(command_buffer, "start") == 0) {
        LOG_INF("'start' command received. Beginning to report.");
        is_reporting = true;
        gpio_pin_set_dt(&led, 1); // Turn LED on
    } else if (strcmp(command_buffer, "stop") == 0) {
        LOG_INF("'stop' command received. Halting reports.");
        is_reporting = false;
        gpio_pin_set_dt(&led, 0); // Turn LED off
    }
    otMessageFree(aMessage);
}

// --- Command Listener Setup (Unchanged) ---
void setup_command_listener(void)
{
    otError error;
    otSockAddr sockaddr;
    otInstance *p_ot_instance = openthread_get_default_instance();
    memset(&sockaddr, 0, sizeof(sockaddr));
    sockaddr.mPort = NODE_COMMAND_PORT;
    error = otUdpOpen(p_ot_instance, &command_socket, command_receive_callback, NULL);
    if (error != OT_ERROR_NONE) { LOG_ERR("Failed to open command UDP socket: %d", error); return; }
    error = otUdpBind(p_ot_instance, &command_socket, &sockaddr, OT_NETIF_THREAD);
    if (error != OT_ERROR_NONE) { LOG_ERR("Failed to bind command UDP socket: %d", error); return; }
    LOG_INF("Node command listener started on port %d", NODE_COMMAND_PORT);
}

// --- Main ---
int main(void)
{
    LOG_INF("Starting OpenThread Node Application");

    // Initialize the LED
    if (!device_is_ready(led.port)) { LOG_ERR("LED device not ready"); return -1; }
    if (gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE) < 0) { LOG_ERR("Failed to configure LED pin"); return -1; }
    LOG_INF("LED initialized.");

    if (openthread_start(openthread_get_default_context()) != 0) {
        LOG_ERR("Failed to start OpenThread");
        return -1;
    }

    setup_command_listener();
    LOG_INF("Node initialized. Waiting for commands.");

    while (1) {
        if (is_reporting && (otThreadGetDeviceRole(openthread_get_default_instance()) > OT_DEVICE_ROLE_DISABLED)) {
            // CHANGED: Call the new function
            send_hello_to_border_router();
        }
        k_sleep(K_SECONDS(1));
    }
    return 0;
}