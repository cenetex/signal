#include "test_harness.h"

#include "public_actor_presentation.h"

static public_actor_id_t presentation_actor(uint8_t first)
{
    public_actor_id_t actor = {
        .kind = PUBLIC_ACTOR_ID_DERIVED,
    };
    for (size_t i = 0; i < sizeof(actor.id); i++) {
        actor.id[i] = (uint8_t)(first + i);
    }
    return actor;
}

TEST(test_public_actor_presentation_keeps_full_id_rows_with_same_callsign) {
    client_scoreboard_t scoreboard = {0};
    public_actor_id_t actor_a = presentation_actor(0x10);
    public_actor_id_t actor_b = presentation_actor(0x80);

    int row_a = client_scoreboard_row_for_actor(
        &scoreboard, &actor_a, "TWIN", false);
    int row_b = client_scoreboard_row_for_actor(
        &scoreboard, &actor_b, "TWIN", false);
    ASSERT_EQ_INT(row_a, 0);
    ASSERT_EQ_INT(row_b, 1);
    ASSERT_EQ_INT(scoreboard.row_count, 2);
    ASSERT(!public_actor_id_equal(
        &scoreboard.rows[row_a].actor,
        &scoreboard.rows[row_b].actor));

    scoreboard.rows[row_a].kills = 3;
    scoreboard.rows[row_b].deaths = 2;
    ASSERT_EQ_INT(scoreboard.rows[row_a].kills, 3);
    ASSERT_EQ_INT(scoreboard.rows[row_a].deaths, 0);
    ASSERT_EQ_INT(scoreboard.rows[row_b].kills, 0);
    ASSERT_EQ_INT(scoreboard.rows[row_b].deaths, 2);

    char label_a[CLIENT_PUBLIC_ACTOR_DISPLAY_LABEL_CAP];
    char label_b[CLIENT_PUBLIC_ACTOR_DISPLAY_LABEL_CAP];
    ASSERT(client_scoreboard_format_row_label(
        &scoreboard, row_a, label_a, sizeof(label_a)));
    ASSERT(client_scoreboard_format_row_label(
        &scoreboard, row_b, label_b, sizeof(label_b)));
    ASSERT(strcmp(label_a, "TWIN#1011") == 0);
    ASSERT(strcmp(label_b, "TWIN#8081") == 0);
    ASSERT(strcmp(label_a, label_b) != 0);
}

TEST(test_public_actor_presentation_suffix_only_on_collision) {
    client_scoreboard_t scoreboard = {0};
    public_actor_id_t actor_a = presentation_actor(0x20);
    public_actor_id_t actor_b = presentation_actor(0x40);
    int row_a = client_scoreboard_row_for_actor(
        &scoreboard, &actor_a, "SOLO", false);
    int row_b = client_scoreboard_row_for_actor(
        &scoreboard, &actor_b, "OTHER", false);
    char label[CLIENT_PUBLIC_ACTOR_DISPLAY_LABEL_CAP];

    ASSERT(client_scoreboard_format_row_label(
        &scoreboard, row_a, label, sizeof(label)));
    ASSERT(strcmp(label, "SOLO") == 0);
    ASSERT(client_scoreboard_format_row_label(
        &scoreboard, row_b, label, sizeof(label)));
    ASSERT(strcmp(label, "OTHER") == 0);
}

TEST(test_public_actor_presentation_label_mutation_never_merges_keys) {
    client_scoreboard_t scoreboard = {0};
    public_actor_id_t actor_a = presentation_actor(0x31);
    public_actor_id_t actor_b = presentation_actor(0x61);
    int row_a = client_scoreboard_row_for_actor(
        &scoreboard, &actor_a, "ALPHA", false);
    int row_b = client_scoreboard_row_for_actor(
        &scoreboard, &actor_b, "BRAVO", false);

    ASSERT_EQ_INT(client_scoreboard_row_for_actor(
        &scoreboard, &actor_a, "TWIN", false), row_a);
    ASSERT_EQ_INT(client_scoreboard_row_for_actor(
        &scoreboard, &actor_b, "TWIN", false), row_b);
    ASSERT_EQ_INT(scoreboard.row_count, 2);
    ASSERT(public_actor_id_equal(
        &scoreboard.rows[row_a].actor, &actor_a));
    ASSERT(public_actor_id_equal(
        &scoreboard.rows[row_b].actor, &actor_b));
}

TEST(test_public_actor_presentation_expands_adversarial_common_prefix) {
    client_scoreboard_t scoreboard = {0};
    public_actor_id_t actor_a = presentation_actor(0x51);
    public_actor_id_t actor_b = actor_a;
    actor_b.id[PUBLIC_ACTOR_ID_SIZE - 1] ^= 0xffu;
    int row_a = client_scoreboard_row_for_actor(
        &scoreboard, &actor_a, "TWIN", false);
    int row_b = client_scoreboard_row_for_actor(
        &scoreboard, &actor_b, "TWIN", false);
    char label_a[CLIENT_PUBLIC_ACTOR_DISPLAY_LABEL_CAP];
    char label_b[CLIENT_PUBLIC_ACTOR_DISPLAY_LABEL_CAP];

    ASSERT(client_scoreboard_format_row_label(
        &scoreboard, row_a, label_a, sizeof(label_a)));
    ASSERT(client_scoreboard_format_actor_label(
        &scoreboard, &actor_b, label_b, sizeof(label_b)));
    ASSERT_EQ_INT(row_b, 1);
    ASSERT(strcmp(label_a, label_b) != 0);
    ASSERT_EQ_INT((int)strlen(label_a), 4 + 1 + PUBLIC_ACTOR_ID_SIZE * 2);
    ASSERT_EQ_INT((int)strlen(label_b), 4 + 1 + PUBLIC_ACTOR_ID_SIZE * 2);
}

TEST(test_public_actor_presentation_rejects_non_public_keys_and_small_output) {
    client_scoreboard_t scoreboard = {0};
    public_actor_id_t none = public_actor_id_none();
    public_actor_id_t actor_a = presentation_actor(0x70);
    public_actor_id_t actor_b = presentation_actor(0x90);
    ASSERT_EQ_INT(client_scoreboard_row_for_actor(
        &scoreboard, &none, "NONE", false), -1);
    int row = client_scoreboard_row_for_actor(
        &scoreboard, &actor_a, "TWIN", false);
    ASSERT(client_scoreboard_row_for_actor(
        &scoreboard, &actor_b, "TWIN", false) >= 0);

    char tiny[5] = "xxxx";
    ASSERT(!client_scoreboard_format_row_label(
        &scoreboard, row, tiny, sizeof(tiny)));
    ASSERT_EQ_INT(tiny[0], '\0');
}

TEST(test_public_actor_death_reducer_uses_full_ids_and_generic_remote_labels) {
    client_scoreboard_t scoreboard = {0};
    public_actor_id_t local = presentation_actor(0x11);
    public_actor_id_t remote_killer = presentation_actor(0x41);
    public_actor_id_t remote_victim = presentation_actor(0x71);

    client_scoreboard_event_result_t local_death =
        client_scoreboard_record_death(
            &scoreboard,
            &local,
            "LOCAL",
            &remote_killer,
            &local);
    ASSERT(local_death.subject_is_local);
    ASSERT(!local_death.source_is_local);
    ASSERT_EQ_INT(local_death.source_row, 0);
    ASSERT_EQ_INT(local_death.subject_row, 1);
    ASSERT(strcmp(scoreboard.rows[local_death.source_row].label, "Pilot") == 0);
    ASSERT(strcmp(scoreboard.rows[local_death.subject_row].label, "LOCAL") == 0);
    ASSERT_EQ_INT(scoreboard.rows[local_death.source_row].kills, 1);
    ASSERT_EQ_INT(scoreboard.rows[local_death.subject_row].deaths, 1);

    client_scoreboard_event_result_t local_kill =
        client_scoreboard_record_death(
            &scoreboard,
            &local,
            "LOCAL",
            &local,
            &remote_victim);
    ASSERT(local_kill.source_is_local);
    ASSERT(!local_kill.subject_is_local);
    ASSERT_EQ_INT(scoreboard.rows[local_kill.source_row].kills, 1);
    ASSERT(strcmp(scoreboard.rows[local_kill.subject_row].label, "Pilot") == 0);
    ASSERT_EQ_INT(scoreboard.rows[local_kill.subject_row].deaths, 1);
}

TEST(test_public_actor_reducers_reject_sentinels_and_do_not_credit_suicide) {
    client_scoreboard_t scoreboard = {0};
    public_actor_id_t local = presentation_actor(0x21);
    public_actor_id_t unattributed = public_actor_id_unattributed();
    public_actor_id_t legacy = public_actor_id_legacy_unattributed();

    client_scoreboard_event_result_t unknown =
        client_scoreboard_record_death(
            &scoreboard,
            &local,
            "LOCAL",
            &unattributed,
            &legacy);
    ASSERT_EQ_INT(unknown.source_row, -1);
    ASSERT_EQ_INT(unknown.subject_row, -1);
    ASSERT_EQ_INT(scoreboard.row_count, 0);

    client_scoreboard_event_result_t suicide =
        client_scoreboard_record_death(
            &scoreboard,
            &local,
            "LOCAL",
            &local,
            &local);
    ASSERT(suicide.source_is_local);
    ASSERT(suicide.subject_is_local);
    ASSERT_EQ_INT(suicide.source_row, -1);
    ASSERT_EQ_INT(suicide.subject_row, 0);
    ASSERT_EQ_INT(scoreboard.rows[0].kills, 0);
    ASSERT_EQ_INT(scoreboard.rows[0].deaths, 1);

    client_scoreboard_event_result_t npc =
        client_scoreboard_record_npc_kill(
            &scoreboard,
            &local,
            "LOCAL",
            &unattributed);
    ASSERT_EQ_INT(npc.source_row, -1);
    ASSERT_EQ_INT(scoreboard.row_count, 1);
}

void register_public_actor_presentation_tests(void)
{
    RUN(test_public_actor_presentation_keeps_full_id_rows_with_same_callsign);
    RUN(test_public_actor_presentation_suffix_only_on_collision);
    RUN(test_public_actor_presentation_label_mutation_never_merges_keys);
    RUN(test_public_actor_presentation_expands_adversarial_common_prefix);
    RUN(test_public_actor_presentation_rejects_non_public_keys_and_small_output);
    RUN(test_public_actor_death_reducer_uses_full_ids_and_generic_remote_labels);
    RUN(test_public_actor_reducers_reject_sentinels_and_do_not_credit_suicide);
}
