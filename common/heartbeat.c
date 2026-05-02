#include "heartbeat.h"
#include <string.h>

void heartbeat_init(struct heartbeat_state *hb, const struct heartbeat_config *config)
{
    memset(hb, 0, sizeof(*hb));
    hb->last_packet_time = time(NULL);
    hb->timeout_seconds = config->timeout_seconds;
    hb->max_reconnect_attempts = config->max_reconnect_attempts;
    hb->reconnect_delay_ms = config->reconnect_delay_ms;
    hb->reconnect_attempts = 0;
    hb->is_timeout = 0;
}

void heartbeat_update(struct heartbeat_state *hb)
{
    hb->last_packet_time = time(NULL);

    /* If we were in timeout state and now received data, we've recovered */
    if (hb->is_timeout)
    {
        hb->is_timeout = 0;
        hb->reconnect_attempts = 0;
    }
}

int heartbeat_check_timeout(struct heartbeat_state *hb)
{
    time_t now = time(NULL);
    int idle_time = (int)(now - hb->last_packet_time);

    if (idle_time >= hb->timeout_seconds)
    {
        if (!hb->is_timeout)
        {
            hb->is_timeout = 1;
            return 1; /* First timeout detection */
        }
        return 0; /* Already in timeout state */
    }

    return 0;
}

void heartbeat_reset_reconnect(struct heartbeat_state *hb)
{
    hb->reconnect_attempts = 0;
    hb->is_timeout = 0;
}

int heartbeat_increment_reconnect(struct heartbeat_state *hb)
{
    hb->reconnect_attempts++;

    /* Check if we've exceeded max attempts (0 means infinite) */
    if (hb->max_reconnect_attempts > 0 &&
        hb->reconnect_attempts >= hb->max_reconnect_attempts)
    {
        return -1; /* Max attempts reached */
    }

    return 0;
}

int heartbeat_get_idle_time(const struct heartbeat_state *hb)
{
    time_t now = time(NULL);
    return (int)(now - hb->last_packet_time);
}
