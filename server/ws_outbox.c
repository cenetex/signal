#include "ws_outbox.h"

#include <limits.h>
#include <string.h>

#include "protocol.h"

static size_t ws_frame_header_size(size_t payload_len) {
    if (payload_len < 126u) return 2u;
    if (payload_len <= UINT16_MAX) return 4u;
    return 10u;
}

static size_t ws_frame_wire_size(size_t payload_len) {
    size_t header = ws_frame_header_size(payload_len);
    if (payload_len > SIZE_MAX - header) return SIZE_MAX;
    return payload_len + header;
}

size_t ws_outbox_wire_bytes(size_t payload_len) {
    return ws_frame_wire_size(payload_len);
}

static bool ws_outbox_key_equal(const ws_outbox_frame_t *frame,
                                ws_outbox_key_t key) {
    return frame && frame->active &&
           frame->family == key.family &&
           frame->object_id == key.object_id;
}

static void ws_outbox_fail(ws_outbox_t *outbox,
                           ws_outbox_close_reason_t reason,
                           ws_outbox_lane_t lane) {
    if (!outbox || outbox->close_reason != WS_OUTBOX_CLOSE_NONE) return;
    outbox->close_reason = reason;
    outbox->stats.disconnect_events++;
    if (lane != WS_OUTBOX_LANE_REPLACEABLE)
        outbox->stats.critical_failures++;
}

static void ws_outbox_refresh_pressure(ws_outbox_t *outbox,
                                       uint64_t now_ms,
                                       size_t transport_bytes) {
    if (!outbox) return;
    size_t total = ws_outbox_total_bytes(outbox, transport_bytes);
    if (total > outbox->stats.high_water_bytes)
        outbox->stats.high_water_bytes = total;
    if (outbox->frame_count > outbox->stats.high_water_frames)
        outbox->stats.high_water_frames = outbox->frame_count;

    if (!outbox->pressure_active &&
        total >= WS_OUTBOX_PRESSURE_ENTER_BYTES) {
        outbox->pressure_active = true;
        outbox->warning_active = false;
        outbox->pressure_enter_ms = now_ms;
        outbox->last_write_progress_ms = now_ms;
    } else if (outbox->pressure_active &&
               total <= WS_OUTBOX_PRESSURE_RECOVER_BYTES) {
        outbox->pressure_active = false;
        outbox->warning_active = false;
        outbox->stats.recovery_events++;
    }

    if (outbox->pressure_active && !outbox->warning_active &&
        total >= WS_OUTBOX_WARNING_BYTES &&
        now_ms - outbox->pressure_enter_ms >=
            WS_OUTBOX_WARNING_SUSTAIN_MS) {
        outbox->warning_active = true;
        outbox->stats.warning_events++;
    }
}

void ws_outbox_init(ws_outbox_t *outbox, uint64_t now_ms) {
    if (!outbox) return;
    memset(outbox, 0, sizeof(*outbox));
    outbox->last_write_progress_ms = now_ms;
}

void ws_outbox_reset(ws_outbox_t *outbox, uint64_t now_ms) {
    ws_outbox_init(outbox, now_ms);
}

static ws_outbox_policy_t ws_outbox_policy_make(ws_outbox_lane_t lane,
                                                 uint16_t family,
                                                 uint32_t object_id,
                                                 uint8_t message_type) {
    ws_outbox_policy_t policy;
    policy.lane = lane;
    policy.key.family = family;
    policy.key.object_id = object_id;
    policy.message_type = message_type;
    return policy;
}

ws_outbox_policy_t ws_outbox_classify(const uint8_t *payload,
                                      size_t payload_len) {
    uint8_t type = payload_len > 0u && payload ? payload[0] : 0u;
    uint32_t object_id = payload_len > 1u && payload ? payload[1] : 0u;

    switch (type) {
    case NET_MSG_JOIN:
    case NET_MSG_LEAVE:
    case NET_MSG_STATE:
    case NET_MSG_SESSION:
    case NET_MSG_PUBKEY_CHALLENGE:
    case NET_MSG_HOST_ASSIGN:
    case NET_MSG_SERVER_INFO:
    case NET_MSG_DEATH:
    case NET_MSG_HAIL_RESPONSE:
    case NET_MSG_FRACTURE_CHALLENGE:
    case NET_MSG_FRACTURE_RESOLVED:
    case NET_MSG_CARGO_RECEIPT_BUNDLE:
    case NET_MSG_ACTION_ACK:
    case NET_MSG_ACTION_RESULT:
    case NET_MSG_LATENCY_PONG:
    case NET_MSG_PROTOCOL_INFO:
    case NET_MSG_HANDOFF_TICKET:
    case NET_MSG_HANDOFF_RESULT:
    case NET_MSG_INPUT_APPLIED:
    case NET_MSG_LEGACY_RECOVERY_OFFER:
    case NET_MSG_LEGACY_RECOVERY_RESULT:
        return ws_outbox_policy_make(
            WS_OUTBOX_LANE_CONTROL, WS_OUTBOX_FAMILY_NONE, 0u, type);

    case NET_MSG_STATION_IDENTITY:
    case NET_MSG_STATION_IDENTITY_Q:
        return ws_outbox_policy_make(
            WS_OUTBOX_LANE_REPLACEABLE,
            WS_OUTBOX_FAMILY_STATION_IDENTITY, object_id, type);
    case NET_MSG_STATION_DIAG:
        return ws_outbox_policy_make(
            WS_OUTBOX_LANE_REPLACEABLE,
            WS_OUTBOX_FAMILY_STATION_DIAG, object_id, type);
    case NET_MSG_STATION_MANIFEST:
        return ws_outbox_policy_make(
            WS_OUTBOX_LANE_REPLACEABLE,
            WS_OUTBOX_FAMILY_STATION_MANIFEST, object_id, type);
    case NET_MSG_WORLD_STATIONS:
    case NET_MSG_WORLD_STATIONS_Q:
        return ws_outbox_policy_make(
            WS_OUTBOX_LANE_REPLACEABLE,
            WS_OUTBOX_FAMILY_WORLD_STATIONS, 0u, type);
    case NET_MSG_CONTRACTS:
    case NET_MSG_CONTRACTS_Q:
        return ws_outbox_policy_make(
            WS_OUTBOX_LANE_REPLACEABLE,
            WS_OUTBOX_FAMILY_CONTRACTS, 0u, type);
    case NET_MSG_PLAYER_SHIP:
        return ws_outbox_policy_make(
            WS_OUTBOX_LANE_REPLACEABLE,
            WS_OUTBOX_FAMILY_PLAYER_SHIP, 0u, type);
    case NET_MSG_PLAYER_MANIFEST:
        return ws_outbox_policy_make(
            WS_OUTBOX_LANE_REPLACEABLE,
            WS_OUTBOX_FAMILY_PLAYER_MANIFEST, 0u, type);
    case NET_MSG_WORLD_PLAYERS:
        return ws_outbox_policy_make(
            WS_OUTBOX_LANE_REPLACEABLE,
            WS_OUTBOX_FAMILY_WORLD_PLAYERS, 0u, type);
    case NET_MSG_WORLD_PLAYER_DOCK_Q:
        return ws_outbox_policy_make(
            WS_OUTBOX_LANE_REPLACEABLE,
            WS_OUTBOX_FAMILY_WORLD_PLAYER_DOCK, 0u, type);
    case NET_MSG_WORLD_TIME:
        return ws_outbox_policy_make(
            WS_OUTBOX_LANE_REPLACEABLE,
            WS_OUTBOX_FAMILY_WORLD_TIME, 0u, type);
    case NET_MSG_WORLD_TOW_LINKS:
        return ws_outbox_policy_make(
            WS_OUTBOX_LANE_REPLACEABLE,
            WS_OUTBOX_FAMILY_WORLD_TOW_LINKS, 0u, type);
    case NET_MSG_HIGHSCORES:
        return ws_outbox_policy_make(
            WS_OUTBOX_LANE_REPLACEABLE,
            WS_OUTBOX_FAMILY_HIGHSCORES, 0u, type);
    case NET_MSG_SIGNAL_CHANNEL:
        return ws_outbox_policy_make(
            WS_OUTBOX_LANE_REPLACEABLE,
            WS_OUTBOX_FAMILY_SIGNAL_CHANNEL, 0u, type);
    case NET_MSG_PLAYER_KNOWN_CONTRACTS:
        return ws_outbox_policy_make(
            WS_OUTBOX_LANE_REPLACEABLE,
            WS_OUTBOX_FAMILY_KNOWN_CONTRACTS, 0u, type);
    case NET_MSG_PLAYER_MARKET_MEMORIES:
        return ws_outbox_policy_make(
            WS_OUTBOX_LANE_REPLACEABLE,
            WS_OUTBOX_FAMILY_MARKET_MEMORIES, 0u, type);
    case NET_MSG_PLAYER_KNOWN_LEDGER:
        return ws_outbox_policy_make(
            WS_OUTBOX_LANE_REPLACEABLE,
            WS_OUTBOX_FAMILY_KNOWN_LEDGER, 0u, type);
    case NET_MSG_DELIVERY_LEDGER:
        return ws_outbox_policy_make(
            WS_OUTBOX_LANE_REPLACEABLE,
            WS_OUTBOX_FAMILY_DELIVERY_LEDGER, 0u, type);
    case NET_MSG_INSPECT_SNAPSHOT:
        return ws_outbox_policy_make(
            WS_OUTBOX_LANE_REPLACEABLE,
            WS_OUTBOX_FAMILY_INSPECT_SNAPSHOT, 0u, type);
    default:
        /*
         * Unknown future messages are reliable by construction. A newly
         * introduced critical packet can therefore fail closed, never be
         * silently replaced by an older protocol's classification table.
         */
        return ws_outbox_policy_make(
            WS_OUTBOX_LANE_RELIABLE, WS_OUTBOX_FAMILY_NONE, 0u, type);
    }
}

static size_t ws_outbox_count_free_pages(const ws_outbox_t *outbox,
                                         ws_outbox_lane_t lane) {
    if (!outbox) return 0u;
    size_t free_pages = 0u;
    size_t begin = lane == WS_OUTBOX_LANE_CONTROL
        ? 0u : 0u;
    size_t end = lane == WS_OUTBOX_LANE_CONTROL
        ? WS_OUTBOX_PAGE_COUNT : WS_OUTBOX_NORMAL_PAGE_COUNT;
    for (size_t i = begin; i < end; i++) {
        if (!outbox->page_used[i]) free_pages++;
    }
    return free_pages;
}

static uint16_t ws_outbox_take_free_page(ws_outbox_t *outbox,
                                         ws_outbox_lane_t lane,
                                         bool reserved_first) {
    if (!outbox) return WS_OUTBOX_PAGE_NONE;
    if (lane == WS_OUTBOX_LANE_CONTROL && reserved_first) {
        for (size_t i = WS_OUTBOX_NORMAL_PAGE_COUNT;
             i < WS_OUTBOX_PAGE_COUNT; i++) {
            if (!outbox->page_used[i]) {
                outbox->page_used[i] = 1u;
                outbox->page_next[i] = WS_OUTBOX_PAGE_NONE;
                return (uint16_t)i;
            }
        }
    }
    size_t end = lane == WS_OUTBOX_LANE_CONTROL
        ? WS_OUTBOX_PAGE_COUNT : WS_OUTBOX_NORMAL_PAGE_COUNT;
    for (size_t i = 0u; i < end; i++) {
        if (!outbox->page_used[i]) {
            outbox->page_used[i] = 1u;
            outbox->page_next[i] = WS_OUTBOX_PAGE_NONE;
            return (uint16_t)i;
        }
    }
    return WS_OUTBOX_PAGE_NONE;
}

static void ws_outbox_release_pages(ws_outbox_t *outbox,
                                    uint16_t first_page) {
    if (!outbox) return;
    uint16_t page = first_page;
    size_t guard = 0u;
    while (page != WS_OUTBOX_PAGE_NONE && guard < WS_OUTBOX_PAGE_COUNT) {
        uint16_t next = outbox->page_next[page];
        outbox->page_used[page] = 0u;
        outbox->page_next[page] = WS_OUTBOX_PAGE_NONE;
        page = next;
        guard++;
    }
}

static uint16_t ws_outbox_store_payload(ws_outbox_t *outbox,
                                        ws_outbox_lane_t lane,
                                        const uint8_t *payload,
                                        size_t payload_len,
                                        uint16_t *out_page_count) {
    if (!outbox || (!payload && payload_len > 0u) || !out_page_count)
        return WS_OUTBOX_PAGE_NONE;
    size_t page_count =
        (payload_len + WS_OUTBOX_PAGE_BYTES - 1u) / WS_OUTBOX_PAGE_BYTES;
    if (page_count == 0u) page_count = 1u;
    if (page_count > UINT16_MAX ||
        ws_outbox_count_free_pages(outbox, lane) < page_count) {
        return WS_OUTBOX_PAGE_NONE;
    }

    uint16_t first = WS_OUTBOX_PAGE_NONE;
    uint16_t previous = WS_OUTBOX_PAGE_NONE;
    size_t copied = 0u;
    for (size_t n = 0u; n < page_count; n++) {
        uint16_t page = ws_outbox_take_free_page(
            outbox, lane, lane == WS_OUTBOX_LANE_CONTROL);
        if (page == WS_OUTBOX_PAGE_NONE) {
            ws_outbox_release_pages(outbox, first);
            return WS_OUTBOX_PAGE_NONE;
        }
        if (first == WS_OUTBOX_PAGE_NONE) first = page;
        if (previous != WS_OUTBOX_PAGE_NONE)
            outbox->page_next[previous] = page;
        previous = page;

        size_t remaining = payload_len - copied;
        size_t chunk = remaining < WS_OUTBOX_PAGE_BYTES
            ? remaining : WS_OUTBOX_PAGE_BYTES;
        if (chunk > 0u)
            memcpy(outbox->pages[page], payload + copied, chunk);
        if (chunk < WS_OUTBOX_PAGE_BYTES)
            memset(outbox->pages[page] + chunk, 0,
                   WS_OUTBOX_PAGE_BYTES - chunk);
        copied += chunk;
    }
    *out_page_count = (uint16_t)page_count;
    return first;
}

static bool ws_outbox_copy_payload(const ws_outbox_t *outbox,
                                   const ws_outbox_frame_t *frame,
                                   uint8_t *dst,
                                   size_t dst_capacity) {
    if (!outbox || !frame || !frame->active || !dst ||
        frame->payload_len > dst_capacity) {
        return false;
    }
    uint16_t page = frame->first_page;
    size_t copied = 0u;
    size_t guard = 0u;
    while (copied < frame->payload_len &&
           page != WS_OUTBOX_PAGE_NONE &&
           guard < frame->page_count) {
        size_t remaining = (size_t)frame->payload_len - copied;
        size_t chunk = remaining < WS_OUTBOX_PAGE_BYTES
            ? remaining : WS_OUTBOX_PAGE_BYTES;
        memcpy(dst + copied, outbox->pages[page], chunk);
        copied += chunk;
        page = outbox->page_next[page];
        guard++;
    }
    return copied == frame->payload_len;
}

static int ws_outbox_find_replaceable(const ws_outbox_t *outbox,
                                      ws_outbox_key_t key) {
    if (!outbox || key.family == WS_OUTBOX_FAMILY_NONE) return -1;
    for (int i = 0; i < WS_OUTBOX_MAX_FRAMES; i++) {
        const ws_outbox_frame_t *frame = &outbox->frames[i];
        if (frame->lane == WS_OUTBOX_LANE_REPLACEABLE &&
            ws_outbox_key_equal(frame, key)) {
            return i;
        }
    }
    return -1;
}

static int ws_outbox_find_free_frame(const ws_outbox_t *outbox,
                                     ws_outbox_lane_t lane) {
    if (!outbox) return -1;
    if (lane == WS_OUTBOX_LANE_CONTROL) {
        for (int i = WS_OUTBOX_NORMAL_FRAME_COUNT;
             i < WS_OUTBOX_MAX_FRAMES; i++) {
            if (!outbox->frames[i].active) return i;
        }
    }
    int end = lane == WS_OUTBOX_LANE_CONTROL
        ? WS_OUTBOX_MAX_FRAMES : WS_OUTBOX_NORMAL_FRAME_COUNT;
    for (int i = 0; i < end; i++) {
        if (!outbox->frames[i].active) return i;
    }
    return -1;
}

static bool ws_outbox_admission_fits(const ws_outbox_t *outbox,
                                     ws_outbox_lane_t lane,
                                     size_t old_wire_bytes,
                                     size_t new_wire_bytes,
                                     size_t transport_bytes) {
    if (!outbox || old_wire_bytes > outbox->stats.queue_wire_bytes)
        return false;
    size_t base = outbox->stats.queue_wire_bytes - old_wire_bytes;
    if (base > SIZE_MAX - transport_bytes)
        return false;
    size_t projected = base + transport_bytes;
    if (projected > SIZE_MAX - new_wire_bytes)
        return false;
    projected += new_wire_bytes;
    size_t limit = lane == WS_OUTBOX_LANE_CONTROL
        ? WS_OUTBOX_APP_HARD_BYTES : WS_OUTBOX_NORMAL_LIMIT_BYTES;
    return projected <= limit;
}

bool ws_outbox_can_admit_control_frame(
    const ws_outbox_t *outbox,
    const uint8_t *payload,
    size_t payload_len,
    size_t transport_bytes) {
    if (!outbox || !payload || payload_len == 0u ||
        payload_len > WS_OUTBOX_MAX_FRAME_BYTES ||
        outbox->close_reason != WS_OUTBOX_CLOSE_NONE ||
        ws_outbox_classify(payload, payload_len).lane !=
            WS_OUTBOX_LANE_CONTROL) {
        return false;
    }

    size_t wire_bytes = ws_frame_wire_size(payload_len);
    if (wire_bytes == SIZE_MAX ||
        !ws_outbox_admission_fits(
            outbox, WS_OUTBOX_LANE_CONTROL, 0u,
            wire_bytes, transport_bytes)) {
        return false;
    }

    size_t needed_pages =
        (payload_len + WS_OUTBOX_PAGE_BYTES - 1u) /
        WS_OUTBOX_PAGE_BYTES;
    if (needed_pages == 0u) needed_pages = 1u;
    return ws_outbox_count_free_pages(
               outbox, WS_OUTBOX_LANE_CONTROL) >= needed_pages &&
           ws_outbox_find_free_frame(
               outbox, WS_OUTBOX_LANE_CONTROL) >= 0;
}

bool ws_outbox_can_admit_reliable_batch(
    const ws_outbox_t *outbox,
    const uint8_t *const *payloads,
    const size_t *payload_lengths,
    size_t payload_count,
    size_t transport_bytes) {
    if (!outbox || !payloads || !payload_lengths || payload_count == 0u ||
        outbox->close_reason != WS_OUTBOX_CLOSE_NONE) {
        return false;
    }

    size_t required_wire = 0u;
    size_t required_pages = 0u;
    size_t required_frames = 0u;
    for (size_t i = 0u; i < payload_count; i++) {
        const uint8_t *payload = payloads[i];
        size_t payload_len = payload_lengths[i];
        if (!payload || payload_len == 0u ||
            payload_len > WS_OUTBOX_MAX_FRAME_BYTES ||
            ws_outbox_classify(payload, payload_len).lane !=
                WS_OUTBOX_LANE_RELIABLE) {
            return false;
        }
        size_t wire_bytes = ws_frame_wire_size(payload_len);
        size_t pages =
            (payload_len + WS_OUTBOX_PAGE_BYTES - 1u) /
            WS_OUTBOX_PAGE_BYTES;
        if (wire_bytes == SIZE_MAX ||
            required_wire > SIZE_MAX - wire_bytes ||
            required_pages > SIZE_MAX - pages) {
            return false;
        }
        required_wire += wire_bytes;
        required_pages += pages;
        required_frames++;
    }

    if (outbox->stats.queue_wire_bytes > SIZE_MAX - transport_bytes)
        return false;
    size_t projected =
        outbox->stats.queue_wire_bytes + transport_bytes;
    if (projected > SIZE_MAX - required_wire ||
        projected + required_wire > WS_OUTBOX_NORMAL_LIMIT_BYTES) {
        return false;
    }
    if (ws_outbox_count_free_pages(
            outbox, WS_OUTBOX_LANE_RELIABLE) < required_pages) {
        return false;
    }
    size_t free_frames = 0u;
    for (int i = 0; i < WS_OUTBOX_NORMAL_FRAME_COUNT; i++) {
        if (!outbox->frames[i].active) free_frames++;
    }
    return free_frames >= required_frames;
}

void ws_outbox_note_suppressed(ws_outbox_t *outbox, size_t payload_len) {
    if (!outbox) return;
    outbox->stats.suppressed_packets++;
    outbox->stats.suppressed_payload_bytes += payload_len;
    outbox->needs_resync = true;
}

ws_outbox_result_t ws_outbox_enqueue(ws_outbox_t *outbox,
                                     const uint8_t *payload,
                                     size_t payload_len,
                                     size_t transport_bytes,
                                     uint64_t now_ms) {
    if (!outbox) return WS_OUTBOX_CLOSED;
    if (outbox->close_reason != WS_OUTBOX_CLOSE_NONE)
        return WS_OUTBOX_CLOSED;

    ws_outbox_policy_t policy = ws_outbox_classify(payload, payload_len);
    if (!payload || payload_len == 0u ||
        payload_len > WS_OUTBOX_MAX_FRAME_BYTES) {
        ws_outbox_fail(outbox, WS_OUTBOX_CLOSE_FRAME_TOO_LARGE,
                       policy.lane);
        return WS_OUTBOX_FATAL;
    }
    /*
     * Defense in depth for #672 containment: this retired message disclosed
     * prefixes from unrelated legacy save basenames. Even if a future caller
     * accidentally reconstructs a producer, it cannot enter the dedicated
     * server's only binary-message outbox.
     */
    if (payload[0] == NET_MSG_LEGACY_SAVES_AVAILABLE)
        return WS_OUTBOX_SUPPRESSED;
    size_t wire_bytes = ws_frame_wire_size(payload_len);
    if (wire_bytes == SIZE_MAX) {
        ws_outbox_fail(outbox, WS_OUTBOX_CLOSE_FRAME_TOO_LARGE,
                       policy.lane);
        return WS_OUTBOX_FATAL;
    }

    int replacement = -1;
    size_t old_wire_bytes = 0u;
    uint16_t old_page_count = 0u;
    if (policy.lane == WS_OUTBOX_LANE_REPLACEABLE) {
        replacement = ws_outbox_find_replaceable(outbox, policy.key);
        if (replacement >= 0) {
            old_wire_bytes = outbox->frames[replacement].wire_bytes;
            old_page_count = outbox->frames[replacement].page_count;
        }
    }

    if (!ws_outbox_admission_fits(outbox, policy.lane, old_wire_bytes,
                                  wire_bytes, transport_bytes)) {
        if (policy.lane == WS_OUTBOX_LANE_REPLACEABLE) {
            ws_outbox_note_suppressed(outbox, payload_len);
            ws_outbox_refresh_pressure(outbox, now_ms, transport_bytes);
            return WS_OUTBOX_SUPPRESSED;
        }
        ws_outbox_fail(
            outbox,
            policy.lane == WS_OUTBOX_LANE_CONTROL
                ? WS_OUTBOX_CLOSE_CONTROL_HEADROOM_EXHAUSTED
                : WS_OUTBOX_CLOSE_QUEUE_HARD_LIMIT,
            policy.lane);
        return WS_OUTBOX_FATAL;
    }

    size_t needed_pages =
        (payload_len + WS_OUTBOX_PAGE_BYTES - 1u) / WS_OUTBOX_PAGE_BYTES;
    if (needed_pages == 0u) needed_pages = 1u;
    size_t available_pages =
        ws_outbox_count_free_pages(outbox, policy.lane) + old_page_count;
    if (available_pages < needed_pages) {
        if (policy.lane == WS_OUTBOX_LANE_REPLACEABLE) {
            ws_outbox_note_suppressed(outbox, payload_len);
            ws_outbox_refresh_pressure(outbox, now_ms, transport_bytes);
            return WS_OUTBOX_SUPPRESSED;
        }
        ws_outbox_fail(
            outbox,
            policy.lane == WS_OUTBOX_LANE_CONTROL
                ? WS_OUTBOX_CLOSE_CONTROL_HEADROOM_EXHAUSTED
                : WS_OUTBOX_CLOSE_QUEUE_HARD_LIMIT,
            policy.lane);
        return WS_OUTBOX_FATAL;
    }

    int frame_index = replacement >= 0
        ? replacement : ws_outbox_find_free_frame(outbox, policy.lane);
    if (frame_index < 0) {
        if (policy.lane == WS_OUTBOX_LANE_REPLACEABLE) {
            ws_outbox_note_suppressed(outbox, payload_len);
            ws_outbox_refresh_pressure(outbox, now_ms, transport_bytes);
            return WS_OUTBOX_SUPPRESSED;
        }
        ws_outbox_fail(outbox, WS_OUTBOX_CLOSE_DESCRIPTOR_EXHAUSTED,
                       policy.lane);
        return WS_OUTBOX_FATAL;
    }

    ws_outbox_frame_t *frame = &outbox->frames[frame_index];
    if (replacement >= 0) {
        ws_outbox_release_pages(outbox, frame->first_page);
        outbox->stats.queue_payload_bytes -= frame->payload_len;
        outbox->stats.queue_wire_bytes -= frame->wire_bytes;
    }

    uint16_t page_count = 0u;
    uint16_t first_page = ws_outbox_store_payload(
        outbox, policy.lane, payload, payload_len, &page_count);
    if (first_page == WS_OUTBOX_PAGE_NONE) {
        /*
         * available_pages was checked while the old replacement pages still
         * counted, so reaching this branch indicates internal corruption.
         * Fail explicitly for every lane.
         */
        ws_outbox_fail(outbox, WS_OUTBOX_CLOSE_QUEUE_HARD_LIMIT,
                       policy.lane);
        return WS_OUTBOX_FATAL;
    }

    frame->active = true;
    frame->lane = (uint8_t)policy.lane;
    frame->message_type = policy.message_type;
    frame->first_page = first_page;
    frame->page_count = page_count;
    frame->family = policy.key.family;
    frame->object_id = policy.key.object_id;
    frame->payload_len = (uint32_t)payload_len;
    frame->wire_bytes = (uint32_t)wire_bytes;
    frame->sequence = ++outbox->next_sequence;
    outbox->stats.queue_payload_bytes += payload_len;
    outbox->stats.queue_wire_bytes += wire_bytes;

    if (replacement >= 0) {
        outbox->stats.coalesced_packets++;
        outbox->stats.coalesced_payload_bytes += payload_len;
    } else {
        outbox->frame_count++;
        outbox->stats.admitted_packets++;
        outbox->stats.admitted_payload_bytes += payload_len;
    }
    ws_outbox_refresh_pressure(outbox, now_ms, transport_bytes);
    return replacement >= 0 ? WS_OUTBOX_COALESCED : WS_OUTBOX_ADMITTED;
}

static int ws_outbox_find_oldest(const ws_outbox_t *outbox,
                                 bool control) {
    if (!outbox) return -1;
    int selected = -1;
    uint64_t sequence = UINT64_MAX;
    for (int i = 0; i < WS_OUTBOX_MAX_FRAMES; i++) {
        const ws_outbox_frame_t *frame = &outbox->frames[i];
        if (!frame->active) continue;
        bool is_control = frame->lane == WS_OUTBOX_LANE_CONTROL;
        if (is_control != control) continue;
        if (frame->sequence < sequence) {
            selected = i;
            sequence = frame->sequence;
        }
    }
    return selected;
}

size_t ws_outbox_pump(ws_outbox_t *outbox,
                      size_t transport_bytes,
                      size_t transport_limit_bytes,
                      uint64_t now_ms,
                      uint8_t *scratch,
                      size_t scratch_capacity,
                      ws_outbox_transport_send_fn send,
                      void *send_user) {
    if (!outbox || !scratch || !send ||
        outbox->close_reason != WS_OUTBOX_CLOSE_NONE) {
        return 0u;
    }
    if (transport_limit_bytes == 0u)
        transport_limit_bytes = WS_OUTBOX_TRANSPORT_LIMIT_BYTES;

    size_t sent_count = 0u;
    size_t local_transport = transport_bytes;
    bool sent_noncontrol = false;
    for (;;) {
        int frame_index = ws_outbox_find_oldest(outbox, true);
        if (frame_index < 0) {
            if (sent_noncontrol) break;
            frame_index = ws_outbox_find_oldest(outbox, false);
            if (frame_index < 0) break;
        }
        ws_outbox_frame_t *frame = &outbox->frames[frame_index];
        if (frame->wire_bytes > transport_limit_bytes ||
            local_transport >
                transport_limit_bytes - frame->wire_bytes) {
            break;
        }
        if (frame->payload_len > scratch_capacity ||
            !ws_outbox_copy_payload(outbox, frame, scratch,
                                    scratch_capacity)) {
            ws_outbox_fail(outbox, WS_OUTBOX_CLOSE_FRAME_TOO_LARGE,
                           (ws_outbox_lane_t)frame->lane);
            break;
        }
        if (!send(send_user, scratch, frame->payload_len)) {
            ws_outbox_fail(outbox, WS_OUTBOX_CLOSE_TRANSPORT_REJECTED,
                           (ws_outbox_lane_t)frame->lane);
            break;
        }

        bool was_control = frame->lane == WS_OUTBOX_LANE_CONTROL;
        local_transport += frame->wire_bytes;
        outbox->stats.sent_packets++;
        outbox->stats.sent_payload_bytes += frame->payload_len;
        outbox->stats.sent_wire_bytes += frame->wire_bytes;
        outbox->stats.queue_payload_bytes -= frame->payload_len;
        outbox->stats.queue_wire_bytes -= frame->wire_bytes;
        ws_outbox_release_pages(outbox, frame->first_page);
        memset(frame, 0, sizeof(*frame));
        outbox->frame_count--;
        sent_count++;
        if (!was_control) sent_noncontrol = true;
    }
    ws_outbox_refresh_pressure(outbox, now_ms, local_transport);
    return sent_count;
}

void ws_outbox_note_write_progress(ws_outbox_t *outbox,
                                   uint64_t now_ms,
                                   size_t bytes_written) {
    if (!outbox || bytes_written == 0u) return;
    outbox->last_write_progress_ms = now_ms;
}

ws_outbox_close_reason_t ws_outbox_check_timeouts(ws_outbox_t *outbox,
                                                  uint64_t now_ms,
                                                  size_t transport_bytes) {
    if (!outbox) return WS_OUTBOX_CLOSE_NONE;
    if (outbox->close_reason != WS_OUTBOX_CLOSE_NONE)
        return outbox->close_reason;
    ws_outbox_refresh_pressure(outbox, now_ms, transport_bytes);
    if (!outbox->pressure_active) return WS_OUTBOX_CLOSE_NONE;

    size_t total = ws_outbox_total_bytes(outbox, transport_bytes);
    if (total >= WS_OUTBOX_WARNING_BYTES &&
        now_ms - outbox->last_write_progress_ms >=
            WS_OUTBOX_NO_PROGRESS_MS) {
        ws_outbox_fail(outbox, WS_OUTBOX_CLOSE_NO_WRITE_PROGRESS,
                       WS_OUTBOX_LANE_RELIABLE);
    } else if (now_ms - outbox->pressure_enter_ms >=
                   WS_OUTBOX_PRESSURE_DISCONNECT_MS) {
        ws_outbox_fail(outbox, WS_OUTBOX_CLOSE_SUSTAINED_PRESSURE,
                       WS_OUTBOX_LANE_RELIABLE);
    }
    return outbox->close_reason;
}

size_t ws_outbox_total_bytes(const ws_outbox_t *outbox,
                             size_t transport_bytes) {
    if (!outbox) return transport_bytes;
    if (outbox->stats.queue_wire_bytes > SIZE_MAX - transport_bytes)
        return SIZE_MAX;
    return outbox->stats.queue_wire_bytes + transport_bytes;
}

bool ws_outbox_should_suppress(const ws_outbox_t *outbox,
                               size_t transport_bytes) {
    return outbox &&
        (outbox->pressure_active ||
         ws_outbox_total_bytes(outbox, transport_bytes) >=
             WS_OUTBOX_PRESSURE_ENTER_BYTES);
}

bool ws_outbox_needs_resync(const ws_outbox_t *outbox) {
    return outbox && outbox->needs_resync;
}

void ws_outbox_mark_resynced(ws_outbox_t *outbox) {
    if (!outbox) return;
    outbox->needs_resync = false;
}

const char *ws_outbox_close_reason_name(ws_outbox_close_reason_t reason) {
    switch (reason) {
    case WS_OUTBOX_CLOSE_NONE: return "none";
    case WS_OUTBOX_CLOSE_QUEUE_HARD_LIMIT: return "queue_hard_limit";
    case WS_OUTBOX_CLOSE_CONTROL_HEADROOM_EXHAUSTED:
        return "control_headroom_exhausted";
    case WS_OUTBOX_CLOSE_NO_WRITE_PROGRESS: return "no_write_progress";
    case WS_OUTBOX_CLOSE_SUSTAINED_PRESSURE:
        return "sustained_pressure";
    case WS_OUTBOX_CLOSE_FRAME_TOO_LARGE: return "frame_too_large";
    case WS_OUTBOX_CLOSE_DESCRIPTOR_EXHAUSTED:
        return "descriptor_exhausted";
    case WS_OUTBOX_CLOSE_TRANSPORT_REJECTED:
        return "transport_rejected";
    default: return "unknown";
    }
}

size_t ws_outbox_resident_capacity_bytes(void) {
    return sizeof(ws_outbox_t);
}

size_t ws_replication_cycle_order(
    bool private_due,
    bool bulk_due,
    ws_replication_cycle_t order[2]) {
    if (!order) return 0u;
    size_t count = 0u;
    if (private_due)
        order[count++] = WS_REPLICATION_CYCLE_PRIVATE;
    if (bulk_due)
        order[count++] = WS_REPLICATION_CYCLE_BULK;
    return count;
}

bool ws_backpressure_fixture_enabled(const char *value) {
    return value && value[0] != '\0' &&
           strcmp(value, "0") != 0 &&
           strcmp(value, "false") != 0 &&
           strcmp(value, "FALSE") != 0 &&
           strcmp(value, "no") != 0 &&
           strcmp(value, "NO") != 0;
}

void ws_sync_pacer_init(ws_sync_pacer_t *pacer, uint64_t now_ms) {
    if (!pacer) return;
    pacer->credit_byte_ms =
        (uint64_t)WS_OUTBOX_SYNC_BURST_BYTES * 1000u;
    pacer->last_refill_ms = now_ms;
}

static void ws_sync_pacer_refill(ws_sync_pacer_t *pacer,
                                 uint64_t now_ms) {
    if (!pacer) return;
    if (now_ms > pacer->last_refill_ms) {
        uint64_t elapsed = now_ms - pacer->last_refill_ms;
        uint64_t add;
        if (elapsed > UINT64_MAX / WS_OUTBOX_SYNC_RATE_BYTES_PER_SEC)
            add = UINT64_MAX;
        else
            add = elapsed * WS_OUTBOX_SYNC_RATE_BYTES_PER_SEC;
        uint64_t cap = (uint64_t)WS_OUTBOX_SYNC_BURST_BYTES * 1000u;
        if (add >= cap || pacer->credit_byte_ms >= cap - add)
            pacer->credit_byte_ms = cap;
        else
            pacer->credit_byte_ms += add;
        pacer->last_refill_ms = now_ms;
    }
}

bool ws_sync_pacer_can_send(ws_sync_pacer_t *pacer,
                            uint64_t now_ms,
                            size_t wire_bytes) {
    if (!pacer || wire_bytes > WS_OUTBOX_SYNC_BURST_BYTES ||
        wire_bytes > UINT64_MAX / 1000u) {
        return false;
    }
    ws_sync_pacer_refill(pacer, now_ms);
    return (uint64_t)wire_bytes * 1000u <= pacer->credit_byte_ms;
}

bool ws_sync_pacer_charge(ws_sync_pacer_t *pacer,
                          uint64_t now_ms,
                          size_t wire_bytes) {
    if (!ws_sync_pacer_can_send(pacer, now_ms, wire_bytes))
        return false;
    uint64_t cost = (uint64_t)wire_bytes * 1000u;
    pacer->credit_byte_ms -= cost;
    return true;
}

bool ws_sync_pacer_allow(ws_sync_pacer_t *pacer,
                         uint64_t now_ms,
                         size_t wire_bytes) {
    return ws_sync_pacer_charge(pacer, now_ms, wire_bytes);
}

void ws_initial_sync_begin(ws_initial_sync_t *sync,
                           uint64_t now_ms,
                           uint16_t station_count) {
    if (!sync) return;
    memset(sync, 0, sizeof(*sync));
    ws_sync_pacer_init(&sync->pacer, now_ms);
    sync->started_ms = now_ms;
    sync->station_count = station_count;
    sync->step = WS_INITIAL_SYNC_PRIVATE;
    sync->active = true;
}

ws_initial_sync_step_t ws_initial_sync_current(
    const ws_initial_sync_t *sync,
    uint16_t *station_index) {
    if (station_index)
        *station_index = sync ? sync->station_index : 0u;
    if (!sync || !sync->active)
        return WS_INITIAL_SYNC_NONE;
    return (ws_initial_sync_step_t)sync->step;
}

uint8_t ws_initial_sync_substep(const ws_initial_sync_t *sync) {
    return sync && sync->active ? sync->substep : 0u;
}

void ws_initial_sync_commit_substep(ws_initial_sync_t *sync,
                                    uint8_t substep_count,
                                    uint64_t now_ms) {
    if (!sync || !sync->active || substep_count == 0u) return;
    sync->substep++;
    if (sync->substep >= substep_count) {
        sync->substep = 0u;
        ws_initial_sync_commit(sync, now_ms);
    }
}

void ws_initial_sync_skip_station(ws_initial_sync_t *sync) {
    if (!sync || !sync->active ||
        sync->step < WS_INITIAL_SYNC_STATION_IDENTITY ||
        sync->step > WS_INITIAL_SYNC_STATION_MANIFEST) {
        return;
    }
    sync->substep = 0u;
    sync->station_index++;
    sync->step = sync->station_index < sync->station_count
        ? WS_INITIAL_SYNC_STATION_IDENTITY
        : WS_INITIAL_SYNC_WORLD_STATIONS;
}

void ws_initial_sync_commit(ws_initial_sync_t *sync, uint64_t now_ms) {
    if (!sync || !sync->active) return;
    sync->substep = 0u;
    switch ((ws_initial_sync_step_t)sync->step) {
    case WS_INITIAL_SYNC_PRIVATE:
        sync->step = sync->station_count > 0u
            ? WS_INITIAL_SYNC_STATION_IDENTITY
            : WS_INITIAL_SYNC_WORLD_STATIONS;
        break;
    case WS_INITIAL_SYNC_STATION_IDENTITY:
        sync->step = WS_INITIAL_SYNC_STATION_DIAG;
        break;
    case WS_INITIAL_SYNC_STATION_DIAG:
        sync->step = WS_INITIAL_SYNC_STATION_MANIFEST;
        break;
    case WS_INITIAL_SYNC_STATION_MANIFEST:
        sync->station_index++;
        sync->step = sync->station_index < sync->station_count
            ? WS_INITIAL_SYNC_STATION_IDENTITY
            : WS_INITIAL_SYNC_WORLD_STATIONS;
        break;
    case WS_INITIAL_SYNC_WORLD_STATIONS:
        sync->step = WS_INITIAL_SYNC_CONTRACTS;
        break;
    case WS_INITIAL_SYNC_CONTRACTS:
        sync->step = WS_INITIAL_SYNC_ASTEROIDS;
        break;
    case WS_INITIAL_SYNC_ASTEROIDS:
        sync->step = WS_INITIAL_SYNC_HIGHSCORES;
        break;
    case WS_INITIAL_SYNC_HIGHSCORES:
        sync->step = WS_INITIAL_SYNC_SIGNAL_CHANNEL;
        break;
    case WS_INITIAL_SYNC_SIGNAL_CHANNEL:
        sync->step = WS_INITIAL_SYNC_CATCHUP;
        sync->catchup_snapshot_admitted = false;
        break;
    case WS_INITIAL_SYNC_CATCHUP:
        sync->step = WS_INITIAL_SYNC_DONE;
        sync->completed_ms = now_ms;
        sync->active = false;
        sync->reconcile_pending = true;
        break;
    case WS_INITIAL_SYNC_NONE:
    case WS_INITIAL_SYNC_DONE:
    default:
        break;
    }
}

bool ws_initial_sync_active(const ws_initial_sync_t *sync) {
    return sync && sync->active;
}

bool ws_initial_sync_take_reconcile(ws_initial_sync_t *sync) {
    if (!sync || !sync->reconcile_pending) return false;
    sync->reconcile_pending = false;
    return true;
}

bool ws_initial_sync_catchup_needs_snapshot(
    const ws_initial_sync_t *sync) {
    return sync && sync->active &&
        sync->step == WS_INITIAL_SYNC_CATCHUP &&
        !sync->catchup_snapshot_admitted;
}

void ws_initial_sync_catchup_note_snapshot(
    ws_initial_sync_t *sync,
    uint64_t suppressed_packets,
    uint64_t snapshot_generation) {
    if (!sync || !sync->active ||
        sync->step != WS_INITIAL_SYNC_CATCHUP) {
        return;
    }
    sync->catchup_suppressed_packets = suppressed_packets;
    sync->catchup_snapshot_generation = snapshot_generation;
    sync->catchup_snapshot_admitted = true;
}

bool ws_initial_sync_catchup_snapshot_current(
    ws_initial_sync_t *sync,
    uint64_t suppressed_packets,
    uint64_t snapshot_generation) {
    if (!sync || !sync->active ||
        sync->step != WS_INITIAL_SYNC_CATCHUP ||
        !sync->catchup_snapshot_admitted) {
        return false;
    }
    if (sync->catchup_suppressed_packets != suppressed_packets ||
        sync->catchup_snapshot_generation != snapshot_generation) {
        sync->catchup_snapshot_admitted = false;
        return false;
    }
    return true;
}
