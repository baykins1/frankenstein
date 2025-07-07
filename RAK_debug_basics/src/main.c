#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <openthread/thread_ftd.h>
#include <openthread/dataset_ftd.h>
#include <zephyr/net/openthread.h>
#include <openthread/udp.h>
#include <openthread/message.h>
#include <openthread/link.h>
#include <openthread/platform/radio.h>

LOG_MODULE_REGISTER(ot_end_device, CONFIG_LOG_DEFAULT_LEVEL);

#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

#define OT_CONNECTION_LED_PORT 1234
#define HELLO_INTERVAL_MS 1000

static struct k_timer hello_timer;
static bool streaming = false;
static otUdpSocket udpSocket;

static void get_mac_suffix(char *buf, size_t buflen)
{
    otInstance *instance = openthread_get_default_instance();
    const otExtAddress *ext_addr = otLinkGetExtendedAddress(instance);
    // Print last 2 bytes (4 hex digits)
    snprintk(buf, buflen, "%02X%02X", ext_addr->m8[6], ext_addr->m8[7]);
}

static void send_hello(void)
{
    otInstance *instance = openthread_get_default_instance();
    char mac[5];
    get_mac_suffix(mac, sizeof(mac));
    char msg[32];
    snprintk(msg, sizeof(msg), "hello world %s", mac);

    otMessage *message = otUdpNewMessage(instance, NULL);
    if (!message) return;
    otMessageAppend(message, msg, strlen(msg));

    otMessageInfo msgInfo = {0};
    // Send to border router (assume leader anycast address)
    otIp6AddressFromString("ff03::1", &msgInfo.mPeerAddr);
    msgInfo.mPeerPort = OT_CONNECTION_LED_PORT;

    otUdpSend(instance, &udpSocket, message, &msgInfo);
}

static void hello_timer_handler(struct k_timer *timer_id)
{
    send_hello();
}

static void udp_receive_cb(void *aContext, otMessage *aMessage, const otMessageInfo *aMessageInfo)
{
    char buf[16];
    int len = otMessageRead(aMessage, 0, buf, sizeof(buf) - 1);
    buf[len] = 0;

    if (strcmp(buf, "start") == 0 && !streaming) {
        streaming = true;
        gpio_pin_set_dt(&led, 1);
        k_timer_start(&hello_timer, K_MSEC(HELLO_INTERVAL_MS), K_MSEC(HELLO_INTERVAL_MS));
        LOG_INF("Received start, streaming...");
    } else if (strcmp(buf, "stop") == 0 && streaming) {
        streaming = false;
        gpio_pin_set_dt(&led, 0);
        k_timer_stop(&hello_timer);
        LOG_INF("Received stop, streaming stopped.");
    }
}

static void set_thread_network_config(otInstance *instance)
{
    otOperationalDataset dataset;
    memset(&dataset, 0, sizeof(dataset));
    dataset.mComponents.mIsNetworkNamePresent = true;
    strncpy(dataset.mNetworkName.m8, "Campos", OT_NETWORK_NAME_MAX_SIZE);
    dataset.mComponents.mIsNetworkKeyPresent = true;
    uint8_t key[16] = {0x11,0x11,0x22,0x22,0x33,0x33,0x44,0x44,0x55,0x55,0x66,0x66,0x77,0x77,0x88,0x88};
    memcpy(dataset.mNetworkKey.m8, key, 16);
    dataset.mComponents.mIsPanIdPresent = true;
    dataset.mPanId = 0xABCD;
    dataset.mComponents.mIsChannelPresent = true;
    dataset.mChannel = 15;
    otDatasetSetActive(instance, &dataset);
}

int main(void)
{
    int ret;
    LOG_INF("Starting OpenThread End Device");

    if (!device_is_ready(led.port)) {
        LOG_ERR("LED device not ready");
        return -1;
    }
    gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);

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
    LOG_INF("OpenThread stack started.");

    // Open UDP socket for multicast commands
    otSockAddr listen_addr = {0};
    otIp6AddressFromString("ff03::1", &listen_addr.mAddress);
    listen_addr.mPort = OT_CONNECTION_LED_PORT;
    otUdpOpen(instance, &udpSocket, udp_receive_cb, NULL);
    otUdpBind(instance, &udpSocket, &listen_addr, OT_NETIF_THREAD);

    k_timer_init(&hello_timer, hello_timer_handler, NULL);

    LOG_INF("End device ready.");
    return 0;
}