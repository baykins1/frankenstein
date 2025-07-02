/*
 * =================================================================
 * == COMPLETE and CORRECTED main.c for Node (RAK/Argon)          ==
 * =================================================================
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>
#include <stdio.h> // Needed for snprintf

#include <zephyr/net/openthread.h>
#include <openthread/udp.h>
#include <openthread/message.h>
#include <openthread/instance.h>
#include <openthread/platform/radio.h> // Needed for otPlatRadioGetIeeeEui64
#include <openthread/thread.h>         // <-- FIX: Added missing header for thread APIs

LOG_MODULE_REGISTER(ot_node, CONFIG_LOG_DEFAULT_LEVEL);

// --- Port Definitions ---
#define HELLO_PORT 1235
#define NODE_COMMAND_PORT 1236

// --- Global State ---
static bool is_reporting = false;

// --- UDP Sockets ---
static otUdpSocket command_socket;

// --- Unique ID Function ---
void get_unique_id_string(char *id_buffer, size_t buffer_size)
{
    otInstance *p_ot_instance = openthread_get_default_instance();
    uint8_t mac_addr[8]; // <-- FIX: Create a buffer to hold the MAC address

    // FIX: Correctly call the function by passing the buffer
    otPlatRadioGetIeeeEui64(p_ot_instance, mac_addr);

    // Get the last 2 bytes (4 hex characters) of the MAC address
    snprintf(id_buffer, buffer_size, "%02x%02x", mac_addr[6], mac_addr[7]);
}

// --- Hello Message Sender ---
void send_hello_to_leader(void)
{
    otError error;
    otMessage *message;
    otMessageInfo messageInfo;
    otInstance *p_ot_instance = openthread_get_default_instance();
    otUdpSocket udpSocket;

    char unique_id[5];
    char hello_payload[20];

    get_unique_id_string(unique_id, sizeof(unique_id));
    snprintf(hello_payload, sizeof(hello_payload), "hello-%s", unique_id);

    memset(&messageInfo, 0, sizeof(messageInfo));
    
    // Get the Leader's Anycast address from the Thread network data
    error = otThreadGetLeaderRloc(p_ot_instance, &messageInfo.mPeerAddr);
    if (error != OT_ERROR_NONE) {
        LOG_ERR("Cannot get Leader address, is network formed?");
        return;
    }
    messageInfo.mPeerPort = HELLO_PORT;

    message = otUdpNewMessage(p_ot_instance, NULL);
    if (message) {
        otMessageAppend(message, hello_payload, strlen(hello_payload));
        otUdpOpen(p_ot_instance, &udpSocket, NULL, NULL);
        otUdpSend(p_ot_instance, &udpSocket, message, &messageInfo);
        otUdpClose(p_ot_instance, &udpSocket);
    }
}

// --- Command Listener Callback ---
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
    } else if (strcmp(command_buffer, "stop") == 0) {
        LOG_INF("'stop' command received. Halting reports.");
        is_reporting = false;
    }
    otMessageFree(aMessage);
}

// --- Command Listener Setup ---
void setup_command_listener(void)
{
    otError error;
    otSockAddr sockaddr;
    otInstance *p_ot_instance = openthread_get_default_instance();

    memset(&sockaddr, 0, sizeof(sockaddr));
    sockaddr.mPort = NODE_COMMAND_PORT;

    error = otUdpOpen(p_ot_instance, &command_socket, command_receive_callback, NULL);
    if (error != OT_ERROR_NONE) {
        LOG_ERR("Failed to open command UDP socket: %d", error);
        return;
    }

    error = otUdpBind(p_ot_instance, &command_socket, &sockaddr, OT_NETIF_THREAD);
     if (error != OT_ERROR_NONE) {
        LOG_ERR("Failed to bind command UDP socket: %d", error);
        return;
    }
    LOG_INF("Node command listener started on port %d", NODE_COMMAND_PORT);
}

// --- Main ---
int main(void)
{
    LOG_INF("Starting OpenThread Node Application");

    if (openthread_start(openthread_get_default_context()) != 0) {
        LOG_ERR("Failed to start OpenThread");
        return -1;
    }

    setup_command_listener();
    LOG_INF("Node initialized. Waiting for commands.");

    while (1) {
        // Only send if we are told to and are fully connected to the network
        if (is_reporting && (otThreadGetDeviceRole(openthread_get_default_instance()) > OT_DEVICE_ROLE_DISABLED)) {
            send_hello_to_leader();
        }
        k_sleep(K_SECONDS(1));
    }
    return 0; // Should not be reached
}