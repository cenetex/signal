/* Private bridge used by the same-host wallet service. */
static bool save_persistent_state(void);

static bool fly_shop_hex(const char *text, uint8_t *out, size_t bytes) {
    if (!text || strlen(text) != bytes * 2) return false;
    for (size_t i = 0; i < bytes; i++) {
        unsigned value = 0;
        for (size_t j = 0; j < 2; j++) {
            char ch = text[i * 2 + j];
            if (ch >= '0' && ch <= '9') value = value * 16 + (unsigned)(ch - '0');
            else if (ch >= 'a' && ch <= 'f') value = value * 16 + (unsigned)(ch - 'a' + 10);
            else return false;
        }
        out[i] = (uint8_t)value;
    }
    return true;
}

static bool fly_shop_auth(struct mg_http_message *hm) {
    const char *key = getenv("SIGNAL_FLY_SHOP_KEY");
    struct mg_str *header = mg_http_get_header(hm, "X-Fly-Shop-Key");
    if (!key || strlen(key) != 64 || !header || header->len != 64) return false;
    unsigned diff = 0;
    for (size_t i = 0; i < 64; i++) diff |= (unsigned char)key[i] ^ (unsigned char)header->buf[i];
    return diff == 0;
}

static void handle_fly_shop(struct mg_connection *c, struct mg_http_message *hm) {
    if (!fly_shop_auth(hm)) {
        mg_http_reply(c, 401, api_headers, "{\"error\":\"unauthorized\"}"); return;
    }
    if (!mg_match(hm->method, mg_str("POST"), NULL) || hm->body.len > 2048) {
        mg_http_reply(c, 400, api_headers, "{\"error\":\"invalid request\"}"); return;
    }
    char *wallet_text = mg_json_get_str(hm->body, "$.wallet");
    uint8_t wallet[32] = {0};
    bool wallet_ok = fly_shop_hex(wallet_text, wallet, 32);
    free(wallet_text);
    if (!wallet_ok) { mg_http_reply(c, 400, api_headers, "{\"error\":\"invalid wallet\"}"); return; }
    bool reserve = mg_match(hm->uri, mg_str("/internal/v1/fly-shop/reserve"), NULL);
    if (reserve || mg_match(hm->uri, mg_str("/internal/v1/fly-shop/grant"), NULL)) {
        char *id_text = mg_json_get_str(hm->body, "$.id");
        char *sig_text = mg_json_get_str(hm->body, "$.signature");
        uint8_t id[32] = {0}, signature[64] = {0};
        double station = -1;
        bool valid = fly_shop_hex(id_text, id, 32) && (reserve || fly_shop_hex(sig_text, signature, 64)) &&
            mg_json_get_num(hm->body, "$.station", &station) &&
            station >= 0 && station <= 2 && station == floor(station);
        free(id_text); free(sig_text);
        if (!valid || !signal_connectome_enabled()) {
            mg_http_reply(c, 400, api_headers, "{\"error\":\"invalid grant\"}"); return;
        }
        /* Finish an older background save before publishing this purchase. */
        wait_for_persistence_writer_result(mg_millis());
        const fly_purchase_t *p = reserve
            ? world_fly_purchase_reserve(&world, id, wallet, (int)station)
            : world_fly_purchase_grant(&world, id, wallet, signature, (int)station);
        if (!p) { mg_http_reply(c, 409, api_headers, "{\"error\":\"worker grant unavailable\"}"); return; }
        if (!save_persistent_state()) {
            mg_http_reply(c, 503, api_headers, "{\"error\":\"purchase save pending\"}"); return;
        }
        mg_http_reply(c, 200, api_headers, "{\"ok\":true,\"assetId\":%u}", p->asset_id);
        return;
    }
    if (!mg_match(hm->uri, mg_str("/internal/v1/fly-shop/view"), NULL)) {
        mg_http_reply(c, 404, api_headers, "{\"error\":\"unknown route\"}"); return;
    }
    enum { CAP = 131072 };
    char *out = calloc(CAP, 1);
    if (!out) { mg_http_reply(c, 503, api_headers, "{}"); return; }
    size_t pos = 0;
    uint32_t free_assets = 0;
    for (int i = 0; i < MAX_SHIP_ASSETS; i++) {
        const ship_asset_t *asset = &world.ship_assets[i];
        if (!asset->active || (asset->destroyed && asset->provenance != SHIP_ASSET_PROVENANCE_FLY_PURCHASE)) free_assets++;
    }
    uint32_t remaining = MAX_FLY_PURCHASES - world.fly_purchase_count;
    if (remaining > free_assets) remaining = free_assets;
    BUF_APPEND(pos, out, CAP, "{\"ready\":%s,\"remaining\":%u,\"tick\":%llu,\"stations\":[",
        signal_connectome_enabled() ? "true" : "false",
        remaining, (unsigned long long)world.tick);
    for (int i = 0; i < 3; i++)
        BUF_APPEND(pos, out, CAP, "%s{\"id\":%d,\"x\":%.3f,\"y\":%.3f}",
            i ? "," : "", i, world.stations[i].pos.x, world.stations[i].pos.y);
    BUF_APPEND(pos, out, CAP, "],\"workers\":[");
    bool first = true;
    for (uint32_t i = 0; i < world.fly_purchase_count; i++) {
        const fly_purchase_t *p = &world.fly_purchases[i];
        if (memcmp(p->wallet, wallet, 32)) continue;
        bool paid = false;
        for (size_t k = 0; k < 64; k++) if (p->burn_signature[k]) paid = true;
        if (!paid) continue;
        const ship_asset_t *asset = world_ship_asset_by_id_const(&world, p->asset_id);
        if (!asset) continue;
        const ship_t *ship = world_ship_asset_state_const(&world, asset);
        const npc_ship_t *npc = NULL;
        if (asset->operator_kind == SHIP_ASSET_OPERATOR_NPC && asset->operator_slot >= 0 &&
            asset->operator_slot < MAX_NPC_SHIPS) {
            const npc_ship_t *candidate = &world.npc_ships[asset->operator_slot];
            if (candidate->active && candidate->ship_asset_id == asset->asset_id) npc = candidate;
        }
        double credit_total = 0.0;
        char credits[48] = "null";
        if (world_fly_worker_credits(&world, p->asset_id, &credit_total))
            snprintf(credits, sizeof(credits), "%.2f", credit_total);
        char id[65];
        for (int k = 0; k < 32; k++) snprintf(id + k * 2, 3, "%02x", p->purchase_id[k]);
        vec2 position = ship ? ship->pos : world.stations[p->station].pos;
        BUF_APPEND(pos, out, CAP, "%s{\"id\":\"%s\",\"assetId\":%u,\"station\":%u,"
            "\"lost\":%s,\"launched\":%s,\"state\":%d,\"x\":%.3f,\"y\":%.3f,"
            "\"angle\":%.4f,\"hull\":%.3f,\"maxHull\":%.3f,\"towing\":%d,\"brain\":%d,\"credits\":%s,\"rocks\":[",
            first ? "" : ",", id, p->asset_id, p->station, asset->destroyed ? "true" : "false",
            npc ? "true" : "false", npc ? (int)npc->state : -1, position.x, position.y,
            ship ? ship->angle : 0.0f, ship ? ship->hull : 0.0f,
            hull_def_for_class(asset->hull_class)->max_hull, ship ? ship->towed_count : 0,
            npc ? (int)npc->brain_mode : 0, credits);
        first = false;
        int rocks = 0;
        for (int a = 0; a < MAX_ASTEROIDS && rocks < 36; a++) {
            const asteroid_t *rock = &world.asteroids[a];
            if (!rock->active || v2_dist_sq(rock->pos, position) > 25000000.0f) continue;
            BUF_APPEND(pos, out, CAP, "%s[%.2f,%.2f,%.2f]", rocks ? "," : "",
                rock->pos.x, rock->pos.y, rock->radius);
            rocks++;
        }
        BUF_APPEND(pos, out, CAP, "]}");
    }
    BUF_APPEND(pos, out, CAP, "]}");
    mg_http_reply(c, 200, api_headers, "%s", out);
    free(out);
}
