/*
 * =================================================================
 * ==         STEP 1 (Final Test): Zero-Logic main.c            ==
 * =================================================================
 */
#include <zephyr/kernel.h>

// NO LOGGING INCLUDES OR REGISTRATION

int main(void)
{
    // DO ABSOLUTELY NOTHING.
    // The shell is started automatically by CONFIG_SHELL_AUTOSTART.
    // We just need to keep the main thread alive.

    while (1) {
        k_sleep(K_SECONDS(2));
    }

    return 0;
}