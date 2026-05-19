#ifndef SERVER_HANDOFF_FLOW_H
#define SERVER_HANDOFF_FLOW_H

#include <stdbool.h>
#include <stdint.h>

#include "game_sim.h"
#include "handoff_ticket.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HANDOFF_TICKET_DEFAULT_TTL_TICKS 7200u /* 60 seconds at 120Hz */
#define HANDOFF_CONSUMED_TICKET_CAP 128

typedef enum {
    HANDOFF_FLOW_OK = 0,
    HANDOFF_FLOW_REJECT_BAD_ARGS,
    HANDOFF_FLOW_REJECT_NO_PLAYER_KEY,
    HANDOFF_FLOW_REJECT_SOURCE,
    HANDOFF_FLOW_REJECT_DEST,
    HANDOFF_FLOW_REJECT_ISSUE,
    HANDOFF_FLOW_REJECT_VERIFY,
    HANDOFF_FLOW_REJECT_REPLAY,
    HANDOFF_FLOW_REJECT_HYDRATE
} handoff_flow_result_t;

const char *handoff_flow_result_name(handoff_flow_result_t result);

bool handoff_issue_ticket_to_station(world_t *w, int player_idx,
                                     int source_station_idx,
                                     int dest_station_idx,
                                     uint64_t ttl_ticks,
                                     handoff_ticket_t *out);

handoff_flow_result_t handoff_accept_presented_ship(world_t *w, int player_idx,
                                                    const handoff_ticket_t *ticket,
                                                    const ship_t *presented_ship,
                                                    int *out_dest_station);

#ifdef __cplusplus
}
#endif

#endif /* SERVER_HANDOFF_FLOW_H */
