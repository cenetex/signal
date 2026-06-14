#ifndef NPC_IDENTITY_H
#define NPC_IDENTITY_H

#include <stdint.h>

#include "sha256.h"

static inline void npc_custody_pubkey_from_fields(const uint8_t session_token[8],
                                                  int npc_slot,
                                                  uint8_t role,
                                                  uint8_t home_station,
                                                  uint8_t out[32]) {
    static const uint8_t domain[] = {
        'S','I','G','N','A','L','-','N','P','C','-','C','U','S','T','O','D','Y','-','v','1'
    };
    sha256_ctx_t ctx;
    uint8_t slot_le[2];

    slot_le[0] = (uint8_t)(npc_slot & 0xFF);
    slot_le[1] = (uint8_t)((npc_slot >> 8) & 0xFF);

    sha256_init(&ctx);
    sha256_update(&ctx, domain, sizeof(domain));
    if (session_token) sha256_update(&ctx, session_token, 8);
    sha256_update(&ctx, slot_le, sizeof(slot_le));
    sha256_update(&ctx, &role, sizeof(role));
    sha256_update(&ctx, &home_station, sizeof(home_station));
    sha256_final(&ctx, out);
}

#endif /* NPC_IDENTITY_H */
