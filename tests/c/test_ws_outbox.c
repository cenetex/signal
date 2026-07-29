#include "test_harness.h"

#include "mongoose.h"
#include "ws_outbox.h"

typedef struct {
    uint8_t types[64];
    uint8_t last_payload[2048];
    size_t count;
    size_t last_len;
} outbox_capture_t;

static size_t test_ws_wire_size(size_t payload_len) {
    if (payload_len < 126u) return payload_len + 2u;
    if (payload_len <= UINT16_MAX) return payload_len + 4u;
    return payload_len + 10u;
}

static bool capture_send(void *user, const uint8_t *payload,
                         size_t payload_len) {
    outbox_capture_t *capture = user;
    if (!capture || !payload || payload_len == 0u) return false;
    if (capture->count < sizeof(capture->types))
        capture->types[capture->count] = payload[0];
    capture->count++;
    capture->last_len = payload_len;
    size_t copy_len = payload_len < sizeof(capture->last_payload)
        ? payload_len : sizeof(capture->last_payload);
    memcpy(capture->last_payload, payload, copy_len);
    return true;
}

static ws_outbox_t *test_outbox_new(void) {
    ws_outbox_t *outbox = calloc(1u, sizeof(*outbox));
    if (outbox) ws_outbox_init(outbox, 0u);
    return outbox;
}

TEST(test_ws_outbox_policy_is_semantic_and_fail_safe) {
    uint8_t identity[] = {NET_MSG_STATION_IDENTITY, 7u};
    uint8_t identity_q[] = {NET_MSG_STATION_IDENTITY_Q, 7u};
    uint8_t manifest[] = {NET_MSG_STATION_MANIFEST, 9u};
    uint8_t contracts[] = {NET_MSG_CONTRACTS, 0u};
    uint8_t contracts_q[] = {NET_MSG_CONTRACTS_Q, 0u};
    uint8_t tow_links[] = {NET_MSG_WORLD_TOW_LINKS, 0u};
    uint8_t ack[] = {NET_MSG_ACTION_ACK};
    uint8_t future[] = {0xFEu};

    ws_outbox_policy_t a =
        ws_outbox_classify(identity, sizeof(identity));
    ws_outbox_policy_t aq =
        ws_outbox_classify(identity_q, sizeof(identity_q));
    ws_outbox_policy_t m =
        ws_outbox_classify(manifest, sizeof(manifest));
    ws_outbox_policy_t c =
        ws_outbox_classify(contracts, sizeof(contracts));
    ws_outbox_policy_t cq =
        ws_outbox_classify(contracts_q, sizeof(contracts_q));
    ws_outbox_policy_t tow =
        ws_outbox_classify(tow_links, sizeof(tow_links));
    ws_outbox_policy_t control = ws_outbox_classify(ack, sizeof(ack));
    ws_outbox_policy_t unknown =
        ws_outbox_classify(future, sizeof(future));

    ASSERT_EQ_INT(a.lane, WS_OUTBOX_LANE_REPLACEABLE);
    ASSERT_EQ_INT(a.key.family, aq.key.family);
    ASSERT_EQ_INT(a.key.object_id, aq.key.object_id);
    ASSERT_EQ_INT(a.key.object_id, 7);
    ASSERT_EQ_INT(m.key.family, WS_OUTBOX_FAMILY_STATION_MANIFEST);
    ASSERT_EQ_INT(m.key.object_id, 9);
    ASSERT_EQ_INT(c.key.family, cq.key.family);
    ASSERT_EQ_INT(tow.lane, WS_OUTBOX_LANE_REPLACEABLE);
    ASSERT_EQ_INT(tow.key.family,
                  WS_OUTBOX_FAMILY_WORLD_TOW_LINKS);
    ASSERT_EQ_INT(tow.key.object_id, 0);
    ASSERT_EQ_INT(control.lane, WS_OUTBOX_LANE_CONTROL);
    ASSERT_EQ_INT(unknown.lane, WS_OUTBOX_LANE_RELIABLE);
}

TEST(test_ws_outbox_one_shot_receipts_use_control_reserve) {
    static const uint8_t control_types[] = {
        NET_MSG_JOIN,
        NET_MSG_LEAVE,
        NET_MSG_STATE,
        NET_MSG_SESSION,
        NET_MSG_PUBKEY_CHALLENGE,
        NET_MSG_HOST_ASSIGN,
        NET_MSG_SERVER_INFO,
        NET_MSG_DEATH,
        NET_MSG_HAIL_RESPONSE,
        NET_MSG_FRACTURE_CHALLENGE,
        NET_MSG_FRACTURE_RESOLVED,
        NET_MSG_CARGO_RECEIPT_BUNDLE,
        NET_MSG_ACTION_ACK,
        NET_MSG_ACTION_RESULT,
        NET_MSG_LATENCY_PONG,
        NET_MSG_PROTOCOL_INFO,
        NET_MSG_HANDOFF_TICKET,
        NET_MSG_HANDOFF_RESULT,
        NET_MSG_INPUT_APPLIED,
        NET_MSG_LEGACY_RECOVERY_OFFER,
        NET_MSG_LEGACY_RECOVERY_RESULT
    };
    for (size_t i = 0u; i < sizeof(control_types); i++) {
        uint8_t payload[] = {control_types[i], 0u};
        ws_outbox_policy_t policy =
            ws_outbox_classify(payload, sizeof(payload));
        ASSERT_EQ_INT(policy.lane, WS_OUTBOX_LANE_CONTROL);
    }

    /* Ordered event/delta batches must never be coalesced after serializers
     * mutate their recipient baseline. They remain reliable and fail closed. */
    static const uint8_t reliable_types[] = {
        NET_MSG_EVENTS,
        NET_MSG_EVENTS_V2,
        NET_MSG_WORLD_ASTEROIDS,
        NET_MSG_WORLD_ASTEROID_REMOVE,
        NET_MSG_WORLD_SCAFFOLDS,
        NET_MSG_WORLD_SCAFFOLD_REMOVE,
        NET_MSG_WORLD_CARGO_PODS,
        NET_MSG_WORLD_CARGO_POD_REMOVE,
        NET_MSG_WORLD_NPCS,
        NET_MSG_WORLD_NPC_STATUS
    };
    for (size_t i = 0u; i < sizeof(reliable_types); i++) {
        uint8_t payload[] = {reliable_types[i], 0u};
        ws_outbox_policy_t policy =
            ws_outbox_classify(payload, sizeof(payload));
        ASSERT_EQ_INT(policy.lane, WS_OUTBOX_LANE_RELIABLE);
    }
}

TEST(test_ws_outbox_never_queues_retired_legacy_save_disclosure) {
    ws_outbox_t *outbox = test_outbox_new();
    ASSERT(outbox != NULL);
    const uint8_t retired[] = {
        NET_MSG_LEGACY_SAVES_AVAILABLE,
        2u,
        'd', 'e', 'a', 'd', 'b', 'e', 'e', 'f',
        'c', 'a', 'f', 'e', 'b', 'a', 'b', 'e',
    };
    ASSERT_EQ_INT(
        ws_outbox_enqueue(outbox, retired, sizeof(retired), 0u, 1u),
        WS_OUTBOX_SUPPRESSED);
    ASSERT(outbox->stats.queue_wire_bytes == 0u);
    ASSERT(outbox->frame_count == 0u);
    ASSERT_EQ_INT(outbox->close_reason, WS_OUTBOX_CLOSE_NONE);
}

TEST(test_ws_outbox_coalesces_latest_semantic_state) {
    ws_outbox_t *outbox = test_outbox_new();
    ASSERT(outbox != NULL);
    uint8_t first[300] = {NET_MSG_STATION_MANIFEST, 12u, 1u};
    uint8_t latest[500] = {NET_MSG_STATION_MANIFEST, 12u, 2u};
    latest[499] = 0xA5u;

    ASSERT_EQ_INT(ws_outbox_enqueue(outbox, first, sizeof(first), 0u, 1u),
                  WS_OUTBOX_ADMITTED);
    ASSERT_EQ_INT(ws_outbox_enqueue(outbox, latest, sizeof(latest), 0u, 2u),
                  WS_OUTBOX_COALESCED);
    ASSERT_EQ_INT((int)outbox->frame_count, 1);
    ASSERT_EQ_INT((int)outbox->stats.coalesced_packets, 1);

    uint8_t scratch[WS_OUTBOX_MAX_FRAME_BYTES];
    outbox_capture_t capture = {0};
    ASSERT_EQ_INT((int)ws_outbox_pump(
                      outbox, 0u, WS_OUTBOX_TRANSPORT_LIMIT_BYTES, 3u,
                      scratch, sizeof(scratch), capture_send, &capture),
                  1);
    ASSERT_EQ_INT((int)capture.count, 1);
    ASSERT_EQ_INT((int)capture.last_len, (int)sizeof(latest));
    ASSERT_EQ_INT(capture.last_payload[2], 2);
    ASSERT_EQ_INT(capture.last_payload[499], 0xA5);
    ASSERT_EQ_INT((int)outbox->frame_count, 0);
    free(outbox);
}

TEST(test_ws_outbox_coalesces_tow_snapshot_without_displacing_control) {
    ws_outbox_t *outbox = test_outbox_new();
    ASSERT(outbox != NULL);
    uint8_t first[] = {
        NET_MSG_WORLD_TOW_LINKS, 1u, 0u, 0x11u
    };
    uint8_t latest[] = {
        NET_MSG_WORLD_TOW_LINKS, 2u, 0u, 0x22u, 0xA5u
    };
    uint8_t recovery[] = {
        NET_MSG_LEGACY_RECOVERY_RESULT,
        LEGACY_RECOVERY_RESULT_STALE_OFFER
    };

    ASSERT_EQ_INT(ws_outbox_enqueue(
                      outbox, first, sizeof(first), 0u, 1u),
                  WS_OUTBOX_ADMITTED);
    ASSERT_EQ_INT(ws_outbox_enqueue(
                      outbox, recovery, sizeof(recovery), 0u, 2u),
                  WS_OUTBOX_ADMITTED);
    ASSERT_EQ_INT(ws_outbox_enqueue(
                      outbox, latest, sizeof(latest), 0u, 3u),
                  WS_OUTBOX_COALESCED);
    ASSERT_EQ_INT((int)outbox->frame_count, 2);
    ASSERT_EQ_INT((int)outbox->stats.coalesced_packets, 1);

    uint8_t scratch[WS_OUTBOX_MAX_FRAME_BYTES];
    outbox_capture_t capture = {0};
    ASSERT_EQ_INT((int)ws_outbox_pump(
                      outbox, 0u, WS_OUTBOX_TRANSPORT_LIMIT_BYTES, 4u,
                      scratch, sizeof(scratch), capture_send, &capture),
                  2);
    ASSERT_EQ_INT((int)capture.count, 2);
    ASSERT_EQ_INT(capture.types[0],
                  NET_MSG_LEGACY_RECOVERY_RESULT);
    ASSERT_EQ_INT(capture.types[1], NET_MSG_WORLD_TOW_LINKS);
    ASSERT_EQ_INT((int)capture.last_len, (int)sizeof(latest));
    ASSERT(memcmp(capture.last_payload, latest,
                  sizeof(latest)) == 0);
    ASSERT_EQ_INT((int)outbox->frame_count, 0);
    free(outbox);
}

TEST(test_ws_outbox_normal_limit_preserves_control_reserve) {
    ws_outbox_t *outbox = test_outbox_new();
    ASSERT(outbox != NULL);
    uint8_t normal[1020] = {0};
    normal[0] = NET_MSG_STATION_MANIFEST;

    /* 1020-byte payload + 4-byte WS header consumes exactly four pages. */
    for (int i = 0; i < WS_OUTBOX_NORMAL_PAGE_COUNT / 4; i++) {
        normal[1] = (uint8_t)i;
        ASSERT_EQ_INT(
            ws_outbox_enqueue(outbox, normal, sizeof(normal), 0u,
                              (uint64_t)i),
            WS_OUTBOX_ADMITTED);
    }
    ASSERT_EQ_INT((int)outbox->stats.queue_wire_bytes,
                  WS_OUTBOX_NORMAL_LIMIT_BYTES);

    normal[1] = 250u;
    ASSERT_EQ_INT(ws_outbox_enqueue(outbox, normal, sizeof(normal), 0u,
                                    1000u),
                  WS_OUTBOX_SUPPRESSED);
    ASSERT(ws_outbox_needs_resync(outbox));
    ASSERT(ws_outbox_should_suppress(outbox, 0u));

    /* The action-time receipt fast path uses this exact pressure gate. It
     * skips the CONTROL enqueue, leaving the durable transaction/connection
     * intact for the paced post-manifest replay. */
    uint8_t receipt_bundle[] = {
        NET_MSG_CARGO_RECEIPT_BUNDLE, 1u, 0u,
    };
    size_t frames_before_receipt = outbox->frame_count;
    if (!ws_outbox_should_suppress(outbox, 0u)) {
        (void)ws_outbox_enqueue(
            outbox, receipt_bundle, sizeof(receipt_bundle),
            0u, 1001u);
    }
    ASSERT_EQ_INT(
        (int)outbox->frame_count,
        (int)frames_before_receipt);
    ASSERT_EQ_INT(outbox->close_reason, WS_OUTBOX_CLOSE_NONE);

    uint8_t challenge[PUBKEY_CHALLENGE_MSG_SIZE] = {
        NET_MSG_PUBKEY_CHALLENGE
    };
    ASSERT_EQ_INT(ws_outbox_enqueue(outbox, challenge, sizeof(challenge), 0u,
                                    1002u),
                  WS_OUTBOX_ADMITTED);

    uint8_t ack[8000] = {NET_MSG_ACTION_ACK};
    ASSERT_EQ_INT(ws_outbox_enqueue(outbox, ack, sizeof(ack), 0u, 1003u),
                  WS_OUTBOX_ADMITTED);
    ASSERT(ws_outbox_total_bytes(outbox, 0u) <=
           WS_OUTBOX_APP_HARD_BYTES);
    ASSERT_EQ_INT(outbox->close_reason, WS_OUTBOX_CLOSE_NONE);

    uint8_t *scratch = malloc(WS_OUTBOX_MAX_FRAME_BYTES);
    ASSERT(scratch != NULL);
    outbox_capture_t capture = {0};
    while (ws_outbox_pump(
               outbox, 0u, WS_OUTBOX_TRANSPORT_LIMIT_BYTES,
               1010u, scratch, WS_OUTBOX_MAX_FRAME_BYTES,
               capture_send, &capture) > 0u) {
    }
    ASSERT(!ws_outbox_should_suppress(outbox, 0u));
    ASSERT_EQ_INT(
        ws_outbox_enqueue(
            outbox, receipt_bundle, sizeof(receipt_bundle),
            0u, 1011u),
        WS_OUTBOX_ADMITTED);
    ASSERT_EQ_INT(outbox->close_reason, WS_OUTBOX_CLOSE_NONE);
    free(scratch);
    free(outbox);
}

TEST(test_ws_outbox_control_probe_handles_descriptor_saturation) {
    ws_outbox_t *outbox = test_outbox_new();
    ASSERT(outbox != NULL);

    uint8_t tiny_control[] = {NET_MSG_ACTION_ACK};
    for (int i = 0; i < WS_OUTBOX_MAX_FRAMES; i++) {
        ASSERT_EQ_INT(
            ws_outbox_enqueue(
                outbox, tiny_control, sizeof(tiny_control),
                0u, (uint64_t)i),
            WS_OUTBOX_ADMITTED);
    }
    ASSERT_EQ_INT(
        (int)outbox->frame_count, WS_OUTBOX_MAX_FRAMES);
    ASSERT(!ws_outbox_should_suppress(outbox, 0u));
    ASSERT_EQ_INT(outbox->close_reason, WS_OUTBOX_CLOSE_NONE);

    uint8_t receipt_bundle[] = {
        NET_MSG_CARGO_RECEIPT_BUNDLE, 1u, 0u,
    };
    ASSERT(!ws_outbox_can_admit_control_frame(
        outbox, receipt_bundle, sizeof(receipt_bundle), 0u));

    /* The receipt path treats a failed probe as a best-effort skip. */
    size_t frames_before_receipt = outbox->frame_count;
    if (ws_outbox_can_admit_control_frame(
            outbox, receipt_bundle, sizeof(receipt_bundle), 0u)) {
        (void)ws_outbox_enqueue(
            outbox, receipt_bundle, sizeof(receipt_bundle),
            0u, 1000u);
    }
    ASSERT_EQ_INT(
        (int)outbox->frame_count, (int)frames_before_receipt);
    ASSERT_EQ_INT(outbox->close_reason, WS_OUTBOX_CLOSE_NONE);

    uint8_t *scratch = malloc(WS_OUTBOX_MAX_FRAME_BYTES);
    ASSERT(scratch != NULL);
    outbox_capture_t capture = {0};
    ASSERT_EQ_INT(
        (int)ws_outbox_pump(
            outbox, 0u, WS_OUTBOX_TRANSPORT_LIMIT_BYTES,
            1001u, scratch, WS_OUTBOX_MAX_FRAME_BYTES,
            capture_send, &capture),
        WS_OUTBOX_MAX_FRAMES);
    ASSERT_EQ_INT((int)outbox->frame_count, 0);
    ASSERT(ws_outbox_can_admit_control_frame(
        outbox, receipt_bundle, sizeof(receipt_bundle), 0u));
    ASSERT_EQ_INT(
        ws_outbox_enqueue(
            outbox, receipt_bundle, sizeof(receipt_bundle),
            0u, 1002u),
        WS_OUTBOX_ADMITTED);
    ASSERT_EQ_INT(outbox->close_reason, WS_OUTBOX_CLOSE_NONE);

    free(scratch);
    free(outbox);
}

TEST(test_ws_outbox_control_headroom_exhaustion_is_explicit) {
    ws_outbox_t *outbox = test_outbox_new();
    ASSERT(outbox != NULL);
    uint8_t normal[1020] = {NET_MSG_STATION_MANIFEST};
    for (int i = 0; i < WS_OUTBOX_NORMAL_PAGE_COUNT / 4; i++) {
        normal[1] = (uint8_t)i;
        ASSERT_EQ_INT(ws_outbox_enqueue(outbox, normal, sizeof(normal), 0u,
                                        (uint64_t)i),
                      WS_OUTBOX_ADMITTED);
    }
    uint8_t ack[8200] = {NET_MSG_ACTION_ACK};
    ASSERT_EQ_INT(ws_outbox_enqueue(outbox, ack, sizeof(ack), 0u, 1000u),
                  WS_OUTBOX_ADMITTED);
    ASSERT_EQ_INT(ws_outbox_enqueue(outbox, ack, sizeof(ack), 0u, 1001u),
                  WS_OUTBOX_FATAL);
    ASSERT_EQ_INT(outbox->close_reason,
                  WS_OUTBOX_CLOSE_CONTROL_HEADROOM_EXHAUSTED);
    ASSERT_EQ_INT((int)outbox->stats.critical_failures, 1);
    ASSERT_STR_EQ(ws_outbox_close_reason_name(outbox->close_reason),
                  "control_headroom_exhausted");
    free(outbox);
}

TEST(test_ws_outbox_control_overtakes_normal_without_displacement) {
    ws_outbox_t *outbox = test_outbox_new();
    ASSERT(outbox != NULL);
    uint8_t snapshot[] = {NET_MSG_WORLD_TIME, 1u, 2u, 3u, 4u};
    uint8_t unknown[] = {0xFEu, 8u};
    uint8_t ack[] = {NET_MSG_ACTION_RESULT, 9u};
    ASSERT_EQ_INT(ws_outbox_enqueue(outbox, snapshot, sizeof(snapshot), 0u,
                                    0u),
                  WS_OUTBOX_ADMITTED);
    ASSERT_EQ_INT(ws_outbox_enqueue(outbox, unknown, sizeof(unknown), 0u,
                                    1u),
                  WS_OUTBOX_ADMITTED);
    ASSERT_EQ_INT(ws_outbox_enqueue(outbox, ack, sizeof(ack), 0u, 2u),
                  WS_OUTBOX_ADMITTED);

    uint8_t scratch[WS_OUTBOX_MAX_FRAME_BYTES];
    outbox_capture_t capture = {0};
    ASSERT_EQ_INT((int)ws_outbox_pump(
                      outbox, 0u, WS_OUTBOX_TRANSPORT_LIMIT_BYTES, 3u,
                      scratch, sizeof(scratch), capture_send, &capture),
                  2);
    ASSERT_EQ_INT((int)capture.count, 2);
    ASSERT_EQ_INT(capture.types[0], NET_MSG_ACTION_RESULT);
    ASSERT_EQ_INT(capture.types[1], NET_MSG_WORLD_TIME);
    ASSERT_EQ_INT((int)outbox->frame_count, 1);
    free(outbox);
}

TEST(test_ws_outbox_large_frame_and_reliable_overflow_fail_closed) {
    ws_outbox_t *outbox = test_outbox_new();
    ASSERT(outbox != NULL);
    uint8_t *too_large = calloc(1u, WS_OUTBOX_MAX_FRAME_BYTES + 1u);
    ASSERT(too_large != NULL);
    too_large[0] = NET_MSG_WORLD_TIME;
    ASSERT_EQ_INT(ws_outbox_enqueue(
                      outbox, too_large, WS_OUTBOX_MAX_FRAME_BYTES + 1u,
                      0u, 0u),
                  WS_OUTBOX_FATAL);
    ASSERT_EQ_INT(outbox->close_reason, WS_OUTBOX_CLOSE_FRAME_TOO_LARGE);
    free(too_large);
    free(outbox);

    outbox = test_outbox_new();
    ASSERT(outbox != NULL);
    uint8_t normal[1020] = {NET_MSG_STATION_MANIFEST};
    for (int i = 0; i < WS_OUTBOX_NORMAL_PAGE_COUNT / 4; i++) {
        normal[1] = (uint8_t)i;
        ASSERT_EQ_INT(ws_outbox_enqueue(outbox, normal, sizeof(normal), 0u,
                                        (uint64_t)i),
                      WS_OUTBOX_ADMITTED);
    }
    uint8_t future[] = {0xFEu};
    ASSERT_EQ_INT(ws_outbox_enqueue(outbox, future, sizeof(future), 0u,
                                    1000u),
                  WS_OUTBOX_FATAL);
    ASSERT_EQ_INT(outbox->close_reason, WS_OUTBOX_CLOSE_QUEUE_HARD_LIMIT);
    ASSERT_EQ_INT((int)outbox->stats.critical_failures, 1);
    free(outbox);
}

TEST(test_ws_outbox_pressure_warns_recovers_and_times_out) {
    ws_outbox_t *outbox = test_outbox_new();
    ASSERT(outbox != NULL);
    uint8_t *large = calloc(1u, 70000u);
    ASSERT(large != NULL);
    large[0] = NET_MSG_STATION_MANIFEST;
    large[1] = 1u;
    ASSERT_EQ_INT(ws_outbox_enqueue(outbox, large, 70000u, 0u, 0u),
                  WS_OUTBOX_ADMITTED);
    large[1] = 2u;
    ASSERT_EQ_INT(ws_outbox_enqueue(outbox, large, 70000u, 0u, 1u),
                  WS_OUTBOX_ADMITTED);
    ASSERT(ws_outbox_should_suppress(outbox, 0u));
    ASSERT_EQ_INT(ws_outbox_check_timeouts(outbox, 2001u, 0u),
                  WS_OUTBOX_CLOSE_NONE);
    ASSERT_EQ_INT((int)outbox->stats.warning_events, 1);
    ASSERT_EQ_INT(ws_outbox_check_timeouts(
                      outbox, WS_OUTBOX_NO_PROGRESS_MS + 1u, 0u),
                  WS_OUTBOX_CLOSE_NO_WRITE_PROGRESS);
    free(large);
    free(outbox);

    outbox = test_outbox_new();
    ASSERT(outbox != NULL);
    large = calloc(1u, 70000u);
    ASSERT(large != NULL);
    large[0] = NET_MSG_STATION_MANIFEST;
    large[1] = 1u;
    ASSERT_EQ_INT(ws_outbox_enqueue(outbox, large, 70000u, 0u, 0u),
                  WS_OUTBOX_ADMITTED);
    uint8_t scratch[WS_OUTBOX_MAX_FRAME_BYTES];
    outbox_capture_t capture = {0};
    ASSERT_EQ_INT((int)ws_outbox_pump(
                      outbox, 0u, WS_OUTBOX_TRANSPORT_LIMIT_BYTES, 10u,
                      scratch, sizeof(scratch), capture_send, &capture),
                  1);
    ASSERT_EQ_INT(ws_outbox_check_timeouts(outbox, 11u, 0u),
                  WS_OUTBOX_CLOSE_NONE);
    ASSERT_EQ_INT((int)outbox->stats.recovery_events, 1);
    /* Crossing the soft watermark and draining without dropping a frame
     * needs no replay. Actual suppressed replaceable frames set this bit. */
    ASSERT(!ws_outbox_needs_resync(outbox));
    ws_outbox_note_suppressed(outbox, sizeof(*large));
    ASSERT(ws_outbox_needs_resync(outbox));
    free(large);
    free(outbox);
}

TEST(test_ws_initial_sync_pacer_is_burst_and_rate_bounded) {
    ws_sync_pacer_t pacer;
    ws_sync_pacer_init(&pacer, 0u);
    ASSERT(ws_sync_pacer_can_send(
        &pacer, 0u, WS_OUTBOX_SYNC_BURST_BYTES));
    ASSERT(ws_sync_pacer_can_send(
        &pacer, 0u, WS_OUTBOX_SYNC_BURST_BYTES));
    ASSERT(ws_sync_pacer_charge(
        &pacer, 0u, WS_OUTBOX_SYNC_BURST_BYTES));
    ASSERT(!ws_sync_pacer_can_send(&pacer, 0u, 1u));

    ws_sync_pacer_init(&pacer, 0u);
    ASSERT(ws_sync_pacer_allow(&pacer, 0u,
                               WS_OUTBOX_SYNC_BURST_BYTES));
    ASSERT(!ws_sync_pacer_allow(&pacer, 0u, 1u));
    ASSERT(!ws_sync_pacer_allow(&pacer, 39u, 21u * 1024u));
    ASSERT(ws_sync_pacer_allow(&pacer, 40u, 20u * 1024u));
    ASSERT(!ws_sync_pacer_allow(&pacer, 40u, 1024u));
    ASSERT(ws_sync_pacer_allow(&pacer, 1040u,
                               WS_OUTBOX_SYNC_BURST_BYTES));
}

TEST(test_ws_initial_sync_substeps_reconcile_once_without_loop) {
    ws_initial_sync_t sync;
    ws_initial_sync_begin(&sync, 100u, 0u);
    ASSERT_EQ_INT(ws_initial_sync_current(&sync, NULL),
                  WS_INITIAL_SYNC_PRIVATE);
    for (int i = 0; i < 6; i++) {
        ws_initial_sync_commit_substep(&sync, 7u, 101u + (uint64_t)i);
        ASSERT_EQ_INT(ws_initial_sync_current(&sync, NULL),
                      WS_INITIAL_SYNC_PRIVATE);
        ASSERT_EQ_INT(ws_initial_sync_substep(&sync), i + 1);
    }
    ws_initial_sync_commit_substep(&sync, 7u, 107u);
    ASSERT_EQ_INT(ws_initial_sync_current(&sync, NULL),
                  WS_INITIAL_SYNC_WORLD_STATIONS);

    int guard = 0;
    while (ws_initial_sync_active(&sync) && guard++ < 16)
        ws_initial_sync_commit(&sync, 200u + (uint64_t)guard);
    ASSERT(!ws_initial_sync_active(&sync));
    ASSERT_EQ_INT(ws_initial_sync_current(&sync, NULL),
                  WS_INITIAL_SYNC_NONE);
    ASSERT(ws_initial_sync_take_reconcile(&sync));
    ASSERT(!ws_initial_sync_take_reconcile(&sync));
    ASSERT(!ws_initial_sync_active(&sync));
}

TEST(test_ws_initial_sync_catchup_delivers_latest_station_mutation_once) {
    ws_outbox_t *outbox = test_outbox_new();
    ASSERT(outbox != NULL);
    ws_initial_sync_t sync;
    ws_initial_sync_begin(&sync, 100u, 1u);

    ws_initial_sync_commit_substep(&sync, 1u, 101u);
    ASSERT_EQ_INT(ws_initial_sync_current(&sync, NULL),
                  WS_INITIAL_SYNC_STATION_IDENTITY);
    ws_initial_sync_commit(&sync, 102u);
    ws_initial_sync_commit(&sync, 103u);

    uint8_t initial[] = {
        NET_MSG_STATION_MANIFEST, 0u, 1u
    };
    ASSERT_EQ_INT(ws_outbox_enqueue(
                      outbox, initial, sizeof(initial), 0u, 104u),
                  WS_OUTBOX_ADMITTED);
    ws_initial_sync_commit(&sync, 104u);

    /* The station mutates after its first manifest step, and that update is
     * initially suppressed. The terminal catch-up must not clear resync yet. */
    uint64_t station_generation = 2u;
    ws_outbox_note_suppressed(outbox, sizeof(initial));
    while (ws_initial_sync_current(&sync, NULL) !=
           WS_INITIAL_SYNC_CATCHUP) {
        ws_initial_sync_commit(&sync, 105u);
    }
    ASSERT(ws_initial_sync_catchup_needs_snapshot(&sync));

    uint8_t catchup[] = {
        NET_MSG_STATION_MANIFEST, 0u, 2u
    };
    ASSERT_EQ_INT(ws_outbox_enqueue(
                      outbox, catchup, sizeof(catchup), 0u, 106u),
                  WS_OUTBOX_COALESCED);
    ws_initial_sync_catchup_note_snapshot(
        &sync, outbox->stats.suppressed_packets, station_generation);

    /* A second mutation during the catch-up boundary invalidates only that
     * boundary. One retry coalesces the latest revision and then completes. */
    station_generation++;
    ws_outbox_note_suppressed(outbox, sizeof(catchup));
    ASSERT(!ws_initial_sync_catchup_snapshot_current(
        &sync, outbox->stats.suppressed_packets, station_generation));
    ASSERT(ws_initial_sync_catchup_needs_snapshot(&sync));
    catchup[2] = 3u;
    ASSERT_EQ_INT(ws_outbox_enqueue(
                      outbox, catchup, sizeof(catchup), 0u, 107u),
                  WS_OUTBOX_COALESCED);
    ws_initial_sync_catchup_note_snapshot(
        &sync, outbox->stats.suppressed_packets, station_generation);

    uint8_t scratch[WS_OUTBOX_MAX_FRAME_BYTES];
    outbox_capture_t capture = {0};
    ASSERT_EQ_INT((int)ws_outbox_pump(
                      outbox, 0u, WS_OUTBOX_TRANSPORT_LIMIT_BYTES, 108u,
                      scratch, sizeof(scratch), capture_send, &capture),
                  1);
    ASSERT_EQ_INT((int)capture.count, 1);
    ASSERT_EQ_INT(capture.last_payload[2], 3);
    ASSERT(ws_initial_sync_catchup_snapshot_current(
        &sync, outbox->stats.suppressed_packets, station_generation));

    ws_outbox_mark_resynced(outbox);
    ws_initial_sync_commit(&sync, 109u);
    ASSERT(!ws_initial_sync_active(&sync));
    ASSERT(!ws_outbox_needs_resync(outbox));
    ASSERT(ws_initial_sync_take_reconcile(&sync));
    ASSERT(!ws_initial_sync_take_reconcile(&sync));
    ws_initial_sync_commit(&sync, 110u);
    ASSERT(!ws_initial_sync_active(&sync));
    free(outbox);
}

TEST(test_ws_replication_schedule_prioritizes_private_before_bulk) {
    ws_replication_cycle_t order[2] = {0};
    ASSERT_EQ_INT((int)ws_replication_cycle_order(
                      true, true, order),
                  2);
    ASSERT_EQ_INT(order[0], WS_REPLICATION_CYCLE_PRIVATE);
    ASSERT_EQ_INT(order[1], WS_REPLICATION_CYCLE_BULK);

    ASSERT_EQ_INT((int)ws_replication_cycle_order(
                      false, true, order),
                  1);
    ASSERT_EQ_INT(order[0], WS_REPLICATION_CYCLE_BULK);
    ASSERT_EQ_INT((int)ws_replication_cycle_order(
                      true, false, order),
                  1);
    ASSERT_EQ_INT(order[0], WS_REPLICATION_CYCLE_PRIVATE);
    ASSERT_EQ_INT((int)ws_replication_cycle_order(
                      false, false, order),
                  0);
}

TEST(test_ws_outbox_reliable_batch_preflight_is_atomic) {
    ws_outbox_t *outbox = test_outbox_new();
    ASSERT(outbox != NULL);
    uint8_t asteroid[1020] = {NET_MSG_WORLD_ASTEROIDS};
    uint8_t asteroid_q[1020] = {NET_MSG_WORLD_ASTEROIDS_Q};
    uint8_t asteroid8_q[1020] = {NET_MSG_WORLD_ASTEROIDS8_Q};
    const uint8_t *payloads[] = {
        asteroid, asteroid_q, asteroid8_q
    };
    const size_t lengths[] = {
        sizeof(asteroid), sizeof(asteroid_q), sizeof(asteroid8_q)
    };

    ASSERT(ws_outbox_can_admit_reliable_batch(
        outbox, payloads, lengths, 3u, 0u));
    ASSERT_EQ_INT((int)outbox->frame_count, 0);
    ASSERT_EQ_INT((int)outbox->stats.queue_wire_bytes, 0);

    bool baseline_committed = false;
    ASSERT(!ws_outbox_can_admit_reliable_batch(
        outbox, payloads, lengths, 3u,
        WS_OUTBOX_NORMAL_LIMIT_BYTES));
    ASSERT(!baseline_committed);
    ASSERT_EQ_INT((int)outbox->frame_count, 0);
    ASSERT_EQ_INT((int)outbox->stats.queue_wire_bytes, 0);

    for (size_t i = 0u; i < 3u; i++) {
        ASSERT_EQ_INT(ws_outbox_enqueue(
                          outbox, payloads[i], lengths[i], 0u, i),
                      WS_OUTBOX_ADMITTED);
    }
    baseline_committed = true;
    ASSERT(baseline_committed);
    ASSERT_EQ_INT((int)outbox->frame_count, 3);
    free(outbox);
}

TEST(test_mongoose_ws_send_limit_is_atomic) {
    struct mg_connection c;
    memset(&c, 0, sizeof(c));
    ASSERT(mg_iobuf_init(&c.send, 0u, 16u));
    c.send_limit = 100u;

    uint8_t exact[98] = {0};
    exact[0] = 0xA5u;
    ASSERT_EQ_INT((int)mg_ws_send(
                      &c, exact, sizeof(exact), WEBSOCKET_OP_BINARY),
                  100);
    ASSERT_EQ_INT((int)c.send.len, 100);
    ASSERT_EQ_INT(c.send.buf[0], 0x82);
    ASSERT_EQ_INT(c.send.buf[1], 98);
    ASSERT_EQ_INT(c.send.buf[2], 0xA5);

    uint8_t before[100];
    memcpy(before, c.send.buf, sizeof(before));
    uint8_t overflow[] = {1u};
    ASSERT_EQ_INT((int)mg_ws_send(
                      &c, overflow, sizeof(overflow),
                      WEBSOCKET_OP_BINARY),
                  0);
    ASSERT_EQ_INT((int)c.send.len, 100);
    ASSERT(memcmp(before, c.send.buf, sizeof(before)) == 0);
    mg_iobuf_free(&c.send);

    memset(&c, 0, sizeof(c));
    ASSERT(mg_iobuf_init(&c.send, 0u, 16u));
    c.send_limit = 5u;
    uint8_t four[] = {1u, 2u, 3u, 4u};
    ASSERT_EQ_INT((int)mg_ws_send(
                      &c, four, sizeof(four), WEBSOCKET_OP_BINARY),
                  0);
    ASSERT_EQ_INT((int)c.send.len, 0);
    mg_iobuf_free(&c.send);

    /*
     * Application pumping stops 1 KiB below Mongoose's hard boundary, so a
     * maximum legal PONG can still be framed atomically under pressure.
     */
    memset(&c, 0, sizeof(c));
    ASSERT(mg_iobuf_init(&c.send, 0u, 16u));
    c.send_limit = WS_OUTBOX_TRANSPORT_HARD_LIMIT_BYTES;
    uint8_t *prefill = calloc(
        1u, WS_OUTBOX_TRANSPORT_LIMIT_BYTES);
    ASSERT(prefill != NULL);
    ASSERT(mg_send(&c, prefill,
                   WS_OUTBOX_TRANSPORT_LIMIT_BYTES));
    ASSERT_EQ_INT((int)c.send.len,
                  WS_OUTBOX_TRANSPORT_LIMIT_BYTES);
    uint8_t pong[125] = {0};
    ASSERT_EQ_INT((int)mg_ws_send(
                      &c, pong, sizeof(pong), WEBSOCKET_OP_PONG),
                  127);
    ASSERT(c.send.len <= WS_OUTBOX_TRANSPORT_HARD_LIMIT_BYTES);
    free(prefill);
    mg_iobuf_free(&c.send);
}

TEST(test_mongoose_ws_control_validation_bounds_auto_replies) {
    ASSERT(mg_ws_control_frame_valid(
        0x80u | WEBSOCKET_OP_PING, 125u));
    ASSERT(!mg_ws_control_frame_valid(
        0x80u | WEBSOCKET_OP_PING, 126u));
    ASSERT(!mg_ws_control_frame_valid(WEBSOCKET_OP_PING, 1u));
    ASSERT(!mg_ws_control_frame_valid(
        0x80u | WEBSOCKET_OP_CLOSE, 1u));
    ASSERT(mg_ws_control_frame_valid(
        0x80u | WEBSOCKET_OP_CLOSE, 2u));
    ASSERT(!mg_ws_control_frame_valid(0x80u | 0x0bu, 0u));
    ASSERT(!mg_ws_control_frame_valid(0x80u | 0x0fu, 0u));
    ASSERT(mg_ws_control_frame_valid(WEBSOCKET_OP_BINARY, 4096u));
}

enum {
    VIRTUAL_CLIENTS = 32,
    VIRTUAL_DURATION_MS = 5 * 60 * 1000,
    VIRTUAL_POLL_MS = 8,
    VIRTUAL_MANIFEST_BYTES = 20686,
    VIRTUAL_IDENTITY_BYTES = 2048,
    VIRTUAL_DIAG_BYTES = 64,
    VIRTUAL_GLOBAL_SNAPSHOT_BYTES = 4096,
    VIRTUAL_PRIVATE_PACKET_BYTES = 2048,
    VIRTUAL_PRIVATE_PACKET_COUNT =
        SERVER_INITIAL_PRIVATE_SNAPSHOT_PACKET_COUNT,
    VIRTUAL_ASTEROID_BYTES = WS_OUTBOX_SYNC_BURST_BYTES - 10,
    VIRTUAL_HEALTHY_DRAIN_BYTES = 64 * 1024,
    VIRTUAL_LATENCY_BUCKETS = 65
};

typedef struct {
    ws_outbox_t *outbox;
    ws_initial_sync_t initial_sync;
    size_t transport_bytes;
    size_t max_transport_bytes;
    size_t max_total_bytes;
    uint64_t latency_hist[VIRTUAL_LATENCY_BUCKETS];
    uint64_t latency_samples;
    uint64_t now_ms;
    uint64_t initial_complete_ms;
    uint64_t next_ack_ms;
    uint64_t next_mutation_ms;
    uint8_t mutation_station;
    bool reader;
    bool disconnected;
} virtual_peer_t;

typedef struct {
    uint64_t sim_ticks;
    uint64_t max_tick_gap_ms;
    uint64_t healthy_latency_hist[VIRTUAL_LATENCY_BUCKETS];
    uint64_t healthy_latency_samples;
    size_t max_fixed_capacity_bytes;
    size_t max_slow_queue_bytes;
    size_t max_slow_transport_bytes;
    uint64_t slow_disconnect_ms;
    ws_outbox_close_reason_t slow_reason;
    uint64_t coalesced_packets;
    uint64_t suppressed_packets;
    uint64_t healthy_initial_completed;
    uint64_t earliest_healthy_initial_complete_ms;
} virtual_run_result_t;

static uint32_t virtual_read_u32(const uint8_t *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void virtual_write_u32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static bool virtual_send(void *user, const uint8_t *payload,
                         size_t payload_len) {
    virtual_peer_t *peer = user;
    if (!peer || !payload || payload_len == 0u) return false;
    size_t wire = test_ws_wire_size(payload_len);
    if (wire > WS_OUTBOX_TRANSPORT_HARD_LIMIT_BYTES ||
        peer->transport_bytes >
            WS_OUTBOX_TRANSPORT_HARD_LIMIT_BYTES - wire) {
        return false;
    }
    peer->transport_bytes += wire;
    if (peer->transport_bytes > peer->max_transport_bytes)
        peer->max_transport_bytes = peer->transport_bytes;

    if (payload[0] == NET_MSG_ACTION_ACK && payload_len >= 5u) {
        uint32_t enqueued_ms = virtual_read_u32(&payload[1]);
        uint64_t latency = peer->now_ms - enqueued_ms;
        size_t bucket = latency < VIRTUAL_LATENCY_BUCKETS - 1u
            ? (size_t)latency : VIRTUAL_LATENCY_BUCKETS - 1u;
        peer->latency_hist[bucket]++;
        peer->latency_samples++;
    }
    return true;
}

static uint64_t virtual_percentile(
    const uint64_t histogram[VIRTUAL_LATENCY_BUCKETS],
    uint64_t samples,
    uint64_t percentile) {
    if (samples == 0u) return 0u;
    uint64_t rank = (samples * percentile + 99u) / 100u;
    uint64_t seen = 0u;
    for (uint64_t i = 0u; i < VIRTUAL_LATENCY_BUCKETS; i++) {
        seen += histogram[i];
        if (seen >= rank) return i;
    }
    return VIRTUAL_LATENCY_BUCKETS - 1u;
}

static bool virtual_run(bool one_non_reader, virtual_run_result_t *result) {
    if (!result) return false;
    memset(result, 0, sizeof(*result));
    result->earliest_healthy_initial_complete_ms = UINT64_MAX;

    ws_outbox_t *outboxes =
        calloc(VIRTUAL_CLIENTS, sizeof(*outboxes));
    virtual_peer_t *peers =
        calloc(VIRTUAL_CLIENTS, sizeof(*peers));
    uint8_t *manifest = calloc(1u, WS_OUTBOX_MAX_FRAME_BYTES);
    uint8_t *scratch = calloc(1u, WS_OUTBOX_MAX_FRAME_BYTES);
    if (!outboxes || !peers || !manifest || !scratch) {
        free(outboxes);
        free(peers);
        free(manifest);
        free(scratch);
        return false;
    }
    manifest[0] = NET_MSG_STATION_MANIFEST;

    for (int i = 0; i < VIRTUAL_CLIENTS; i++) {
        peers[i].outbox = &outboxes[i];
        peers[i].reader = !(one_non_reader && i == VIRTUAL_CLIENTS - 1);
        peers[i].next_mutation_ms = 1000u;
        ws_outbox_init(peers[i].outbox, 0u);
        ws_initial_sync_begin(
            &peers[i].initial_sync, 0u, MAX_STATIONS);
    }

    uint64_t sim_accumulator = 0u;
    uint64_t last_sim_tick_ms = 0u;
    for (uint64_t step = 0u;
         step < VIRTUAL_DURATION_MS / VIRTUAL_POLL_MS; step++) {
        uint64_t now_ms = step * VIRTUAL_POLL_MS;
        sim_accumulator += VIRTUAL_POLL_MS * 120u;
        while (sim_accumulator >= 1000u) {
            sim_accumulator -= 1000u;
            result->sim_ticks++;
            if (last_sim_tick_ms != 0u &&
                now_ms - last_sim_tick_ms > result->max_tick_gap_ms) {
                result->max_tick_gap_ms = now_ms - last_sim_tick_ms;
            }
            last_sim_tick_ms = now_ms;
        }

        size_t fixed_capacity_now = 0u;
        for (int i = 0; i < VIRTUAL_CLIENTS; i++) {
            virtual_peer_t *peer = &peers[i];
            peer->now_ms = now_ms;
            if (peer->disconnected) continue;

            if (peer->reader && peer->transport_bytes > 0u) {
                size_t drained =
                    peer->transport_bytes < VIRTUAL_HEALTHY_DRAIN_BYTES
                    ? peer->transport_bytes
                    : VIRTUAL_HEALTHY_DRAIN_BYTES;
                peer->transport_bytes -= drained;
                ws_outbox_note_write_progress(
                    peer->outbox, now_ms, drained);
            }

            uint16_t initial_station = 0u;
            ws_initial_sync_step_t initial_step =
                ws_initial_sync_current(
                    &peer->initial_sync, &initial_station);
            if (initial_step != WS_INITIAL_SYNC_NONE) {
                size_t initial_len = VIRTUAL_GLOBAL_SNAPSHOT_BYTES;
                uint8_t initial_type = NET_MSG_WORLD_STATIONS;
                bool private_substep = false;
                if (initial_step == WS_INITIAL_SYNC_PRIVATE) {
                    initial_len = VIRTUAL_PRIVATE_PACKET_BYTES;
                    initial_type = NET_MSG_PLAYER_SHIP;
                    private_substep = true;
                } else if (initial_step ==
                           WS_INITIAL_SYNC_STATION_IDENTITY) {
                    initial_len = VIRTUAL_IDENTITY_BYTES;
                    initial_type = NET_MSG_STATION_IDENTITY;
                } else if (initial_step == WS_INITIAL_SYNC_STATION_DIAG) {
                    initial_len = VIRTUAL_DIAG_BYTES;
                    initial_type = NET_MSG_STATION_DIAG;
                } else if (initial_step ==
                           WS_INITIAL_SYNC_STATION_MANIFEST) {
                    initial_len = VIRTUAL_MANIFEST_BYTES;
                    initial_type = NET_MSG_STATION_MANIFEST;
                } else if (initial_step == WS_INITIAL_SYNC_CONTRACTS) {
                    initial_type = NET_MSG_CONTRACTS;
                } else if (initial_step == WS_INITIAL_SYNC_ASTEROIDS) {
                    initial_len = VIRTUAL_ASTEROID_BYTES;
                    initial_type = NET_MSG_WORLD_ASTEROIDS;
                } else if (initial_step == WS_INITIAL_SYNC_HIGHSCORES) {
                    initial_type = NET_MSG_HIGHSCORES;
                } else if (initial_step ==
                           WS_INITIAL_SYNC_SIGNAL_CHANNEL) {
                    initial_type = NET_MSG_SIGNAL_CHANNEL;
                }
                size_t initial_wire =
                    ws_outbox_wire_bytes(initial_len);
                if (ws_sync_pacer_allow(
                        &peer->initial_sync.pacer, now_ms,
                        initial_wire)) {
                    manifest[0] = initial_type;
                    manifest[1] = (uint8_t)initial_station;
                    ws_outbox_result_t queued = ws_outbox_enqueue(
                        peer->outbox, manifest, initial_len,
                        peer->transport_bytes, now_ms);
                    if (queued == WS_OUTBOX_ADMITTED ||
                        queued == WS_OUTBOX_COALESCED) {
                        if (private_substep) {
                            ws_initial_sync_commit_substep(
                                &peer->initial_sync,
                                VIRTUAL_PRIVATE_PACKET_COUNT,
                                now_ms);
                        } else {
                            ws_initial_sync_commit(
                                &peer->initial_sync, now_ms);
                        }
                        if (!ws_initial_sync_active(
                                &peer->initial_sync)) {
                            peer->initial_complete_ms = now_ms;
                        }
                    }
                }
            }

            if (now_ms >= peer->next_mutation_ms) {
                manifest[0] = NET_MSG_STATION_MANIFEST;
                manifest[1] = peer->mutation_station++;
                (void)ws_outbox_enqueue(
                    peer->outbox, manifest, VIRTUAL_MANIFEST_BYTES,
                    peer->transport_bytes, now_ms);
                peer->next_mutation_ms += 1000u;
            }

            if (now_ms >= peer->next_ack_ms) {
                uint8_t ack[9] = {NET_MSG_ACTION_ACK};
                virtual_write_u32(&ack[1], (uint32_t)now_ms);
                if (ws_outbox_enqueue(
                        peer->outbox, ack, sizeof(ack),
                        peer->transport_bytes, now_ms) == WS_OUTBOX_FATAL) {
                    peer->disconnected = true;
                }
                peer->next_ack_ms += 250u;
            }

            if (!peer->disconnected && peer->outbox->frame_count > 0u) {
                (void)ws_outbox_pump(
                    peer->outbox, peer->transport_bytes,
                    WS_OUTBOX_TRANSPORT_LIMIT_BYTES, now_ms,
                    scratch, WS_OUTBOX_MAX_FRAME_BYTES,
                    virtual_send, peer);
            }

            ws_outbox_close_reason_t reason = ws_outbox_check_timeouts(
                peer->outbox, now_ms, peer->transport_bytes);
            if (reason != WS_OUTBOX_CLOSE_NONE) {
                peer->disconnected = true;
                if (!peer->reader && result->slow_disconnect_ms == 0u) {
                    result->slow_disconnect_ms = now_ms;
                    result->slow_reason = reason;
                }
            }

            size_t total = ws_outbox_total_bytes(
                peer->outbox, peer->transport_bytes);
            if (total > peer->max_total_bytes)
                peer->max_total_bytes = total;
            fixed_capacity_now +=
                ws_outbox_resident_capacity_bytes() +
                peer->transport_bytes;
            if (!peer->reader) {
                if (peer->outbox->stats.queue_wire_bytes >
                    result->max_slow_queue_bytes) {
                    result->max_slow_queue_bytes =
                        peer->outbox->stats.queue_wire_bytes;
                }
                if (peer->max_transport_bytes >
                    result->max_slow_transport_bytes) {
                    result->max_slow_transport_bytes =
                        peer->max_transport_bytes;
                }
            }
        }
        if (fixed_capacity_now > result->max_fixed_capacity_bytes)
            result->max_fixed_capacity_bytes = fixed_capacity_now;
    }

    bool valid = true;
    for (int i = 0; i < VIRTUAL_CLIENTS; i++) {
        virtual_peer_t *peer = &peers[i];
        result->coalesced_packets +=
            peer->outbox->stats.coalesced_packets;
        result->suppressed_packets +=
            peer->outbox->stats.suppressed_packets;
        if (peer->reader) {
            if (peer->outbox->close_reason != WS_OUTBOX_CLOSE_NONE ||
                peer->initial_complete_ms == 0u) {
                valid = false;
            }
            result->healthy_initial_completed++;
            if (peer->initial_complete_ms <
                result->earliest_healthy_initial_complete_ms) {
                result->earliest_healthy_initial_complete_ms =
                    peer->initial_complete_ms;
            }
            for (int b = 0; b < VIRTUAL_LATENCY_BUCKETS; b++) {
                result->healthy_latency_hist[b] +=
                    peer->latency_hist[b];
            }
            result->healthy_latency_samples += peer->latency_samples;
        }
    }

    free(outboxes);
    free(peers);
    free(manifest);
    free(scratch);
    return valid;
}

TEST(test_ws_outbox_deterministic_capacity_model_32_clients_five_minutes) {
    virtual_run_result_t baseline;
    virtual_run_result_t slow_reader;
    ASSERT(virtual_run(false, &baseline));
    ASSERT(virtual_run(true, &slow_reader));

    ASSERT_EQ_INT((int)baseline.sim_ticks, 36000);
    ASSERT_EQ_INT((int)slow_reader.sim_ticks, 36000);
    ASSERT(baseline.max_tick_gap_ms <= 16u);
    ASSERT(slow_reader.max_tick_gap_ms <= 16u);

    ASSERT_EQ_INT((int)baseline.healthy_initial_completed,
                  VIRTUAL_CLIENTS);
    ASSERT_EQ_INT((int)slow_reader.healthy_initial_completed,
                  VIRTUAL_CLIENTS - 1);
    ASSERT(baseline.earliest_healthy_initial_complete_ms >= 5500u);
    ASSERT(slow_reader.earliest_healthy_initial_complete_ms >= 5500u);

    ASSERT(slow_reader.slow_disconnect_ms > 0u);
    ASSERT(slow_reader.slow_disconnect_ms <=
           WS_OUTBOX_PRESSURE_DISCONNECT_MS);
    ASSERT(slow_reader.slow_reason == WS_OUTBOX_CLOSE_NO_WRITE_PROGRESS ||
           slow_reader.slow_reason ==
               WS_OUTBOX_CLOSE_SUSTAINED_PRESSURE);
    ASSERT(slow_reader.max_slow_queue_bytes <=
           WS_OUTBOX_NORMAL_LIMIT_BYTES);
    ASSERT(slow_reader.max_slow_transport_bytes <=
           WS_OUTBOX_TRANSPORT_HARD_LIMIT_BYTES);
    ASSERT(slow_reader.max_fixed_capacity_bytes <=
           VIRTUAL_CLIENTS *
                (ws_outbox_resident_capacity_bytes() +
                WS_OUTBOX_TRANSPORT_HARD_LIMIT_BYTES));
    ASSERT(slow_reader.coalesced_packets > 0u ||
           slow_reader.suppressed_packets > 0u);

    uint64_t baseline_p95 = virtual_percentile(
        baseline.healthy_latency_hist,
        baseline.healthy_latency_samples, 95u);
    uint64_t baseline_p99 = virtual_percentile(
        baseline.healthy_latency_hist,
        baseline.healthy_latency_samples, 99u);
    uint64_t slow_p95 = virtual_percentile(
        slow_reader.healthy_latency_hist,
        slow_reader.healthy_latency_samples, 95u);
    uint64_t slow_p99 = virtual_percentile(
        slow_reader.healthy_latency_hist,
        slow_reader.healthy_latency_samples, 99u);
    ASSERT(slow_p95 <= baseline_p95 + VIRTUAL_POLL_MS);
    ASSERT(slow_p99 <= baseline_p99 + VIRTUAL_POLL_MS);
    ASSERT(slow_p95 <= 16u);
    ASSERT(slow_p99 <= 16u);
}

TEST(test_ws_backpressure_fixture_requires_explicit_opt_in) {
    ASSERT(!ws_backpressure_fixture_enabled(NULL));
    ASSERT(!ws_backpressure_fixture_enabled(""));
    ASSERT(!ws_backpressure_fixture_enabled("0"));
    ASSERT(!ws_backpressure_fixture_enabled("false"));
    ASSERT(!ws_backpressure_fixture_enabled("FALSE"));
    ASSERT(!ws_backpressure_fixture_enabled("no"));
    ASSERT(!ws_backpressure_fixture_enabled("NO"));
    ASSERT(ws_backpressure_fixture_enabled("1"));
    ASSERT(ws_backpressure_fixture_enabled("yes"));
    ASSERT_EQ_INT(WS_BACKPRESSURE_FIXTURE_STATION_COUNT, 4);
    ASSERT((int)WS_BACKPRESSURE_FIXTURE_STATION_COUNT <=
           (int)SIGNAL_SEEDED_STATION_COUNT);
    ASSERT_EQ_INT(WS_BACKPRESSURE_FIXTURE_STATION0_DETAILS, 240);
    ASSERT((int)WS_BACKPRESSURE_FIXTURE_STATION0_DETAILS <=
           (int)STATION_MANIFEST_DEFAULT_CAP);
    ASSERT((int)WS_BACKPRESSURE_FIXTURE_STATION0_DETAILS <=
           (int)MANIFEST_DETAIL_MAX);
    ASSERT((int)WS_BACKPRESSURE_FIXTURE_OTHER_STATION_FRAMES <=
           (int)STATION_MANIFEST_DEFAULT_CAP);
    ASSERT((int)WS_BACKPRESSURE_FIXTURE_OTHER_STATION_FRAMES <=
           (int)MANIFEST_DETAIL_MAX);
}

void register_ws_outbox_tests(void) {
    TEST_SECTION("\nWebSocket bounded outbox (#663):\n");
    RUN(test_ws_outbox_policy_is_semantic_and_fail_safe);
    RUN(test_ws_outbox_one_shot_receipts_use_control_reserve);
    RUN(test_ws_outbox_never_queues_retired_legacy_save_disclosure);
    RUN(test_ws_outbox_coalesces_latest_semantic_state);
    RUN(test_ws_outbox_coalesces_tow_snapshot_without_displacing_control);
    RUN(test_ws_outbox_normal_limit_preserves_control_reserve);
    RUN(test_ws_outbox_control_probe_handles_descriptor_saturation);
    RUN(test_ws_outbox_control_headroom_exhaustion_is_explicit);
    RUN(test_ws_outbox_control_overtakes_normal_without_displacement);
    RUN(test_ws_outbox_large_frame_and_reliable_overflow_fail_closed);
    RUN(test_ws_outbox_pressure_warns_recovers_and_times_out);
    RUN(test_ws_initial_sync_pacer_is_burst_and_rate_bounded);
    RUN(test_ws_initial_sync_substeps_reconcile_once_without_loop);
    RUN(test_ws_initial_sync_catchup_delivers_latest_station_mutation_once);
    RUN(test_ws_replication_schedule_prioritizes_private_before_bulk);
    RUN(test_ws_outbox_reliable_batch_preflight_is_atomic);
    RUN(test_mongoose_ws_send_limit_is_atomic);
    RUN(test_mongoose_ws_control_validation_bounds_auto_replies);
    RUN(test_ws_outbox_deterministic_capacity_model_32_clients_five_minutes);
    RUN(test_ws_backpressure_fixture_requires_explicit_opt_in);
}
