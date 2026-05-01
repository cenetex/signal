/*
 * test_registry.c — Layer A.2 of #479: pubkey registry on the server.
 *
 * Verifies that NET_MSG_REGISTER_PUBKEY-style registration binds a
 * pubkey to the current session_token, that re-registration with a
 * rotated token is treated as a reconnect and rebinds, and that the
 * registry survives a world save / load roundtrip.
 *
 * Identity at the wire level is still the 8-byte session_token; this
 * layer is registration only, not authentication. Signing inputs is
 * Layer A.3.
 */
#include "tests/test_harness.h"

#include <string.h>

static void fill_pubkey(uint8_t pk[32], uint8_t seed) {
    for (int i = 0; i < 32; i++) pk[i] = (uint8_t)(seed + i);
}

static void fill_token(uint8_t tok[8], uint8_t seed) {
    for (int i = 0; i < 8; i++) tok[i] = (uint8_t)(seed * 13 + i);
}

/* Helper: simulate the registration sequence — set a session_token on
 * a slot, mark session_ready, then bind pubkey via the registry API. */
static void setup_registered_player(world_t *w, int slot,
                                    const uint8_t pubkey[32],
                                    const uint8_t token[8]) {
    server_player_t *sp = &w->players[slot];
    sp->connected = true;
    sp->id = (uint8_t)slot;
    memcpy(sp->session_token, token, 8);
    sp->session_ready = true;
    memcpy(sp->pubkey, pubkey, 32);
    sp->pubkey_set = true;
    ASSERT(registry_register_pubkey(w, pubkey, token));
}

TEST(test_registry_fresh_registration) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);

    uint8_t pk[32];  fill_pubkey(pk, 1);
    uint8_t tok[8];  fill_token(tok, 7);

    /* Empty registry: lookup misses. */
    ASSERT_EQ_INT(registry_lookup_by_pubkey(w, pk), -1);

    setup_registered_player(w, 3, pk, tok);

    /* Lookup returns the player_idx we registered. */
    ASSERT_EQ_INT(registry_lookup_by_pubkey(w, pk), 3);
}

TEST(test_registry_idempotent_reregistration) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);

    uint8_t pk[32];  fill_pubkey(pk, 2);
    uint8_t tok[8];  fill_token(tok, 11);

    setup_registered_player(w, 1, pk, tok);
    /* Same (pubkey, token) again — must return true and produce no
     * duplicate registry entry. */
    ASSERT(registry_register_pubkey(w, pk, tok));

    int seen = 0;
    for (int r = 0; r < MAX_PLAYERS; r++) {
        if (!w->pubkey_registry[r].in_use) continue;
        if (memcmp(w->pubkey_registry[r].pubkey, pk, 32) == 0) seen++;
    }
    ASSERT_EQ_INT(seen, 1);
    ASSERT_EQ_INT(registry_lookup_by_pubkey(w, pk), 1);
}

TEST(test_registry_reconnect_with_new_token) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);

    uint8_t pk[32];  fill_pubkey(pk, 3);
    uint8_t tok1[8]; fill_token(tok1, 5);
    uint8_t tok2[8]; fill_token(tok2, 99);

    /* First connection: pubkey P @ slot 0 with token T1, plus a state
     * marker we'll check survives the rebinding, plus a station ledger
     * entry keyed by T1 to verify ledger migration. */
    setup_registered_player(w, 0, pk, tok1);
    w->players[0].ship.stat_credits_earned = 4242.0f;
    station_t *st = &w->stations[0];
    memcpy(st->ledger[st->ledger_count].player_pubkey, tok1, 8);
    st->ledger[st->ledger_count].balance = 1234.0f;
    st->ledger[st->ledger_count].lifetime_supply = 0.0f;
    st->ledger_count++;

    /* Server-side reconnect logic: the new connection arrives on a
     * fresh slot with a NEW session_token T2. The handler ship-copies
     * the old slot's persistent state, migrates ledger entries from
     * T1 → T2 across stations, frees the old slot, and rebinds the
     * registry to T2. */
    server_player_t *new_slot = &w->players[5];
    new_slot->connected = true;
    new_slot->id = 5;
    memcpy(new_slot->session_token, tok2, 8);
    new_slot->session_ready = true;
    /* Carry the persistent state. */
    new_slot->ship.stat_credits_earned = w->players[0].ship.stat_credits_earned;
    /* Migrate ledger entries from T1 → T2. */
    for (int e = 0; e < st->ledger_count; e++) {
        if (memcmp(st->ledger[e].player_pubkey, tok1, 8) == 0)
            memcpy(st->ledger[e].player_pubkey, tok2, 8);
    }
    /* Tear down old slot, as the handler does. */
    w->players[0].connected = false;
    w->players[0].session_ready = false;
    memset(w->players[0].session_token, 0, 8);
    /* Now rebind via REGISTER_PUBKEY. */
    memcpy(new_slot->pubkey, pk, 32);
    new_slot->pubkey_set = true;
    ASSERT(registry_register_pubkey(w, pk, tok2));

    /* Lookup now resolves to the new slot. */
    ASSERT_EQ_INT(registry_lookup_by_pubkey(w, pk), 5);
    /* State marker survives. */
    ASSERT_EQ_FLOAT(w->players[5].ship.stat_credits_earned, 4242.0f, 0.01f);

    /* Old token T1 is no longer claimed by any live, ready slot. */
    bool old_alive = false;
    for (int p = 0; p < MAX_PLAYERS; p++) {
        if (!w->players[p].session_ready) continue;
        if (memcmp(w->players[p].session_token, tok1, 8) == 0) {
            old_alive = true; break;
        }
    }
    ASSERT(!old_alive);

    /* Ledger entry migrated: balance is spendable under the new token. */
    bool found_balance = false;
    for (int e = 0; e < st->ledger_count; e++) {
        if (memcmp(st->ledger[e].player_pubkey, tok2, 8) == 0 &&
            st->ledger[e].balance > 1000.0f) {
            found_balance = true; break;
        }
    }
    ASSERT(found_balance);
}

TEST(test_registry_two_pubkeys_one_machine) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);

    uint8_t p1[32]; fill_pubkey(p1, 10);
    uint8_t p2[32]; fill_pubkey(p2, 200);
    uint8_t t1[8];  fill_token(t1, 1);
    uint8_t t2[8];  fill_token(t2, 2);

    setup_registered_player(w, 0, p1, t1);
    setup_registered_player(w, 1, p2, t2);

    ASSERT_EQ_INT(registry_lookup_by_pubkey(w, p1), 0);
    ASSERT_EQ_INT(registry_lookup_by_pubkey(w, p2), 1);
}

TEST(test_registry_save_load_roundtrip) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);

    uint8_t pk[32]; fill_pubkey(pk, 42);
    uint8_t tok[8]; fill_token(tok, 17);

    setup_registered_player(w, 2, pk, tok);
    ASSERT_EQ_INT(registry_lookup_by_pubkey(w, pk), 2);

    ASSERT(station_catalog_save_all(w->stations, MAX_STATIONS, TMP("test_regcat")));
    ASSERT(world_save(w, TMP("test_registry.sav")));

    WORLD_HEAP loaded = calloc(1, sizeof(world_t));
    ASSERT(loaded != NULL);
    station_catalog_load_all(loaded->stations, MAX_STATIONS, TMP("test_regcat"));
    ASSERT(world_load(loaded, TMP("test_registry.sav")));

    /* Players are cleared on load (they reconnect), so the lookup
     * traversal won't find a live slot — but the persisted registry
     * binding must still be present. After re-establishing a player
     * slot with the persisted token, lookup must resolve again. */
    bool found_persisted = false;
    for (int r = 0; r < MAX_PLAYERS; r++) {
        if (!loaded->pubkey_registry[r].in_use) continue;
        if (memcmp(loaded->pubkey_registry[r].pubkey, pk, 32) == 0 &&
            memcmp(loaded->pubkey_registry[r].session_token, tok, 8) == 0) {
            found_persisted = true; break;
        }
    }
    ASSERT(found_persisted);

    /* Re-attach a player slot to the persisted token; lookup resolves. */
    server_player_t *sp = &loaded->players[4];
    sp->connected = true;
    sp->id = 4;
    memcpy(sp->session_token, tok, 8);
    sp->session_ready = true;
    ASSERT_EQ_INT(registry_lookup_by_pubkey(loaded, pk), 4);

    remove(TMP("test_registry.sav"));
}

void register_registry_tests(void) {
    TEST_SECTION("\nPubkey registry (#479 A.2):\n");
    RUN(test_registry_fresh_registration);
    RUN(test_registry_idempotent_reregistration);
    RUN(test_registry_reconnect_with_new_token);
    RUN(test_registry_two_pubkeys_one_machine);
    RUN(test_registry_save_load_roundtrip);
}
