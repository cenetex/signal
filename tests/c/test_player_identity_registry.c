#include "test_harness.h"

#include "player_identity_registry.h"
#include "signal_memzero.h"

typedef struct {
    uint8_t *data;
    size_t size;
} registry_sensitive_buffer_t;

#ifdef _MSC_VER
#define REGISTRY_TEST_DECL(name) \
    player_identity_registry_t name = {0}
#define REGISTRY_BUFFER_DECL(name) \
    registry_sensitive_buffer_t name = {0}
#else
static void registry_test_auto_cleanup(
    player_identity_registry_t *reg) {
    player_identity_registry_cleanup(reg);
}

static void registry_buffer_auto_cleanup(
    registry_sensitive_buffer_t *buffer) {
    if (!buffer || !buffer->data) return;
    signal_memzero_explicit(buffer->data, buffer->size);
    free(buffer->data);
    buffer->data = NULL;
    buffer->size = 0;
}

#define REGISTRY_TEST_DECL(name) \
    player_identity_registry_t \
        __attribute__((cleanup(registry_test_auto_cleanup))) name = {0}
#define REGISTRY_BUFFER_DECL(name) \
    registry_sensitive_buffer_t \
        __attribute__((cleanup(registry_buffer_auto_cleanup))) name = {0}
#endif

static player_identity_registry_config_t registry_config(
    size_t active_limit,
    size_t archive_limit,
    size_t quarantine_limit) {
    player_identity_registry_config_t config = {
        .canonical_active_limit = active_limit,
        .canonical_archive_limit = archive_limit,
        .quarantine_limit = quarantine_limit,
    };
    return config;
}

static player_identity_canonical_entry_t canonical_entry(
    uint32_t identity) {
    player_identity_canonical_entry_t entry = {0};
    entry.pubkey[0] = 0xa5;
    entry.pubkey[28] = (uint8_t)(identity >> 24);
    entry.pubkey[29] = (uint8_t)(identity >> 16);
    entry.pubkey[30] = (uint8_t)(identity >> 8);
    entry.pubkey[31] = (uint8_t)identity;
    entry.sensitive_actor_id[0] = 0xd1;
    entry.sensitive_actor_id[1] = 0x7e;
    entry.sensitive_actor_id[2] = 0x51;
    entry.sensitive_actor_id[3] = 0xa9;
    entry.sensitive_actor_id[4] = (uint8_t)(identity >> 24);
    entry.sensitive_actor_id[5] = (uint8_t)(identity >> 16);
    entry.sensitive_actor_id[6] = (uint8_t)(identity >> 8);
    entry.sensitive_actor_id[7] = (uint8_t)identity;
    return entry;
}

static player_identity_quarantine_entry_t quarantine_entry(
    uint64_t record_id,
    uint64_t source_ordinal,
    uint32_t identity,
    uint16_t reason) {
    player_identity_canonical_entry_t canonical =
        canonical_entry(identity);
    player_identity_quarantine_entry_t entry = {
        .record_id = record_id,
        .source_version = 82,
        .source_ordinal = source_ordinal,
        .reason = reason,
    };
    memcpy(entry.pubkey, canonical.pubkey,
           sizeof(entry.pubkey));
    memcpy(entry.sensitive_actor_id,
           canonical.sensitive_actor_id,
           sizeof(entry.sensitive_actor_id));
    return entry;
}

static bool registry_capture_sensitive(
    const player_identity_registry_t *reg,
    registry_sensitive_buffer_t *buffer) {
    if (!reg || !buffer || buffer->data) return false;
    size_t size = 0;
    if (player_identity_registry_export_sensitive_size(
            reg, &size) != PLAYER_IDENTITY_REGISTRY_OK) {
        return false;
    }
    uint8_t *bytes = malloc(size);
    if (!bytes) return false;
    size_t written = 0;
    player_identity_registry_status_t status =
        player_identity_registry_export_sensitive(
            reg, bytes, size, &written);
    if (status != PLAYER_IDENTITY_REGISTRY_OK ||
        written != size) {
        signal_memzero_explicit(bytes, size);
        free(bytes);
        return false;
    }
    buffer->data = bytes;
    buffer->size = size;
    return true;
}

static bool sensitive_buffers_equal(
    const registry_sensitive_buffer_t *left,
    const registry_sensitive_buffer_t *right) {
    return left && right &&
        left->size == right->size &&
        left->data && right->data &&
        memcmp(left->data, right->data, left->size) == 0;
}

static uint16_t registry_test_read_u16_le(const uint8_t *bytes) {
    return (uint16_t)bytes[0] |
        (uint16_t)((uint16_t)bytes[1] << 8);
}

static uint32_t registry_test_read_u32_le(const uint8_t *bytes) {
    return (uint32_t)bytes[0] |
        ((uint32_t)bytes[1] << 8) |
        ((uint32_t)bytes[2] << 16) |
        ((uint32_t)bytes[3] << 24);
}

static uint64_t registry_test_read_u64_le(const uint8_t *bytes) {
    uint64_t value = 0;
    for (unsigned shift = 0; shift < 64; shift += 8)
        value |= (uint64_t)bytes[shift / 8] << shift;
    return value;
}

TEST(test_player_identity_registry_contract_and_overflow) {
    ASSERT_EQ_INT(PLAYER_IDENTITY_PUBKEY_SIZE, 32);
    ASSERT_EQ_INT(
        PLAYER_IDENTITY_SENSITIVE_ACTOR_ID_SIZE, 8);
    ASSERT_EQ_INT(PLAYER_IDENTITY_CANONICAL_WIRE_SIZE, 40);
    ASSERT_EQ_INT(PLAYER_IDENTITY_QUARANTINE_WIRE_SIZE, 64);
    ASSERT_EQ_INT(
        PLAYER_IDENTITY_REGISTRY_EXPORT_FLAG_SENSITIVE_BEARERS,
        1);
    ASSERT_EQ_INT(
        PLAYER_IDENTITY_QUARANTINE_REASON_LEGACY_NONCANONICAL, 1);
    ASSERT_EQ_INT(
        PLAYER_IDENTITY_QUARANTINE_REASON_OPERATOR_HOLD, 7);
    ASSERT_EQ_INT(
        PLAYER_IDENTITY_QUARANTINE_REASON_COUNT, 8);
    ASSERT(player_identity_registry_status_is_success(
        PLAYER_IDENTITY_REGISTRY_OK_CLONED));
    ASSERT(!player_identity_registry_status_is_success(
        PLAYER_IDENTITY_REGISTRY_CONFLICT));
    ASSERT_STR_EQ(
        player_identity_registry_status_name(
            PLAYER_IDENTITY_REGISTRY_ACTIVE_BUDGET_EXHAUSTED),
        "canonical_active_budget_exhausted");
    ASSERT_STR_EQ(
        player_identity_registry_status_name(
            PLAYER_IDENTITY_REGISTRY_STATUS_COUNT),
        "unknown");

    REGISTRY_TEST_DECL(empty);
    player_identity_registry_config_t zero =
        registry_config(0, 0, 0);
    ASSERT_EQ_INT(
        player_identity_registry_init(&empty, &zero),
        PLAYER_IDENTITY_REGISTRY_OK);
    ASSERT(player_identity_registry_validate(&empty));

    player_identity_registry_metrics_t metrics;
    memset(&metrics, 0xa5, sizeof(metrics));
    player_identity_registry_get_metrics(&empty, &metrics);
    ASSERT_EQ_INT((int)metrics.canonical_active.hard_limit, 0);
    ASSERT_EQ_INT((int)metrics.canonical_archive.hard_limit, 0);
    ASSERT_EQ_INT((int)metrics.quarantine.hard_limit, 0);

    player_identity_canonical_entry_t canonical =
        canonical_entry(1);
    ASSERT_EQ_INT(
        player_identity_registry_insert_active(
            &empty, &canonical),
        PLAYER_IDENTITY_REGISTRY_ACTIVE_BUDGET_EXHAUSTED);
    ASSERT(player_identity_registry_validate(&empty));

    REGISTRY_TEST_DECL(overflowed);
    player_identity_registry_config_t overflow =
        registry_config(SIZE_MAX, 1, 1);
    ASSERT_EQ_INT(
        player_identity_registry_init(
            &overflowed, &overflow),
        PLAYER_IDENTITY_REGISTRY_SIZE_OVERFLOW);
    ASSERT_EQ_INT((int)overflowed.initialized_tag, 0);
    ASSERT_EQ_INT(
        player_identity_registry_init(NULL, &zero),
        PLAYER_IDENTITY_REGISTRY_INVALID_ARGUMENT);

    player_identity_canonical_entry_t invalid = {0};
    ASSERT(!player_identity_canonical_entry_is_valid(&invalid));
    ASSERT(player_identity_canonical_entry_is_valid(&canonical));

    player_identity_quarantine_entry_t quarantine =
        quarantine_entry(
            1, 0, 1,
            PLAYER_IDENTITY_QUARANTINE_REASON_UNPROVEN_MAPPING);
    ASSERT(player_identity_quarantine_entry_is_valid(
        &quarantine));
    quarantine.source_version = 0;
    ASSERT(!player_identity_quarantine_entry_is_valid(
        &quarantine));
}

TEST(test_player_identity_registry_sensitive_export_wire_layout) {
    REGISTRY_TEST_DECL(reg);
    player_identity_registry_config_t config =
        registry_config(3, 2, 4);
    ASSERT_EQ_INT(
        player_identity_registry_init(&reg, &config),
        PLAYER_IDENTITY_REGISTRY_OK);
    player_identity_canonical_entry_t first =
        canonical_entry(1);
    player_identity_canonical_entry_t second =
        canonical_entry(2);
    ASSERT_EQ_INT(
        player_identity_registry_insert_active(&reg, &first),
        PLAYER_IDENTITY_REGISTRY_OK_INSERTED);
    ASSERT_EQ_INT(
        player_identity_registry_insert_active(&reg, &second),
        PLAYER_IDENTITY_REGISTRY_OK_INSERTED);
    ASSERT_EQ_INT(
        player_identity_registry_archive(&reg, first.pubkey),
        PLAYER_IDENTITY_REGISTRY_OK_ARCHIVED);

    player_identity_quarantine_entry_t quarantine =
        quarantine_entry(
            7, UINT64_C(0x0102030405060708), 9,
            PLAYER_IDENTITY_QUARANTINE_REASON_CONFLICTING_MAPPING);
    ASSERT_EQ_INT(
        player_identity_registry_quarantine_append(
            &reg, &quarantine),
        PLAYER_IDENTITY_REGISTRY_OK_QUARANTINED);

    REGISTRY_BUFFER_DECL(wire);
    ASSERT(registry_capture_sensitive(&reg, &wire));
    ASSERT(
        wire.size ==
        PLAYER_IDENTITY_REGISTRY_EXPORT_HEADER_SIZE +
        2 * PLAYER_IDENTITY_CANONICAL_WIRE_SIZE +
        PLAYER_IDENTITY_QUARANTINE_WIRE_SIZE);
    ASSERT(memcmp(wire.data, "PIR1", 4) == 0);
    ASSERT_EQ_INT(
        registry_test_read_u16_le(wire.data + 4),
        PLAYER_IDENTITY_REGISTRY_EXPORT_VERSION);
    ASSERT_EQ_INT(
        registry_test_read_u16_le(wire.data + 6),
        PLAYER_IDENTITY_REGISTRY_EXPORT_HEADER_SIZE);
    ASSERT_EQ_INT(
        registry_test_read_u16_le(wire.data + 8),
        PLAYER_IDENTITY_CANONICAL_WIRE_SIZE);
    ASSERT_EQ_INT(
        registry_test_read_u16_le(wire.data + 10),
        PLAYER_IDENTITY_QUARANTINE_WIRE_SIZE);
    ASSERT(
        registry_test_read_u32_le(wire.data + 12) ==
        PLAYER_IDENTITY_REGISTRY_EXPORT_FLAG_SENSITIVE_BEARERS);
    ASSERT(registry_test_read_u64_le(wire.data + 16) == 3);
    ASSERT(registry_test_read_u64_le(wire.data + 24) == 2);
    ASSERT(registry_test_read_u64_le(wire.data + 32) == 4);
    ASSERT(registry_test_read_u64_le(wire.data + 40) == 2);
    ASSERT(registry_test_read_u64_le(wire.data + 48) == 1);
    ASSERT(registry_test_read_u64_le(wire.data + 56) == 1);
    ASSERT(registry_test_read_u64_le(wire.data + 64) == 7);
    ASSERT(registry_test_read_u64_le(wire.data + 72) == 1);
    ASSERT(registry_test_read_u64_le(wire.data + 80) == 1);
    ASSERT(registry_test_read_u64_le(wire.data + 88) == 1);

    size_t active_offset =
        PLAYER_IDENTITY_REGISTRY_EXPORT_HEADER_SIZE;
    ASSERT(memcmp(
        wire.data + active_offset,
        second.pubkey, sizeof(second.pubkey)) == 0);
    ASSERT(memcmp(
        wire.data + active_offset + PLAYER_IDENTITY_PUBKEY_SIZE,
        second.sensitive_actor_id,
        sizeof(second.sensitive_actor_id)) == 0);

    size_t archive_offset =
        active_offset + PLAYER_IDENTITY_CANONICAL_WIRE_SIZE;
    ASSERT(memcmp(
        wire.data + archive_offset,
        first.pubkey, sizeof(first.pubkey)) == 0);
    ASSERT(memcmp(
        wire.data + archive_offset + PLAYER_IDENTITY_PUBKEY_SIZE,
        first.sensitive_actor_id,
        sizeof(first.sensitive_actor_id)) == 0);

    size_t quarantine_offset =
        archive_offset + PLAYER_IDENTITY_CANONICAL_WIRE_SIZE;
    ASSERT(
        registry_test_read_u64_le(
            wire.data + quarantine_offset) == 7);
    ASSERT_EQ_INT(
        registry_test_read_u32_le(
            wire.data + quarantine_offset + 8), 82);
    ASSERT_EQ_INT(
        registry_test_read_u16_le(
            wire.data + quarantine_offset + 12),
        PLAYER_IDENTITY_QUARANTINE_REASON_CONFLICTING_MAPPING);
    ASSERT_EQ_INT(
        registry_test_read_u16_le(
            wire.data + quarantine_offset + 14), 0);
    ASSERT(
        registry_test_read_u64_le(
            wire.data + quarantine_offset + 16) ==
        UINT64_C(0x0102030405060708));
    ASSERT(memcmp(
        wire.data + quarantine_offset + 24,
        quarantine.pubkey, sizeof(quarantine.pubkey)) == 0);
    ASSERT(memcmp(
        wire.data + quarantine_offset + 24 +
            PLAYER_IDENTITY_PUBKEY_SIZE,
        quarantine.sensitive_actor_id,
        sizeof(quarantine.sensitive_actor_id)) == 0);
}

typedef struct {
    bool ordered;
    bool have_previous;
    uint8_t previous[PLAYER_IDENTITY_PUBKEY_SIZE];
    size_t count;
    size_t stop_after;
} registry_visit_order_t;

static bool visit_canonical_in_order(
    player_identity_registry_tier_t tier,
    size_t ordinal,
    const player_identity_canonical_redacted_t *entry,
    void *user) {
    registry_visit_order_t *state = user;
    if (!state || !entry ||
        tier != PLAYER_IDENTITY_TIER_CANONICAL_ACTIVE ||
        ordinal != state->count) {
        if (state) state->ordered = false;
        return false;
    }
    if (state->have_previous &&
        memcmp(state->previous, entry->pubkey,
               sizeof(state->previous)) >= 0) {
        state->ordered = false;
        return false;
    }
    memcpy(state->previous, entry->pubkey,
           sizeof(state->previous));
    state->have_previous = true;
    state->count++;
    return state->stop_after == 0 ||
        state->count < state->stop_after;
}

TEST(test_player_identity_registry_scales_and_serializes_deterministically) {
    enum { IDENTITY_COUNT = 2304 };
    REGISTRY_TEST_DECL(ascending);
    REGISTRY_TEST_DECL(permuted);
    player_identity_registry_config_t config =
        registry_config(4096, 64, 64);
    ASSERT_EQ_INT(
        player_identity_registry_init(&ascending, &config),
        PLAYER_IDENTITY_REGISTRY_OK);
    ASSERT_EQ_INT(
        player_identity_registry_init(&permuted, &config),
        PLAYER_IDENTITY_REGISTRY_OK);

    for (uint32_t i = 0; i < IDENTITY_COUNT; i++) {
        player_identity_canonical_entry_t entry =
            canonical_entry(i + 1);
        ASSERT_EQ_INT(
            player_identity_registry_insert_active(
                &ascending, &entry),
            PLAYER_IDENTITY_REGISTRY_OK_INSERTED);
    }
    for (uint32_t i = 0; i < IDENTITY_COUNT; i++) {
        uint32_t identity =
            (uint32_t)(((uint64_t)i * 2053u) %
                       IDENTITY_COUNT) + 1u;
        player_identity_canonical_entry_t entry =
            canonical_entry(identity);
        ASSERT_EQ_INT(
            player_identity_registry_insert_active(
                &permuted, &entry),
            PLAYER_IDENTITY_REGISTRY_OK_INSERTED);
    }
    ASSERT(player_identity_registry_validate(&ascending));
    ASSERT(player_identity_registry_validate(&permuted));

    for (uint32_t identity = 1;
         identity <= IDENTITY_COUNT; identity++) {
        player_identity_canonical_entry_t expected =
            canonical_entry(identity);
        player_identity_canonical_entry_t found;
        player_identity_registry_tier_t tier =
            PLAYER_IDENTITY_TIER_NONE;
        ASSERT_EQ_INT(
            player_identity_registry_find_sensitive(
                &permuted, expected.pubkey, &found, &tier),
            PLAYER_IDENTITY_REGISTRY_OK);
        ASSERT_EQ_INT(
            tier, PLAYER_IDENTITY_TIER_CANONICAL_ACTIVE);
        ASSERT(memcmp(&found, &expected, sizeof(found)) == 0);
    }

    REGISTRY_BUFFER_DECL(ascending_bytes);
    REGISTRY_BUFFER_DECL(permuted_bytes);
    ASSERT(registry_capture_sensitive(
        &ascending, &ascending_bytes));
    ASSERT(registry_capture_sensitive(
        &permuted, &permuted_bytes));
    ASSERT(sensitive_buffers_equal(
        &ascending_bytes, &permuted_bytes));
    ASSERT(ascending_bytes.size >=
           PLAYER_IDENTITY_REGISTRY_EXPORT_HEADER_SIZE);
    ASSERT_EQ_INT(ascending_bytes.data[12], 1);
    ASSERT_EQ_INT(ascending_bytes.data[13], 0);
    ASSERT_EQ_INT(ascending_bytes.data[14], 0);
    ASSERT_EQ_INT(ascending_bytes.data[15], 0);

    registry_visit_order_t order = {
        .ordered = true,
    };
    ASSERT_EQ_INT(
        player_identity_registry_visit(
            &permuted, visit_canonical_in_order,
            NULL, &order),
        PLAYER_IDENTITY_REGISTRY_OK);
    ASSERT(order.ordered);
    ASSERT_EQ_INT((int)order.count, IDENTITY_COUNT);

    registry_visit_order_t stopped = {
        .ordered = true,
        .stop_after = 7,
    };
    ASSERT_EQ_INT(
        player_identity_registry_visit(
            &permuted, visit_canonical_in_order,
            NULL, &stopped),
        PLAYER_IDENTITY_REGISTRY_VISITOR_STOPPED);
    ASSERT(stopped.ordered);
    ASSERT_EQ_INT((int)stopped.count, 7);
}

TEST(test_player_identity_registry_insert_update_is_transactional) {
    REGISTRY_TEST_DECL(reg);
    player_identity_registry_config_t config =
        registry_config(8, 8, 8);
    ASSERT_EQ_INT(
        player_identity_registry_init(&reg, &config),
        PLAYER_IDENTITY_REGISTRY_OK);

    player_identity_canonical_entry_t original =
        canonical_entry(11);
    ASSERT_EQ_INT(
        player_identity_registry_insert_active(
            &reg, &original),
        PLAYER_IDENTITY_REGISTRY_OK_INSERTED);

    REGISTRY_BUFFER_DECL(before);
    ASSERT(registry_capture_sensitive(&reg, &before));
    ASSERT_EQ_INT(
        player_identity_registry_insert_active(
            &reg, &original),
        PLAYER_IDENTITY_REGISTRY_OK_EXISTING);

    player_identity_canonical_entry_t conflicting =
        original;
    conflicting.sensitive_actor_id[0] ^= 0x5au;
    ASSERT(conflicting.sensitive_actor_id[0] != 0);
    ASSERT_EQ_INT(
        player_identity_registry_insert_active(
            &reg, &conflicting),
        PLAYER_IDENTITY_REGISTRY_CONFLICT);

    REGISTRY_BUFFER_DECL(after_conflict);
    ASSERT(registry_capture_sensitive(
        &reg, &after_conflict));
    ASSERT(sensitive_buffers_equal(
        &before, &after_conflict));

    player_identity_canonical_entry_t invalid = original;
    memset(invalid.pubkey, 0, sizeof(invalid.pubkey));
    ASSERT_EQ_INT(
        player_identity_registry_insert_active(
            &reg, &invalid),
        PLAYER_IDENTITY_REGISTRY_INVALID_ARGUMENT);
    REGISTRY_BUFFER_DECL(after_invalid);
    ASSERT(registry_capture_sensitive(
        &reg, &after_invalid));
    ASSERT(sensitive_buffers_equal(
        &before, &after_invalid));

    uint8_t zero_actor[
        PLAYER_IDENTITY_SENSITIVE_ACTOR_ID_SIZE] = {0};
    ASSERT_EQ_INT(
        player_identity_registry_update(
            &reg, original.pubkey, zero_actor),
        PLAYER_IDENTITY_REGISTRY_INVALID_ARGUMENT);
    player_identity_canonical_entry_t absent =
        canonical_entry(99);
    ASSERT_EQ_INT(
        player_identity_registry_update(
            &reg, absent.pubkey,
            absent.sensitive_actor_id),
        PLAYER_IDENTITY_REGISTRY_NOT_FOUND);
    REGISTRY_BUFFER_DECL(after_update_failures);
    ASSERT(registry_capture_sensitive(
        &reg, &after_update_failures));
    ASSERT(sensitive_buffers_equal(
        &before, &after_update_failures));

    ASSERT_EQ_INT(
        player_identity_registry_update(
            &reg, original.pubkey,
            conflicting.sensitive_actor_id),
        PLAYER_IDENTITY_REGISTRY_OK_UPDATED);
    ASSERT_EQ_INT(
        player_identity_registry_update(
            &reg, original.pubkey,
            conflicting.sensitive_actor_id),
        PLAYER_IDENTITY_REGISTRY_OK_EXISTING);

    player_identity_canonical_entry_t found;
    ASSERT_EQ_INT(
        player_identity_registry_find_sensitive(
            &reg, original.pubkey, &found, NULL),
        PLAYER_IDENTITY_REGISTRY_OK);
    ASSERT(memcmp(
        found.sensitive_actor_id,
        conflicting.sensitive_actor_id,
        sizeof(found.sensitive_actor_id)) == 0);

    player_identity_registry_metrics_t metrics;
    player_identity_registry_get_metrics(&reg, &metrics);
    ASSERT_EQ_INT(
        (int)metrics.failure_status_counts[
            PLAYER_IDENTITY_REGISTRY_CONFLICT],
        1);
    ASSERT_EQ_INT(
        (int)metrics.failure_status_counts[
            PLAYER_IDENTITY_REGISTRY_INVALID_ARGUMENT],
        2);
    ASSERT_EQ_INT(
        (int)metrics.failure_status_counts[
            PLAYER_IDENTITY_REGISTRY_NOT_FOUND],
        1);
    ASSERT_EQ_INT(
        (int)metrics.canonical_active.failure_count, 2);
}

TEST(test_player_identity_registry_actor_ids_are_globally_unique) {
    REGISTRY_TEST_DECL(reg);
    player_identity_registry_config_t config =
        registry_config(8, 8, 1);
    ASSERT_EQ_INT(
        player_identity_registry_init(&reg, &config),
        PLAYER_IDENTITY_REGISTRY_OK);

    player_identity_canonical_entry_t first =
        canonical_entry(1);
    player_identity_canonical_entry_t second =
        canonical_entry(2);
    player_identity_canonical_entry_t third =
        canonical_entry(3);
    ASSERT_EQ_INT(
        player_identity_registry_insert_active(&reg, &first),
        PLAYER_IDENTITY_REGISTRY_OK_INSERTED);
    ASSERT_EQ_INT(
        player_identity_registry_insert_active(&reg, &second),
        PLAYER_IDENTITY_REGISTRY_OK_INSERTED);

    player_identity_canonical_entry_t duplicate_actor = third;
    memcpy(
        duplicate_actor.sensitive_actor_id,
        first.sensitive_actor_id,
        sizeof(duplicate_actor.sensitive_actor_id));
    REGISTRY_BUFFER_DECL(before_active_collision);
    ASSERT(registry_capture_sensitive(
        &reg, &before_active_collision));
    ASSERT_EQ_INT(
        player_identity_registry_insert_active(
            &reg, &duplicate_actor),
        PLAYER_IDENTITY_REGISTRY_CONFLICT);
    REGISTRY_BUFFER_DECL(after_active_collision);
    ASSERT(registry_capture_sensitive(
        &reg, &after_active_collision));
    ASSERT(sensitive_buffers_equal(
        &before_active_collision, &after_active_collision));

    ASSERT_EQ_INT(
        player_identity_registry_archive(&reg, first.pubkey),
        PLAYER_IDENTITY_REGISTRY_OK_ARCHIVED);
    ASSERT(player_identity_registry_validate(&reg));

    REGISTRY_BUFFER_DECL(before_cross_tier_collisions);
    ASSERT(registry_capture_sensitive(
        &reg, &before_cross_tier_collisions));
    ASSERT_EQ_INT(
        player_identity_registry_insert_active(
            &reg, &duplicate_actor),
        PLAYER_IDENTITY_REGISTRY_CONFLICT);
    ASSERT_EQ_INT(
        player_identity_registry_update(
            &reg, second.pubkey,
            first.sensitive_actor_id),
        PLAYER_IDENTITY_REGISTRY_CONFLICT);
    ASSERT_EQ_INT(
        player_identity_registry_update(
            &reg, first.pubkey,
            second.sensitive_actor_id),
        PLAYER_IDENTITY_REGISTRY_CONFLICT);
    REGISTRY_BUFFER_DECL(after_cross_tier_collisions);
    ASSERT(registry_capture_sensitive(
        &reg, &after_cross_tier_collisions));
    ASSERT(sensitive_buffers_equal(
        &before_cross_tier_collisions,
        &after_cross_tier_collisions));

    memcpy(
        reg.canonical_active.entries[0].sensitive_actor_id,
        first.sensitive_actor_id,
        PLAYER_IDENTITY_SENSITIVE_ACTOR_ID_SIZE);
    ASSERT(!player_identity_registry_validate(&reg));
    player_identity_canonical_entry_t rejected_lookup;
    memset(&rejected_lookup, 0xa5, sizeof(rejected_lookup));
    player_identity_registry_tier_t rejected_tier =
        PLAYER_IDENTITY_TIER_CANONICAL_ACTIVE;
    ASSERT_EQ_INT(
        player_identity_registry_find_sensitive(
            &reg, second.pubkey,
            &rejected_lookup, &rejected_tier),
        PLAYER_IDENTITY_REGISTRY_INVALID_STATE);
    for (size_t i = 0; i < sizeof(rejected_lookup); i++)
        ASSERT(((const uint8_t *)&rejected_lookup)[i] == 0xa5);
    ASSERT_EQ_INT(
        rejected_tier, PLAYER_IDENTITY_TIER_CANONICAL_ACTIVE);
    player_identity_canonical_entry_t active_snapshot =
        reg.canonical_active.entries[0];
    player_identity_canonical_entry_t archive_snapshot =
        reg.canonical_archive.entries[0];
    size_t active_count = reg.canonical_active.count;
    size_t archive_count = reg.canonical_archive.count;
    ASSERT_EQ_INT(
        player_identity_registry_restore(&reg, first.pubkey),
        PLAYER_IDENTITY_REGISTRY_INVALID_STATE);
    ASSERT(reg.canonical_active.count == active_count);
    ASSERT(reg.canonical_archive.count == archive_count);
    ASSERT(memcmp(
        &reg.canonical_active.entries[0],
        &active_snapshot, sizeof(active_snapshot)) == 0);
    ASSERT(memcmp(
        &reg.canonical_archive.entries[0],
        &archive_snapshot, sizeof(archive_snapshot)) == 0);

    memcpy(
        reg.canonical_active.entries[0].sensitive_actor_id,
        second.sensitive_actor_id,
        PLAYER_IDENTITY_SENSITIVE_ACTOR_ID_SIZE);
    ASSERT(player_identity_registry_validate(&reg));
    ASSERT_EQ_INT(
        player_identity_registry_restore(&reg, first.pubkey),
        PLAYER_IDENTITY_REGISTRY_OK_RESTORED);
    ASSERT(player_identity_registry_validate(&reg));

    player_identity_registry_metrics_t metrics;
    player_identity_registry_get_metrics(&reg, &metrics);
    ASSERT_EQ_INT(
        (int)metrics.failure_status_counts[
            PLAYER_IDENTITY_REGISTRY_CONFLICT],
        4);
    ASSERT_EQ_INT(
        (int)metrics.failure_status_counts[
            PLAYER_IDENTITY_REGISTRY_INVALID_STATE],
        1);
    ASSERT_EQ_INT(
        (int)metrics.canonical_active.failure_count, 4);
    ASSERT_EQ_INT(
        (int)metrics.canonical_archive.failure_count, 1);
}

TEST(test_player_identity_registry_snapshots_aliased_inputs_before_growth) {
    REGISTRY_TEST_DECL(canonical);
    player_identity_registry_config_t config =
        registry_config(8, 1, 8);
    ASSERT_EQ_INT(
        player_identity_registry_init(&canonical, &config),
        PLAYER_IDENTITY_REGISTRY_OK);
    player_identity_canonical_entry_t first =
        canonical_entry(1);
    ASSERT_EQ_INT(
        player_identity_registry_insert_active(
            &canonical, &first),
        PLAYER_IDENTITY_REGISTRY_OK_INSERTED);
    ASSERT(canonical.canonical_active.capacity > 1);

    /*
     * The stores are visible for embedding, so an input can refer to unused
     * backing storage. Report only the occupied prefix as capacity to force
     * growth; the API must snapshot the aliased row before freeing the old
     * allocation.
     */
    canonical.canonical_active.capacity =
        canonical.canonical_active.count;
    player_identity_canonical_entry_t *aliased_canonical =
        &canonical.canonical_active.entries[
            canonical.canonical_active.count];
    player_identity_canonical_entry_t second =
        canonical_entry(2);
    *aliased_canonical = second;
    ASSERT(player_identity_registry_validate(&canonical));
    ASSERT_EQ_INT(
        player_identity_registry_insert_active(
            &canonical, aliased_canonical),
        PLAYER_IDENTITY_REGISTRY_OK_INSERTED);
    ASSERT(player_identity_registry_validate(&canonical));

    player_identity_canonical_entry_t found;
    ASSERT_EQ_INT(
        player_identity_registry_find_sensitive(
            &canonical, second.pubkey,
            &found, NULL),
        PLAYER_IDENTITY_REGISTRY_OK);
    ASSERT(memcmp(
        found.sensitive_actor_id,
        second.sensitive_actor_id,
        sizeof(found.sensitive_actor_id)) == 0);
    ASSERT_EQ_INT(
        player_identity_registry_find_sensitive(
            &canonical, found.pubkey, &found, NULL),
        PLAYER_IDENTITY_REGISTRY_OK);
    ASSERT(memcmp(&found, &second, sizeof(found)) == 0);

    REGISTRY_BUFFER_DECL(before_output_alias);
    ASSERT(registry_capture_sensitive(
        &canonical, &before_output_alias));
    player_identity_registry_tier_t alias_tier =
        PLAYER_IDENTITY_TIER_CANONICAL_ARCHIVE;
    ASSERT_EQ_INT(
        player_identity_registry_find_sensitive(
            &canonical, second.pubkey,
            &canonical.canonical_active.entries[0],
            &alias_tier),
        PLAYER_IDENTITY_REGISTRY_INVALID_ARGUMENT);
    ASSERT_EQ_INT(
        alias_tier, PLAYER_IDENTITY_TIER_CANONICAL_ARCHIVE);
    REGISTRY_BUFFER_DECL(after_output_alias);
    ASSERT(registry_capture_sensitive(
        &canonical, &after_output_alias));
    ASSERT(sensitive_buffers_equal(
        &before_output_alias, &after_output_alias));

    REGISTRY_TEST_DECL(quarantine);
    ASSERT_EQ_INT(
        player_identity_registry_init(&quarantine, &config),
        PLAYER_IDENTITY_REGISTRY_OK);
    player_identity_quarantine_entry_t q1 =
        quarantine_entry(
            1, 0, 11,
            PLAYER_IDENTITY_QUARANTINE_REASON_OPERATOR_HOLD);
    ASSERT_EQ_INT(
        player_identity_registry_quarantine_append(
            &quarantine, &q1),
        PLAYER_IDENTITY_REGISTRY_OK_QUARANTINED);
    ASSERT(quarantine.quarantine.capacity > 1);
    quarantine.quarantine.capacity =
        quarantine.quarantine.count;
    player_identity_quarantine_entry_t *aliased_quarantine =
        &quarantine.quarantine.entries[
            quarantine.quarantine.count];
    *aliased_quarantine = quarantine_entry(
        2, 1, 12,
        PLAYER_IDENTITY_QUARANTINE_REASON_UNPROVEN_MAPPING);
    ASSERT(player_identity_registry_validate(&quarantine));
    ASSERT_EQ_INT(
        player_identity_registry_quarantine_append(
            &quarantine, aliased_quarantine),
        PLAYER_IDENTITY_REGISTRY_OK_QUARANTINED);
    ASSERT(player_identity_registry_validate(&quarantine));
    ASSERT_EQ_INT((int)quarantine.quarantine.count, 2);
    ASSERT(quarantine.quarantine.entries[1].record_id == 2);
}

TEST(test_player_identity_registry_tier_budgets_are_independent) {
    REGISTRY_TEST_DECL(reg);
    player_identity_registry_config_t config =
        registry_config(2, 1, 2);
    ASSERT_EQ_INT(
        player_identity_registry_init(&reg, &config),
        PLAYER_IDENTITY_REGISTRY_OK);

    player_identity_canonical_entry_t first =
        canonical_entry(1);
    player_identity_canonical_entry_t second =
        canonical_entry(2);
    player_identity_canonical_entry_t third =
        canonical_entry(3);
    ASSERT_EQ_INT(
        player_identity_registry_insert_active(&reg, &first),
        PLAYER_IDENTITY_REGISTRY_OK_INSERTED);
    ASSERT_EQ_INT(
        player_identity_registry_insert_active(&reg, &second),
        PLAYER_IDENTITY_REGISTRY_OK_INSERTED);

    REGISTRY_BUFFER_DECL(active_full);
    ASSERT(registry_capture_sensitive(&reg, &active_full));
    ASSERT_EQ_INT(
        player_identity_registry_insert_active(&reg, &third),
        PLAYER_IDENTITY_REGISTRY_ACTIVE_BUDGET_EXHAUSTED);
    REGISTRY_BUFFER_DECL(after_active_failure);
    ASSERT(registry_capture_sensitive(
        &reg, &after_active_failure));
    ASSERT(sensitive_buffers_equal(
        &active_full, &after_active_failure));

    ASSERT_EQ_INT(
        player_identity_registry_archive(
            &reg, first.pubkey),
        PLAYER_IDENTITY_REGISTRY_OK_ARCHIVED);
    REGISTRY_BUFFER_DECL(archive_full);
    ASSERT(registry_capture_sensitive(&reg, &archive_full));
    ASSERT_EQ_INT(
        player_identity_registry_archive(
            &reg, second.pubkey),
        PLAYER_IDENTITY_REGISTRY_ARCHIVE_BUDGET_EXHAUSTED);
    REGISTRY_BUFFER_DECL(after_archive_failure);
    ASSERT(registry_capture_sensitive(
        &reg, &after_archive_failure));
    ASSERT(sensitive_buffers_equal(
        &archive_full, &after_archive_failure));

    ASSERT_EQ_INT(
        player_identity_registry_insert_active(&reg, &third),
        PLAYER_IDENTITY_REGISTRY_OK_INSERTED);

    player_identity_quarantine_entry_t q1 =
        quarantine_entry(
            1, 0, 101,
            PLAYER_IDENTITY_QUARANTINE_REASON_LEGACY_NONCANONICAL);
    player_identity_quarantine_entry_t q2 =
        quarantine_entry(
            2, 1, 102,
            PLAYER_IDENTITY_QUARANTINE_REASON_DUPLICATE_PUBKEY);
    player_identity_quarantine_entry_t q3 =
        quarantine_entry(
            3, 2, 103,
            PLAYER_IDENTITY_QUARANTINE_REASON_CONFLICTING_MAPPING);
    ASSERT_EQ_INT(
        player_identity_registry_quarantine_append(&reg, &q1),
        PLAYER_IDENTITY_REGISTRY_OK_QUARANTINED);
    ASSERT_EQ_INT(
        player_identity_registry_quarantine_append(&reg, &q2),
        PLAYER_IDENTITY_REGISTRY_OK_QUARANTINED);
    REGISTRY_BUFFER_DECL(quarantine_full);
    ASSERT(registry_capture_sensitive(
        &reg, &quarantine_full));
    ASSERT_EQ_INT(
        player_identity_registry_quarantine_append(&reg, &q3),
        PLAYER_IDENTITY_REGISTRY_QUARANTINE_BUDGET_EXHAUSTED);
    REGISTRY_BUFFER_DECL(after_quarantine_failure);
    ASSERT(registry_capture_sensitive(
        &reg, &after_quarantine_failure));
    ASSERT(sensitive_buffers_equal(
        &quarantine_full, &after_quarantine_failure));

    REGISTRY_BUFFER_DECL(before_restore_failure);
    ASSERT(registry_capture_sensitive(
        &reg, &before_restore_failure));
    ASSERT_EQ_INT(
        player_identity_registry_restore(
            &reg, first.pubkey),
        PLAYER_IDENTITY_REGISTRY_ACTIVE_BUDGET_EXHAUSTED);
    REGISTRY_BUFFER_DECL(after_restore_failure);
    ASSERT(registry_capture_sensitive(
        &reg, &after_restore_failure));
    ASSERT(sensitive_buffers_equal(
        &before_restore_failure, &after_restore_failure));

    player_identity_registry_metrics_t metrics;
    player_identity_registry_get_metrics(&reg, &metrics);
    ASSERT_EQ_INT((int)metrics.canonical_active.count, 2);
    ASSERT_EQ_INT((int)metrics.canonical_active.high_water, 2);
    ASSERT_EQ_INT(
        (int)metrics.canonical_active.budget_failure_count, 2);
    ASSERT_EQ_INT((int)metrics.canonical_archive.count, 1);
    ASSERT_EQ_INT((int)metrics.canonical_archive.high_water, 1);
    ASSERT_EQ_INT(
        (int)metrics.canonical_archive.budget_failure_count, 1);
    ASSERT_EQ_INT((int)metrics.quarantine.count, 2);
    ASSERT_EQ_INT((int)metrics.quarantine.high_water, 2);
    ASSERT_EQ_INT(
        (int)metrics.quarantine.budget_failure_count, 1);
    ASSERT_EQ_INT(
        (int)metrics.canonical_active.hard_bytes,
        2 * (int)sizeof(player_identity_canonical_entry_t));
    ASSERT_EQ_INT(
        (int)metrics.quarantine.hard_bytes,
        2 * (int)sizeof(player_identity_quarantine_entry_t));

    player_identity_registry_tier_t tier =
        PLAYER_IDENTITY_TIER_NONE;
    ASSERT_EQ_INT(
        player_identity_registry_find(
            &reg, first.pubkey, &tier),
        PLAYER_IDENTITY_REGISTRY_OK);
    ASSERT_EQ_INT(
        tier, PLAYER_IDENTITY_TIER_CANONICAL_ARCHIVE);
    ASSERT_EQ_INT(
        player_identity_registry_find(
            &reg, third.pubkey, &tier),
        PLAYER_IDENTITY_REGISTRY_OK);
    ASSERT_EQ_INT(
        tier, PLAYER_IDENTITY_TIER_CANONICAL_ACTIVE);
}

typedef struct {
    size_t count;
    bool valid;
    bool padding_zero;
    uint8_t expected_pubkey[PLAYER_IDENTITY_PUBKEY_SIZE];
} quarantine_redacted_visit_t;

static bool byte_is_quarantine_redacted_member(size_t byte) {
#define MEMBER_CONTAINS(type, member, index) \
    ((size_t)(index) - offsetof(type, member) < \
     sizeof(((type *)0)->member))
    return
        MEMBER_CONTAINS(
            player_identity_quarantine_redacted_t,
            record_id, byte) ||
        MEMBER_CONTAINS(
            player_identity_quarantine_redacted_t,
            source_version, byte) ||
        MEMBER_CONTAINS(
            player_identity_quarantine_redacted_t,
            source_ordinal, byte) ||
        MEMBER_CONTAINS(
            player_identity_quarantine_redacted_t,
            reason, byte) ||
        MEMBER_CONTAINS(
            player_identity_quarantine_redacted_t,
            pubkey, byte);
#undef MEMBER_CONTAINS
}

static bool visit_quarantine_redacted(
    size_t ordinal,
    const player_identity_quarantine_redacted_t *entry,
    void *user) {
    quarantine_redacted_visit_t *state = user;
    if (!state || !entry || ordinal != state->count ||
        entry->record_id != (uint64_t)ordinal + 1 ||
        memcmp(entry->pubkey,
               state->expected_pubkey,
               sizeof(state->expected_pubkey)) != 0) {
        if (state) state->valid = false;
        return false;
    }
    uint8_t representation[sizeof(*entry)];
    memcpy(representation, entry, sizeof(representation));
    for (size_t i = 0; i < sizeof(representation); i++) {
        if (!byte_is_quarantine_redacted_member(i) &&
            representation[i] != 0) {
            state->padding_zero = false;
            return false;
        }
    }
    state->count++;
    return true;
}

typedef struct {
    size_t count;
    uint8_t expected_actor[
        PLAYER_IDENTITY_SENSITIVE_ACTOR_ID_SIZE];
    bool valid;
} quarantine_sensitive_visit_t;

static bool visit_quarantine_sensitive(
    size_t ordinal,
    const player_identity_quarantine_entry_t *entry,
    void *user) {
    quarantine_sensitive_visit_t *state = user;
    if (!state || !entry || ordinal != state->count ||
        memcmp(entry->sensitive_actor_id,
               state->expected_actor,
               sizeof(state->expected_actor)) != 0) {
        if (state) state->valid = false;
        return false;
    }
    state->count++;
    return true;
}

TEST(test_player_identity_registry_quarantine_retains_duplicate_evidence) {
    REGISTRY_TEST_DECL(reg);
    player_identity_registry_config_t config =
        registry_config(1, 1, 4);
    ASSERT_EQ_INT(
        player_identity_registry_init(&reg, &config),
        PLAYER_IDENTITY_REGISTRY_OK);

    uint64_t next_id = 0;
    ASSERT_EQ_INT(
        player_identity_registry_next_quarantine_id(
            &reg, &next_id),
        PLAYER_IDENTITY_REGISTRY_OK);
    ASSERT(next_id == 1);

    player_identity_quarantine_entry_t first =
        quarantine_entry(
            1, 9, 77,
            PLAYER_IDENTITY_QUARANTINE_REASON_DUPLICATE_PUBKEY);
    player_identity_quarantine_entry_t duplicate = first;
    duplicate.record_id = 2;
    ASSERT_EQ_INT(
        player_identity_registry_quarantine_append(
            &reg, &first),
        PLAYER_IDENTITY_REGISTRY_OK_QUARANTINED);
    ASSERT_EQ_INT(
        player_identity_registry_quarantine_append(
            &reg, &duplicate),
        PLAYER_IDENTITY_REGISTRY_OK_QUARANTINED);
    ASSERT_EQ_INT(
        player_identity_registry_next_quarantine_id(
            &reg, &next_id),
        PLAYER_IDENTITY_REGISTRY_OK);
    ASSERT(next_id == 3);

    quarantine_redacted_visit_t redacted = {
        .valid = true,
        .padding_zero = true,
    };
    memcpy(redacted.expected_pubkey,
           first.pubkey,
           sizeof(redacted.expected_pubkey));
    ASSERT_EQ_INT(
        player_identity_registry_visit(
            &reg, NULL, visit_quarantine_redacted,
            &redacted),
        PLAYER_IDENTITY_REGISTRY_OK);
    ASSERT(redacted.valid);
    ASSERT(redacted.padding_zero);
    ASSERT_EQ_INT((int)redacted.count, 2);

    quarantine_sensitive_visit_t sensitive = {
        .valid = true,
    };
    memcpy(sensitive.expected_actor,
           first.sensitive_actor_id,
           sizeof(sensitive.expected_actor));
    ASSERT_EQ_INT(
        player_identity_registry_visit_sensitive(
            &reg, NULL, visit_quarantine_sensitive,
            &sensitive),
        PLAYER_IDENTITY_REGISTRY_OK);
    ASSERT(sensitive.valid);
    ASSERT_EQ_INT((int)sensitive.count, 2);

    REGISTRY_BUFFER_DECL(before_conflict);
    ASSERT(registry_capture_sensitive(
        &reg, &before_conflict));
    player_identity_quarantine_entry_t repeated_id =
        first;
    ASSERT_EQ_INT(
        player_identity_registry_quarantine_append(
            &reg, &repeated_id),
        PLAYER_IDENTITY_REGISTRY_CONFLICT);
    REGISTRY_BUFFER_DECL(after_conflict);
    ASSERT(registry_capture_sensitive(
        &reg, &after_conflict));
    ASSERT(sensitive_buffers_equal(
        &before_conflict, &after_conflict));

    player_identity_quarantine_entry_t invalid =
        duplicate;
    invalid.record_id = 3;
    invalid.reason = PLAYER_IDENTITY_QUARANTINE_REASON_NONE;
    ASSERT_EQ_INT(
        player_identity_registry_quarantine_append(
            &reg, &invalid),
        PLAYER_IDENTITY_REGISTRY_INVALID_ARGUMENT);

    REGISTRY_TEST_DECL(exhausted);
    player_identity_registry_config_t exhausted_config =
        registry_config(0, 0, 2);
    ASSERT_EQ_INT(
        player_identity_registry_init(
            &exhausted, &exhausted_config),
        PLAYER_IDENTITY_REGISTRY_OK);
    player_identity_quarantine_entry_t maximum =
        quarantine_entry(
            UINT64_MAX, 0, 88,
            PLAYER_IDENTITY_QUARANTINE_REASON_OPERATOR_HOLD);
    ASSERT_EQ_INT(
        player_identity_registry_quarantine_append(
            &exhausted, &maximum),
        PLAYER_IDENTITY_REGISTRY_OK_QUARANTINED);
    ASSERT_EQ_INT(
        player_identity_registry_next_quarantine_id(
            &exhausted, &next_id),
        PLAYER_IDENTITY_REGISTRY_QUARANTINE_ID_EXHAUSTED);
    player_identity_quarantine_entry_t impossible =
        maximum;
    impossible.record_id = 1;
    ASSERT_EQ_INT(
        player_identity_registry_quarantine_append(
            &exhausted, &impossible),
        PLAYER_IDENTITY_REGISTRY_QUARANTINE_ID_EXHAUSTED);
}

TEST(test_player_identity_registry_archive_restore_and_wipe_tail) {
    REGISTRY_TEST_DECL(reg);
    player_identity_registry_config_t config =
        registry_config(4, 4, 1);
    ASSERT_EQ_INT(
        player_identity_registry_init(&reg, &config),
        PLAYER_IDENTITY_REGISTRY_OK);

    player_identity_canonical_entry_t first =
        canonical_entry(10);
    player_identity_canonical_entry_t middle =
        canonical_entry(20);
    player_identity_canonical_entry_t last =
        canonical_entry(30);
    ASSERT_EQ_INT(
        player_identity_registry_insert_active(&reg, &last),
        PLAYER_IDENTITY_REGISTRY_OK_INSERTED);
    ASSERT_EQ_INT(
        player_identity_registry_insert_active(&reg, &first),
        PLAYER_IDENTITY_REGISTRY_OK_INSERTED);
    ASSERT_EQ_INT(
        player_identity_registry_insert_active(&reg, &middle),
        PLAYER_IDENTITY_REGISTRY_OK_INSERTED);

    ASSERT_EQ_INT(
        player_identity_registry_archive(
            &reg, middle.pubkey),
        PLAYER_IDENTITY_REGISTRY_OK_ARCHIVED);
    ASSERT_EQ_INT(
        player_identity_registry_archive(
            &reg, middle.pubkey),
        PLAYER_IDENTITY_REGISTRY_OK_EXISTING);
    ASSERT(player_identity_registry_validate(&reg));

    player_identity_registry_tier_t tier =
        PLAYER_IDENTITY_TIER_NONE;
    ASSERT_EQ_INT(
        player_identity_registry_find(
            &reg, middle.pubkey, &tier),
        PLAYER_IDENTITY_REGISTRY_OK);
    ASSERT_EQ_INT(
        tier, PLAYER_IDENTITY_TIER_CANONICAL_ARCHIVE);

    player_identity_canonical_entry_t zero = {0};
    ASSERT(memcmp(
        &reg.canonical_active.entries[
            reg.canonical_active.count],
        &zero, sizeof(zero)) == 0);

    ASSERT_EQ_INT(
        player_identity_registry_restore(
            &reg, middle.pubkey),
        PLAYER_IDENTITY_REGISTRY_OK_RESTORED);
    ASSERT_EQ_INT(
        player_identity_registry_restore(
            &reg, middle.pubkey),
        PLAYER_IDENTITY_REGISTRY_OK_EXISTING);
    ASSERT(player_identity_registry_validate(&reg));
    ASSERT_EQ_INT(
        player_identity_registry_find(
            &reg, middle.pubkey, &tier),
        PLAYER_IDENTITY_REGISTRY_OK);
    ASSERT_EQ_INT(
        tier, PLAYER_IDENTITY_TIER_CANONICAL_ACTIVE);
    ASSERT(memcmp(
        &reg.canonical_archive.entries[
            reg.canonical_archive.count],
        &zero, sizeof(zero)) == 0);
}

TEST(test_player_identity_registry_rejects_invalid_state_and_small_export) {
    REGISTRY_TEST_DECL(reg);
    player_identity_registry_config_t config =
        registry_config(4, 4, 4);
    ASSERT_EQ_INT(
        player_identity_registry_init(&reg, &config),
        PLAYER_IDENTITY_REGISTRY_OK);
    player_identity_canonical_entry_t first =
        canonical_entry(1);
    player_identity_canonical_entry_t second =
        canonical_entry(2);
    ASSERT_EQ_INT(
        player_identity_registry_insert_active(&reg, &first),
        PLAYER_IDENTITY_REGISTRY_OK_INSERTED);
    ASSERT_EQ_INT(
        player_identity_registry_insert_active(&reg, &second),
        PLAYER_IDENTITY_REGISTRY_OK_INSERTED);

    size_t actual_count = reg.canonical_active.count;
    reg.canonical_active.count =
        reg.canonical_active.capacity + 1;
    ASSERT(!player_identity_registry_validate(&reg));
    ASSERT_EQ_INT(
        player_identity_registry_update(
            &reg, first.pubkey,
            first.sensitive_actor_id),
        PLAYER_IDENTITY_REGISTRY_INVALID_STATE);
    reg.canonical_active.count = actual_count;
    ASSERT(player_identity_registry_validate(&reg));

    player_identity_canonical_entry_t temporary =
        reg.canonical_active.entries[0];
    reg.canonical_active.entries[0] =
        reg.canonical_active.entries[1];
    reg.canonical_active.entries[1] = temporary;
    ASSERT(!player_identity_registry_validate(&reg));
    player_identity_canonical_entry_t rejected;
    memset(&rejected, 0xa5, sizeof(rejected));
    player_identity_registry_tier_t rejected_tier =
        PLAYER_IDENTITY_TIER_CANONICAL_ACTIVE;
    ASSERT_EQ_INT(
        player_identity_registry_find_sensitive(
            &reg, first.pubkey, &rejected, &rejected_tier),
        PLAYER_IDENTITY_REGISTRY_INVALID_STATE);
    for (size_t i = 0; i < sizeof(rejected); i++)
        ASSERT(((const uint8_t *)&rejected)[i] == 0xa5);
    ASSERT_EQ_INT(
        rejected_tier, PLAYER_IDENTITY_TIER_CANONICAL_ACTIVE);
    size_t export_size = 123;
    ASSERT_EQ_INT(
        player_identity_registry_export_sensitive_size(
            &reg, &export_size),
        PLAYER_IDENTITY_REGISTRY_INVALID_STATE);
    ASSERT_EQ_INT((int)export_size, 0);
    temporary = reg.canonical_active.entries[0];
    reg.canonical_active.entries[0] =
        reg.canonical_active.entries[1];
    reg.canonical_active.entries[1] = temporary;
    ASSERT(player_identity_registry_validate(&reg));

    ASSERT_EQ_INT(
        player_identity_registry_archive(
            &reg, first.pubkey),
        PLAYER_IDENTITY_REGISTRY_OK_ARCHIVED);
    player_identity_canonical_entry_t saved_active =
        reg.canonical_active.entries[0];
    memcpy(reg.canonical_active.entries[0].pubkey,
           reg.canonical_archive.entries[0].pubkey,
           PLAYER_IDENTITY_PUBKEY_SIZE);
    ASSERT(!player_identity_registry_validate(&reg));
    reg.canonical_active.entries[0] = saved_active;
    ASSERT(player_identity_registry_validate(&reg));

    ASSERT_EQ_INT(
        player_identity_registry_export_sensitive_size(
            &reg, &export_size),
        PLAYER_IDENTITY_REGISTRY_OK);
    uint8_t sentinel[16];
    memset(sentinel, 0x5a, sizeof(sentinel));
    uint8_t before[sizeof(sentinel)];
    memcpy(before, sentinel, sizeof(before));
    size_t required = 0;
    ASSERT_EQ_INT(
        player_identity_registry_export_sensitive(
            &reg, sentinel, sizeof(sentinel), &required),
        PLAYER_IDENTITY_REGISTRY_BUFFER_TOO_SMALL);
    ASSERT(required == export_size);
    ASSERT(memcmp(sentinel, before, sizeof(sentinel)) == 0);
}

TEST(test_player_identity_registry_clone_is_deep_and_cleanup_reusable) {
    REGISTRY_TEST_DECL(source);
    REGISTRY_TEST_DECL(destination);
    player_identity_registry_config_t config =
        registry_config(8, 8, 8);
    ASSERT_EQ_INT(
        player_identity_registry_init(&source, &config),
        PLAYER_IDENTITY_REGISTRY_OK);
    ASSERT_EQ_INT(
        player_identity_registry_init(&destination, &config),
        PLAYER_IDENTITY_REGISTRY_OK);

    player_identity_canonical_entry_t first =
        canonical_entry(1);
    player_identity_canonical_entry_t second =
        canonical_entry(2);
    player_identity_canonical_entry_t third =
        canonical_entry(3);
    ASSERT_EQ_INT(
        player_identity_registry_insert_active(
            &source, &first),
        PLAYER_IDENTITY_REGISTRY_OK_INSERTED);
    ASSERT_EQ_INT(
        player_identity_registry_insert_active(
            &source, &second),
        PLAYER_IDENTITY_REGISTRY_OK_INSERTED);
    ASSERT_EQ_INT(
        player_identity_registry_archive(
            &source, first.pubkey),
        PLAYER_IDENTITY_REGISTRY_OK_ARCHIVED);
    player_identity_quarantine_entry_t q1 =
        quarantine_entry(
            1, 0, 51,
            PLAYER_IDENTITY_QUARANTINE_REASON_INVALID_ACTOR);
    player_identity_quarantine_entry_t q2 =
        quarantine_entry(
            2, 1, 52,
            PLAYER_IDENTITY_QUARANTINE_REASON_UNPROVEN_MAPPING);
    ASSERT_EQ_INT(
        player_identity_registry_quarantine_append(
            &source, &q1),
        PLAYER_IDENTITY_REGISTRY_OK_QUARANTINED);
    ASSERT_EQ_INT(
        player_identity_registry_quarantine_append(
            &source, &q2),
        PLAYER_IDENTITY_REGISTRY_OK_QUARANTINED);
    player_identity_canonical_entry_t conflict = second;
    conflict.sensitive_actor_id[0] ^= 0x3cu;
    ASSERT(conflict.sensitive_actor_id[0] != 0);
    ASSERT_EQ_INT(
        player_identity_registry_insert_active(
            &source, &conflict),
        PLAYER_IDENTITY_REGISTRY_CONFLICT);

    ASSERT_EQ_INT(
        player_identity_registry_insert_active(
            &destination, &third),
        PLAYER_IDENTITY_REGISTRY_OK_INSERTED);
    ASSERT_EQ_INT(
        player_identity_registry_clone(
            &destination, &source),
        PLAYER_IDENTITY_REGISTRY_OK_CLONED);
    ASSERT(player_identity_registry_validate(&destination));
    ASSERT(source.canonical_active.entries !=
           destination.canonical_active.entries);
    ASSERT(source.canonical_archive.entries !=
           destination.canonical_archive.entries);
    ASSERT(source.quarantine.entries !=
           destination.quarantine.entries);

    REGISTRY_BUFFER_DECL(source_before);
    REGISTRY_BUFFER_DECL(destination_before);
    ASSERT(registry_capture_sensitive(
        &source, &source_before));
    ASSERT(registry_capture_sensitive(
        &destination, &destination_before));
    ASSERT(sensitive_buffers_equal(
        &source_before, &destination_before));

    player_identity_registry_metrics_t source_metrics;
    player_identity_registry_metrics_t destination_metrics;
    player_identity_registry_get_metrics(
        &source, &source_metrics);
    player_identity_registry_get_metrics(
        &destination, &destination_metrics);
    ASSERT_EQ_INT(
        (int)source_metrics.canonical_active.high_water,
        (int)destination_metrics.canonical_active.high_water);
    ASSERT_EQ_INT(
        (int)source_metrics.canonical_archive.high_water,
        (int)destination_metrics.canonical_archive.high_water);
    ASSERT_EQ_INT(
        (int)source_metrics.quarantine.high_water,
        (int)destination_metrics.quarantine.high_water);
    ASSERT_EQ_INT(
        (int)source_metrics.failure_status_counts[
            PLAYER_IDENTITY_REGISTRY_CONFLICT],
        (int)destination_metrics.failure_status_counts[
            PLAYER_IDENTITY_REGISTRY_CONFLICT]);

    ASSERT_EQ_INT(
        player_identity_registry_update(
            &source, second.pubkey,
            conflict.sensitive_actor_id),
        PLAYER_IDENTITY_REGISTRY_OK_UPDATED);
    player_identity_quarantine_entry_t q3 =
        quarantine_entry(
            3, 2, 53,
            PLAYER_IDENTITY_QUARANTINE_REASON_OPERATOR_HOLD);
    ASSERT_EQ_INT(
        player_identity_registry_quarantine_append(
            &source, &q3),
        PLAYER_IDENTITY_REGISTRY_OK_QUARANTINED);
    REGISTRY_BUFFER_DECL(destination_after);
    ASSERT(registry_capture_sensitive(
        &destination, &destination_after));
    ASSERT(sensitive_buffers_equal(
        &destination_before, &destination_after));

    ASSERT_EQ_INT(
        player_identity_registry_clone(&source, &source),
        PLAYER_IDENTITY_REGISTRY_OK_EXISTING);

    player_identity_registry_cleanup(&destination);
    player_identity_registry_t zero = {0};
    ASSERT(memcmp(&destination, &zero, sizeof(zero)) == 0);
    player_identity_registry_cleanup(&destination);
    ASSERT(memcmp(&destination, &zero, sizeof(zero)) == 0);
    ASSERT_EQ_INT(
        player_identity_registry_init(
            &destination, &config),
        PLAYER_IDENTITY_REGISTRY_OK);
    ASSERT(player_identity_registry_validate(&destination));
}

void register_player_identity_registry_tests(void) {
    TEST_SECTION("\nPlayer identity registry core tests:\n");
    RUN(test_player_identity_registry_contract_and_overflow);
    RUN(test_player_identity_registry_sensitive_export_wire_layout);
    RUN(test_player_identity_registry_scales_and_serializes_deterministically);
    RUN(test_player_identity_registry_insert_update_is_transactional);
    RUN(test_player_identity_registry_actor_ids_are_globally_unique);
    RUN(test_player_identity_registry_snapshots_aliased_inputs_before_growth);
    RUN(test_player_identity_registry_tier_budgets_are_independent);
    RUN(test_player_identity_registry_quarantine_retains_duplicate_evidence);
    RUN(test_player_identity_registry_archive_restore_and_wipe_tail);
    RUN(test_player_identity_registry_rejects_invalid_state_and_small_export);
    RUN(test_player_identity_registry_clone_is_deep_and_cleanup_reusable);
}
