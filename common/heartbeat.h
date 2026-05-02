#ifndef HEARTBEAT_H
#define HEARTBEAT_H

#include <time.h>
#include <stdint.h>

/* Heartbeat configuration */
struct heartbeat_config
{
    int timeout_seconds;        /* Timeout for no data received */
    int check_interval_ms;      /* How often to check (for epoll timeout) */
    int max_reconnect_attempts; /* Max reconnection attempts, 0 = infinite */
    int reconnect_delay_ms;     /* Delay between reconnection attempts */
};

/* Heartbeat state */
struct heartbeat_state
{
    time_t last_packet_time;    /* Last time a valid packet was received */
    int timeout_seconds;
    int reconnect_attempts;
    int max_reconnect_attempts;
    int reconnect_delay_ms;
    int is_timeout;             /* Flag indicating timeout state */
};

/* Initialize heartbeat state */
void heartbeat_init(struct heartbeat_state *hb, const struct heartbeat_config *config);

/* Update heartbeat on packet received */
void heartbeat_update(struct heartbeat_state *hb);

/* Check if heartbeat has timed out */
int heartbeat_check_timeout(struct heartbeat_state *hb);

/* Reset reconnection counter after successful reconnect */
void heartbeat_reset_reconnect(struct heartbeat_state *hb);

/* Increment reconnection attempt counter */
int heartbeat_increment_reconnect(struct heartbeat_state *hb);

/* Get time since last packet in seconds */
int heartbeat_get_idle_time(const struct heartbeat_state *hb);

#endif /* HEARTBEAT_H */
