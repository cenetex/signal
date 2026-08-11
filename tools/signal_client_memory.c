#include "client.h"
#include "client_memory_budget.h"
#include "net_protocol.h"

#include <stdio.h>

int main(void) {
    const size_t world_bytes = sizeof(world_t);
    const size_t game_bytes = sizeof(game_t);
    const size_t local_server_bytes = sizeof(local_server_t);
    const size_t asteroid_interp_bytes =
        sizeof(((game_t *)0)->asteroid_interp);
    const size_t npc_interp_bytes = sizeof(((game_t *)0)->npc_interp);
    const size_t scaffold_interp_bytes =
        sizeof(((game_t *)0)->scaffold_interp);
    const size_t cargo_pod_interp_bytes =
        sizeof(((game_t *)0)->cargo_pod_interp);
    const size_t player_interp_bytes =
        sizeof(((game_t *)0)->player_interp);
    const size_t local_snapshot_scratch_bytes =
        sizeof(server_world_snapshot_scratch_t) +
        sizeof(server_private_snapshot_scratch_t) +
        sizeof(server_station_snapshot_scratch_t);

    printf("world_t=%zu budget=%u\n",
           world_bytes, SIGNAL_WORLD_SIZE_BUDGET_BYTES);
    printf("game_t=%zu budget=%u\n",
           game_bytes, SIGNAL_GAME_SIZE_BUDGET_BYTES);
    printf("local_server_t=%zu\n", local_server_bytes);
    printf("retained_interpolation_bytes="
           "asteroid:%zu,npc:%zu,scaffold:%zu,cargo_pod:%zu,player:%zu\n",
           asteroid_interp_bytes, npc_interp_bytes, scaffold_interp_bytes,
           cargo_pod_interp_bytes, player_interp_bytes);
    printf("local_snapshot_scratch_bytes=%zu\n",
           local_snapshot_scratch_bytes);

    if (world_bytes > SIGNAL_WORLD_SIZE_BUDGET_BYTES) return 1;
    if (game_bytes > SIGNAL_GAME_SIZE_BUDGET_BYTES) return 1;
    return 0;
}
