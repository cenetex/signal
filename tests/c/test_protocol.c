#include "test_harness.h"
#include "cargo_receipt_issue.h"
#include "station_authority.h"
#include "cargo_receipt_issue.h"
#include "faction.h"
#include "station_policy.h"
#include "wire_codec.h"

TEST(test_wire_codec_roundtrips_and_fails_closed_on_bounds) {
    uint8_t buf[18] = {0};
    wire_writer_t writer = wire_writer_init(buf, sizeof(buf));
    wire_put_u16(&writer, 0xBEEF);
    wire_put_i16(&writer, -1234);
    wire_put_u32(&writer, 0x89ABCDEFu);
    wire_put_u64(&writer, 0x0123456789ABCDEFull);
    wire_put_f32(&writer, 12.5f);
    ASSERT(!writer.ok); /* final float exceeds the fixed record */
    ASSERT_EQ_INT(writer.offset, 16);

    wire_reader_t reader = wire_reader_init(buf, 16);
    ASSERT_EQ_INT(wire_get_u16(&reader), 0xBEEF);
    ASSERT_EQ_INT(wire_get_i16(&reader), -1234);
    ASSERT(wire_get_u32(&reader) == 0x89ABCDEFu);
    ASSERT(wire_get_u64(&reader) == 0x0123456789ABCDEFull);
    ASSERT(reader.ok);
    ASSERT_EQ_FLOAT(wire_get_f32(&reader), 0.0f, 0.0f);
    ASSERT(!reader.ok);
}

TEST(test_roundtrip_player_state) {
    SERVER_PLAYER_DECL(sp);
    sp.ship->pos = v2(123.45f, -678.9f);
    sp.ship->vel = v2(1.5f, -2.5f);
    sp.ship->angle = 2.34f;
    sp.docked = true;
    sp.actual_thrusting = true;
    sp.beam_active = true;
    sp.beam_hit = true;

    uint8_t buf[64];
    int len = serialize_player_state(buf, 7, &sp);

    /* Size must be 45 (widened towed_frags uint8→uint16 in #285 Phase 3) */
    ASSERT_EQ_INT(len, 45);
    ASSERT_EQ_INT(buf[0], NET_MSG_STATE);
    ASSERT_EQ_INT(buf[1], 7);

    /* Verify floats roundtrip */
    ASSERT_EQ_FLOAT(read_f32_le(&buf[2]), 123.45f, 0.01f);
    ASSERT_EQ_FLOAT(read_f32_le(&buf[6]), -678.9f, 0.1f);
    ASSERT_EQ_FLOAT(read_f32_le(&buf[10]), 1.5f, 0.01f);
    ASSERT_EQ_FLOAT(read_f32_le(&buf[14]), -2.5f, 0.01f);
    ASSERT_EQ_FLOAT(read_f32_le(&buf[18]), 2.34f, 0.01f);

    /* Verify flags byte */
    uint8_t flags = buf[22];
    ASSERT(flags & 1);   /* thrusting */
    ASSERT(flags & 2);   /* beam active + hit */
    ASSERT(flags & 4);   /* docked */
}

TEST(test_authoritative_player_state_includes_ack_tail) {
    SERVER_PLAYER_DECL(sp);
    sp.ship->pos = v2(10.0f, 20.0f);
    sp.ship->vel = v2(3.0f, 4.0f);
    sp.ship->angle = 1.25f;
    sp.last_input_seq = 0x1234u;
    sp.last_input_tick = 0x01020304u;
    sp.last_input_client_sent_ms = 0x11223344u;
    sp.last_input_server_recv_ms = 0x55667788u;

    uint8_t buf[NET_STATE_AUTH_SIZE];
    int len = serialize_authoritative_player_state(buf, 2, &sp, 0xAABBCCDDu);

    ASSERT_EQ_INT(len, NET_STATE_AUTH_SIZE);
    ASSERT_EQ_INT(buf[0], NET_MSG_STATE);
    ASSERT_EQ_INT(buf[1], 2);
    ASSERT_EQ_FLOAT(read_f32_le(&buf[2]), 10.0f, 0.01f);
    ASSERT_EQ_INT((int)read_u16_le(&buf[NET_STATE_AUTH_INPUT_ACK_OFFSET]),
                  0x1234);
    ASSERT_EQ_INT((int)read_u32_le(&buf[NET_STATE_AUTH_SERVER_TICK_OFFSET]),
                  (int)0xAABBCCDDu);
    ASSERT_EQ_INT((int)read_u32_le(&buf[NET_STATE_AUTH_INPUT_TICK_OFFSET]),
                  0x01020304);
    ASSERT_EQ_INT((int)read_u32_le(
                      &buf[NET_STATE_AUTH_CLIENT_SENT_MS_OFFSET]),
                  (int)0x11223344u);
    ASSERT_EQ_INT((int)read_u32_le(
                      &buf[NET_STATE_AUTH_SERVER_RECV_MS_OFFSET]),
                  (int)0x55667788u);
    ASSERT_EQ_INT((int)read_u32_le(
                      &buf[NET_STATE_AUTH_SERVER_SEND_MS_OFFSET]),
                  0);
}

TEST(test_roundtrip_batched_player_states) {
    SERVER_PLAYER_ARRAY(players, MAX_PLAYERS);

    /* Two connected players */
    players[0].connected = true;
    players[0].ship->pos = v2(100.0f, 200.0f);
    players[0].ship->vel = v2(1.0f, -1.0f);
    players[0].ship->angle = 1.5f;
    players[0].actual_thrusting = true;
    players[0].docked = false;
    players[0].last_input_seq = 321;
    players[0].last_input_tick = 12340u;

    players[3].connected = true;
    players[3].ship->pos = v2(-50.0f, 300.0f);
    players[3].ship->vel = v2(0.0f, 2.0f);
    players[3].ship->angle = 3.14f;
    players[3].docked = true;
    players[3].ship->tractor_active = true;
    players[3].ship->tractor_level = 2;
    players[3].ship->towed_count = 2;
    players[3].ship->towed_fragments[0] = 301;
    players[3].ship->towed_fragments[1] = 1024;

    uint8_t buf[2 + MAX_PLAYERS * PLAYER_RECORD_SIZE];
    int len = serialize_all_player_states(buf, players, 12345u);

    /* Should have 2 records */
    ASSERT_EQ_INT(buf[0], NET_MSG_WORLD_PLAYERS);
    ASSERT_EQ_INT(buf[1], 2);
    ASSERT_EQ_INT(len, 2 + 2 * PLAYER_RECORD_SIZE);

    /* First record: player 0 */
    uint8_t *p0 = &buf[2];
    ASSERT_EQ_INT(p0[0], 0);
    ASSERT_EQ_FLOAT(read_f32_le(&p0[1]), 100.0f, 0.01f);
    ASSERT_EQ_FLOAT(read_f32_le(&p0[5]), 200.0f, 0.01f);
    ASSERT(p0[21] & 1); /* thrusting */
    ASSERT(!(p0[21] & 4)); /* not docked */
    ASSERT_EQ_INT((int)((uint16_t)p0[67] | ((uint16_t)p0[68] << 8)), 321);
    ASSERT_EQ_INT((int)read_u32_le(&p0[69]), 12345);
    ASSERT_EQ_INT((int)read_u32_le(&p0[73]), 12340);

    /* Second record: player 3 */
    uint8_t *p1 = &buf[2 + PLAYER_RECORD_SIZE];
    ASSERT_EQ_INT(p1[0], 3);
    ASSERT_EQ_FLOAT(read_f32_le(&p1[1]), -50.0f, 0.01f);
    ASSERT(p1[21] & 4); /* docked */
    ASSERT(p1[21] & 16); /* tractor active */
    ASSERT_EQ_INT(p1[22], 2);
    ASSERT_EQ_INT(p1[23], 2);
    ASSERT_EQ_INT((int)((uint16_t)p1[24] | ((uint16_t)p1[25] << 8)), 301);
    ASSERT_EQ_INT((int)((uint16_t)p1[26] | ((uint16_t)p1[27] << 8)), 1024);
}

TEST(test_player_states_for_recipient_excludes_self) {
    SERVER_PLAYER_ARRAY(players, MAX_PLAYERS);

    players[0].connected = true;
    players[0].ship->pos = v2(100.0f, 200.0f);
    players[0].last_input_seq = 12;
    players[0].last_input_tick = 1200u;

    players[3].connected = true;
    players[3].ship->pos = v2(-50.0f, 300.0f);
    players[3].last_input_seq = 34;
    players[3].last_input_tick = 3400u;

    uint8_t buf[2 + MAX_PLAYERS * PLAYER_RECORD_SIZE];
    int len = serialize_player_states_except_recipient(
        buf, players, 0, 12345u);

    ASSERT_EQ_INT(buf[0], NET_MSG_WORLD_PLAYERS);
    ASSERT_EQ_INT(buf[1], 1);
    ASSERT_EQ_INT(len, 2 + PLAYER_RECORD_SIZE);
    ASSERT_EQ_INT(buf[2], 3);
    ASSERT_EQ_FLOAT(read_f32_le(&buf[3]), -50.0f, 0.01f);

    len = serialize_player_states_except_recipient(buf, players, 3, 12346u);
    ASSERT_EQ_INT(buf[1], 1);
    ASSERT_EQ_INT(len, 2 + PLAYER_RECORD_SIZE);
    ASSERT_EQ_INT(buf[2], 0);
    ASSERT_EQ_FLOAT(read_f32_le(&buf[3]), 100.0f, 0.01f);

    len = serialize_player_states_except_recipient(buf, players, -1, 12347u);
    ASSERT_EQ_INT(buf[1], 2);
    ASSERT_EQ_INT(len, 2 + 2 * PLAYER_RECORD_SIZE);

    players[0].beam_active = true;
    players[0].beam_end = v2(125.0f, 225.0f);
    len = serialize_player_states_except_recipient(buf, players, 0, 12348u);
    ASSERT_EQ_INT(buf[1], 2);
    ASSERT_EQ_INT(len, 2 + 2 * PLAYER_RECORD_SIZE);
    ASSERT_EQ_INT(buf[2], 0);
    ASSERT(buf[2 + 21] & 2);
    ASSERT_EQ_FLOAT(read_f32_le(&buf[2 + 59]), 125.0f, 0.01f);
}

TEST(test_player_motion_stream_excludes_recipient_and_docked_players) {
    SERVER_PLAYER_ARRAY(players, MAX_PLAYERS);

    players[0].connected = true;
    players[0].session_ready = true;
    players[0].ship->pos = v2(10.0f, 20.0f);
    players[0].ship->vel = v2(1.0f, 2.0f);
    players[0].ship->angle = 0.25f;

    players[1].connected = true;
    players[1].session_ready = true;
    players[1].ship->pos = v2(100.0f, 200.0f);
    players[1].ship->vel = v2(-1.0f, 3.0f);
    players[1].ship->angle = 1.5f;

    players[3].connected = true;
    players[3].session_ready = true;
    players[3].docked = true;
    players[3].ship->pos = v2(300.0f, 400.0f);

    uint8_t buf[PLAYER_MOTION_MSG_HEADER +
                MAX_PLAYERS * PLAYER_MOTION_RECORD_SIZE];
    int len = serialize_player_motion_for_recipient(buf, players, 0);

    ASSERT_EQ_INT(buf[0], NET_MSG_WORLD_PLAYER_MOTION);
    ASSERT_EQ_INT(buf[1], 1);
    ASSERT_EQ_INT(len, PLAYER_MOTION_MSG_HEADER + PLAYER_MOTION_RECORD_SIZE);
    const uint8_t *p = &buf[PLAYER_MOTION_MSG_HEADER];
    ASSERT_EQ_INT(p[0], 1);
    ASSERT_EQ_FLOAT(read_f32_le(&p[1]), 100.0f, 0.01f);
    ASSERT_EQ_FLOAT(read_f32_le(&p[5]), 200.0f, 0.01f);
    ASSERT_EQ_FLOAT(read_f32_le(&p[9]), -1.0f, 0.01f);
    ASSERT_EQ_FLOAT(read_f32_le(&p[13]), 3.0f, 0.01f);
    ASSERT_EQ_FLOAT(read_f32_le(&p[17]), 1.5f, 0.01f);
}

TEST(test_player_motion_q_stream_quantizes_remote_players) {
    SERVER_PLAYER_ARRAY(players, MAX_PLAYERS);

    players[0].connected = true;
    players[0].session_ready = true;
    players[0].ship->pos = v2(10.0f, 20.0f);
    players[0].ship->vel = v2(1.0f, 2.0f);
    players[0].ship->angle = 0.25f;

    players[1].connected = true;
    players[1].session_ready = true;
    players[1].ship->pos = v2(101.0f, -202.0f);
    players[1].ship->vel = v2(-1.25f, 3.5f);
    players[1].ship->angle = 1.57079632679f;

    players[3].connected = true;
    players[3].session_ready = true;
    players[3].docked = true;
    players[3].ship->pos = v2(300.0f, 400.0f);

    uint8_t buf[PLAYER_MOTION_Q_MSG_HEADER +
                MAX_PLAYERS * PLAYER_MOTION_Q_RECORD_SIZE];
    int len = serialize_player_motion_q_for_recipient(buf, players, 0);

    ASSERT_EQ_INT(buf[0], NET_MSG_WORLD_PLAYER_MOTION_Q);
    ASSERT_EQ_INT(buf[1], 1);
    ASSERT_EQ_INT(len,
                  PLAYER_MOTION_Q_MSG_HEADER + PLAYER_MOTION_Q_RECORD_SIZE);
    const uint8_t *p = &buf[PLAYER_MOTION_Q_MSG_HEADER];
    ASSERT_EQ_INT(p[0], 1);
    ASSERT_EQ_INT((int)(int16_t)read_u16_le(&p[1]), 25);
    ASSERT_EQ_INT((int)(int16_t)read_u16_le(&p[3]), -51);
    ASSERT_EQ_INT((int)(int16_t)read_u16_le(&p[5]), -5);
    ASSERT_EQ_INT((int)(int16_t)read_u16_le(&p[7]), 14);
    ASSERT_EQ_INT(p[9], 64);
}

TEST(test_player_motion_delta_q_stream_uses_baseline) {
    SERVER_PLAYER_ARRAY(players, MAX_PLAYERS);
    SERVER_PLAYER_DECL(recipient);

    players[1].connected = true;
    players[1].session_ready = true;
    players[1].docked = false;
    players[1].ship->pos = v2(108.0f, -192.0f);
    players[1].ship->vel = v2(64.0f, -32.0f);
    players[1].ship->angle = PI_F * 0.5f;

    uint8_t abs_buf[PLAYER_MOTION_Q_MSG_HEADER +
                    MAX_PLAYERS * PLAYER_MOTION_Q_RECORD_SIZE];
    uint8_t delta_buf[PLAYER_MOTIOND_Q_MSG_HEADER +
                      MAX_PLAYERS * PLAYER_MOTIOND_Q_RECORD_SIZE];
    uint8_t posed_buf[PLAYER_POSED_Q_MSG_HEADER +
                      MAX_PLAYERS * PLAYER_POSED_Q_RECORD_SIZE];
    int abs_len = 0;
    int delta_len = 0;
    int posed_len = 0;
    int records = serialize_player_motion_split_q_for_recipient(
        abs_buf, &abs_len, delta_buf, &delta_len,
        posed_buf, &posed_len,
        &recipient, players, 0, 100u);

    ASSERT_EQ_INT(records, 1);
    ASSERT_EQ_INT(abs_buf[0], NET_MSG_WORLD_PLAYER_MOTION_Q);
    ASSERT_EQ_INT(abs_buf[1], 1);
    ASSERT_EQ_INT(abs_len,
                  PLAYER_MOTION_Q_MSG_HEADER + PLAYER_MOTION_Q_RECORD_SIZE);
    ASSERT_EQ_INT(delta_buf[0], NET_MSG_WORLD_PLAYER_MOTIOND_Q);
    ASSERT_EQ_INT(delta_buf[1], 0);
    ASSERT_EQ_INT(delta_len, PLAYER_MOTIOND_Q_MSG_HEADER);
    ASSERT_EQ_INT(posed_buf[0], NET_MSG_WORLD_PLAYER_POSED_Q);
    ASSERT_EQ_INT(posed_buf[1], 0);
    ASSERT_EQ_INT(posed_len, PLAYER_POSED_Q_MSG_HEADER);
    server_player_motion_delta_note_abs_msg(
        &recipient, abs_buf, (size_t)abs_len, 100u);
    ASSERT(recipient.replication->player_motion_delta_valid[1]);
    ASSERT_EQ_INT(recipient.replication->player_motion_delta_qx[1], 27);
    ASSERT_EQ_INT(recipient.replication->player_motion_delta_qy[1], -48);
    ASSERT_EQ_FLOAT(recipient.replication->player_motion_delta_vel[1].x, 64.0f, 0.01f);
    ASSERT_EQ_FLOAT(recipient.replication->player_motion_delta_vel[1].y, -32.0f, 0.01f);
    ASSERT_EQ_INT(recipient.replication->player_motion_delta_angle[1], 64);
    ASSERT_EQ_INT((int)recipient.replication->player_motion_delta_tick[1], 100);

    players[1].ship->pos = v2(116.0f, -184.0f);
    players[1].ship->vel = v2(66.0f, -34.0f);
    records = serialize_player_motion_split_q_for_recipient(
        abs_buf, &abs_len, delta_buf, &delta_len,
        posed_buf, &posed_len,
        &recipient, players, 0,
        100u + PLAYER_MOTION_NET_MIN_REPEAT_TICKS);

    ASSERT_EQ_INT(records, 1);
    ASSERT_EQ_INT(abs_buf[1], 0);
    ASSERT_EQ_INT(abs_len, PLAYER_MOTION_Q_MSG_HEADER);
    ASSERT_EQ_INT(delta_buf[0], NET_MSG_WORLD_PLAYER_MOTIOND_Q);
    ASSERT_EQ_INT(delta_buf[1], 1);
    ASSERT_EQ_INT(delta_len,
                  PLAYER_MOTIOND_Q_MSG_HEADER +
                  PLAYER_MOTIOND_Q_RECORD_SIZE);
    ASSERT_EQ_INT(posed_buf[1], 0);
    ASSERT_EQ_INT(posed_len, PLAYER_POSED_Q_MSG_HEADER);
    const uint8_t *p = &delta_buf[PLAYER_MOTIOND_Q_MSG_HEADER];
    ASSERT_EQ_INT(p[0], 1);
    ASSERT_EQ_INT((int)(int8_t)p[1], 2);
    ASSERT_EQ_INT((int)(int8_t)p[2], 2);
    ASSERT_EQ_INT((int)(int8_t)p[3], 17);
    ASSERT_EQ_INT((int)(int8_t)p[4], -9);
    ASSERT_EQ_INT(p[5], 64);

    server_player_motion_delta_note_delta_msg(
        &recipient, delta_buf, (size_t)delta_len,
        100u + PLAYER_MOTION_NET_MIN_REPEAT_TICKS);
    ASSERT_EQ_INT(recipient.replication->player_motion_delta_qx[1], 29);
    ASSERT_EQ_INT(recipient.replication->player_motion_delta_qy[1], -46);
    ASSERT_EQ_FLOAT(recipient.replication->player_motion_delta_vel[1].x, 68.0f, 0.01f);
    ASSERT_EQ_FLOAT(recipient.replication->player_motion_delta_vel[1].y, -36.0f, 0.01f);
    ASSERT_EQ_INT((int)recipient.replication->player_motion_delta_tick[1],
                  100 + (int)PLAYER_MOTION_NET_MIN_REPEAT_TICKS);
}

TEST(test_player_motion_delta_q_skips_predicted_motion_until_heartbeat) {
    ASSERT_EQ_INT((int)PLAYER_MOTION_NET_HEARTBEAT_TICKS, 240);

    SERVER_PLAYER_ARRAY(players, MAX_PLAYERS);
    SERVER_PLAYER_DECL(recipient);

    players[1].connected = true;
    players[1].session_ready = true;
    players[1].docked = false;
    players[1].ship->pos = v2(108.0f, -192.0f);
    players[1].ship->vel = v2(64.0f, -32.0f);
    players[1].ship->angle = PI_F * 0.5f;

    uint8_t abs_buf[PLAYER_MOTION_Q_MSG_HEADER +
                    MAX_PLAYERS * PLAYER_MOTION_Q_RECORD_SIZE];
    uint8_t delta_buf[PLAYER_MOTIOND_Q_MSG_HEADER +
                      MAX_PLAYERS * PLAYER_MOTIOND_Q_RECORD_SIZE];
    uint8_t posed_buf[PLAYER_POSED_Q_MSG_HEADER +
                      MAX_PLAYERS * PLAYER_POSED_Q_RECORD_SIZE];
    int abs_len = 0;
    int delta_len = 0;
    int posed_len = 0;
    int records = serialize_player_motion_split_q_for_recipient(
        abs_buf, &abs_len, delta_buf, &delta_len,
        posed_buf, &posed_len,
        &recipient, players, 0, 100u);
    ASSERT_EQ_INT(records, 1);
    server_player_motion_delta_note_abs_msg(
        &recipient, abs_buf, (size_t)abs_len, 100u);

    float dt = (float)PLAYER_MOTION_NET_MIN_REPEAT_TICKS * SIM_DT;
    players[1].ship->pos = v2(108.0f + 64.0f * dt,
                             -192.0f - 32.0f * dt);
    records = serialize_player_motion_split_q_for_recipient(
        abs_buf, &abs_len, delta_buf, &delta_len,
        posed_buf, &posed_len,
        &recipient, players, 0,
        100u + PLAYER_MOTION_NET_MIN_REPEAT_TICKS);
    ASSERT_EQ_INT(records, 0);
    ASSERT_EQ_INT(abs_len, PLAYER_MOTION_Q_MSG_HEADER);
    ASSERT_EQ_INT(delta_len, PLAYER_MOTIOND_Q_MSG_HEADER);
    ASSERT_EQ_INT(posed_len, PLAYER_POSED_Q_MSG_HEADER);
    ASSERT_EQ_INT(abs_buf[1], 0);
    ASSERT_EQ_INT(delta_buf[1], 0);
    ASSERT_EQ_INT(posed_buf[1], 0);

    dt = (float)PLAYER_MOTION_NET_HEARTBEAT_TICKS * SIM_DT;
    players[1].ship->pos = v2(108.0f + 64.0f * dt,
                             -192.0f - 32.0f * dt);
    players[1].ship->angle = PI_F * 0.5f + 0.1f;
    records = serialize_player_motion_split_q_for_recipient(
        abs_buf, &abs_len, delta_buf, &delta_len,
        posed_buf, &posed_len,
        &recipient, players, 0,
        100u + PLAYER_MOTION_NET_HEARTBEAT_TICKS);
    ASSERT_EQ_INT(records, 1);
    ASSERT_EQ_INT(abs_buf[1], 0);
    ASSERT_EQ_INT(delta_buf[1], 0);
    ASSERT_EQ_INT(posed_buf[0], NET_MSG_WORLD_PLAYER_POSED_Q);
    ASSERT_EQ_INT(posed_buf[1], 1);
    ASSERT_EQ_INT(posed_len,
                  PLAYER_POSED_Q_MSG_HEADER + PLAYER_POSED_Q_RECORD_SIZE);
    const uint8_t *p = &posed_buf[PLAYER_POSED_Q_MSG_HEADER];
    ASSERT_EQ_INT(p[0], 1);
    ASSERT_EQ_INT((int)(int8_t)p[1],
                  (int)player_motion_q_encode(players[1].ship->pos.x,
                                               PLAYER_MOTION_Q_POS_SCALE) -
                  (int)recipient.replication->player_motion_delta_qx[1]);
    ASSERT_EQ_INT((int)(int8_t)p[2],
                  (int)player_motion_q_encode(players[1].ship->pos.y,
                                               PLAYER_MOTION_Q_POS_SCALE) -
                  (int)recipient.replication->player_motion_delta_qy[1]);
    ASSERT_EQ_INT(p[3], player_motion_q_angle(players[1].ship->angle));

    server_player_motion_delta_note_posed_msg(
        &recipient, posed_buf, (size_t)posed_len,
        100u + PLAYER_MOTION_NET_HEARTBEAT_TICKS);
    ASSERT_EQ_FLOAT(recipient.replication->player_motion_delta_vel[1].x, 64.0f, 0.01f);
    ASSERT_EQ_FLOAT(recipient.replication->player_motion_delta_vel[1].y, -32.0f, 0.01f);
    ASSERT_EQ_INT(recipient.replication->player_motion_delta_angle[1], p[3]);
}

TEST(test_player_motion_mixed_q_combines_delta_and_pose_records) {
    SERVER_PLAYER_ARRAY(players, MAX_PLAYERS);
    SERVER_PLAYER_DECL(recipient);

    players[1].connected = true;
    players[1].session_ready = true;
    players[1].docked = false;
    players[1].ship->pos = v2(408.0f, -792.0f);
    players[1].ship->vel = v2(68.0f, -36.0f);
    players[1].ship->angle = PI_F * 0.5f;

    players[2].connected = true;
    players[2].session_ready = true;
    players[2].docked = false;
    players[2].ship->pos = v2(168.0f, 328.0f);
    players[2].ship->vel = v2(79.5f, 52.0f);
    players[2].ship->angle = 0.35f;

    server_player_motion_delta_note_q(
        &recipient, 1, 100, -200, v2(64.0f, -32.0f), 64, 100u);
    server_player_motion_delta_note_q(
        &recipient, 2, 40, 80, v2(79.5f, 52.0f), 10, 100u);

    uint8_t abs_buf[PLAYER_MOTION_Q_MSG_HEADER +
                    MAX_PLAYERS * PLAYER_MOTION_Q_RECORD_SIZE];
    uint8_t mixed_buf[PLAYER_MOTIONM_Q_MSG_HEADER +
                      MAX_PLAYERS * PLAYER_MOTIONM_Q_MAX_RECORD_SIZE];
    int abs_len = 0;
    int mixed_len = 0;
    bool heartbeat_due = false;
    int records = serialize_player_motion_mixed_q_for_recipient(
        abs_buf, &abs_len, mixed_buf, &mixed_len,
        &heartbeat_due,
        &recipient, players, 0,
        100u + PLAYER_MOTION_NET_HEARTBEAT_TICKS);

    ASSERT_EQ_INT(records, 2);
    ASSERT(heartbeat_due);
    ASSERT_EQ_INT(abs_buf[0], NET_MSG_WORLD_PLAYER_MOTION_Q);
    ASSERT_EQ_INT(abs_buf[1], 0);
    ASSERT_EQ_INT(abs_len, PLAYER_MOTION_Q_MSG_HEADER);
    ASSERT_EQ_INT(mixed_buf[0], NET_MSG_WORLD_PLAYER_MOTIONM_Q);
    ASSERT_EQ_INT(mixed_buf[1], 2);
    ASSERT_EQ_INT(mixed_len,
                  PLAYER_MOTIONM_Q_MSG_HEADER +
                  PLAYER_MOTIONM_Q_VEL_RECORD_SIZE +
                  PLAYER_MOTIONM_Q_POSE_RECORD_SIZE);

    const uint8_t *vel = &mixed_buf[PLAYER_MOTIONM_Q_MSG_HEADER];
    ASSERT_EQ_INT(vel[0], 1 | PLAYER_MOTIONM_Q_FLAG_VEL);
    ASSERT_EQ_INT((int)(int8_t)vel[1], 2);
    ASSERT_EQ_INT((int)(int8_t)vel[2], 2);
    ASSERT_EQ_INT((int)(int8_t)vel[3], 17);
    ASSERT_EQ_INT((int)(int8_t)vel[4], -9);
    ASSERT_EQ_INT(vel[5], player_motion_q_angle(players[1].ship->angle));

    const uint8_t *pose =
        &mixed_buf[PLAYER_MOTIONM_Q_MSG_HEADER +
                   PLAYER_MOTIONM_Q_VEL_RECORD_SIZE];
    ASSERT_EQ_INT(pose[0], 2);
    ASSERT_EQ_INT((pose[0] & PLAYER_MOTIONM_Q_FLAG_VEL), 0);
    ASSERT_EQ_INT((int)(int8_t)pose[1], 2);
    ASSERT_EQ_INT((int)(int8_t)pose[2], 2);
    ASSERT_EQ_INT(pose[3], player_motion_q_angle(players[2].ship->angle));

    server_player_motion_delta_note_mixed_msg(
        &recipient, mixed_buf, (size_t)mixed_len,
        100u + PLAYER_MOTION_NET_HEARTBEAT_TICKS);
    ASSERT_EQ_INT(recipient.replication->player_motion_delta_qx[1], 102);
    ASSERT_EQ_INT(recipient.replication->player_motion_delta_qy[1], -198);
    ASSERT_EQ_FLOAT(recipient.replication->player_motion_delta_vel[1].x, 68.0f, 0.01f);
    ASSERT_EQ_FLOAT(recipient.replication->player_motion_delta_vel[1].y, -36.0f, 0.01f);
    ASSERT_EQ_INT(recipient.replication->player_motion_delta_qx[2], 42);
    ASSERT_EQ_INT(recipient.replication->player_motion_delta_qy[2], 82);
    ASSERT_EQ_FLOAT(recipient.replication->player_motion_delta_vel[2].x, 79.5f, 0.01f);
    ASSERT_EQ_FLOAT(recipient.replication->player_motion_delta_vel[2].y, 52.0f, 0.01f);
    ASSERT_EQ_INT(recipient.replication->player_motion_delta_angle[2], pose[3]);
}

TEST(test_player_motion_mixed_q_coalesces_clean_heartbeats) {
    SERVER_PLAYER_ARRAY(players, MAX_PLAYERS);
    SERVER_PLAYER_DECL(recipient);

    players[1].connected = true;
    players[1].session_ready = true;
    players[1].docked = false;
    players[1].ship->pos = v2(108.0f, -192.0f);
    players[1].ship->vel = v2(64.0f, -32.0f);
    players[1].ship->angle = PI_F * 0.5f;

    server_player_motion_delta_note_q(
        &recipient, 1, 27, -48, players[1].ship->vel,
        player_motion_q_angle(players[1].ship->angle), 100u);
    recipient.replication->player_motion_delta_heartbeat_tick = 100u;

    uint8_t abs_buf[PLAYER_MOTION_Q_MSG_HEADER +
                    MAX_PLAYERS * PLAYER_MOTION_Q_RECORD_SIZE];
    uint8_t mixed_buf[PLAYER_MOTIONM_Q_MSG_HEADER +
                      MAX_PLAYERS * PLAYER_MOTIONM_Q_MAX_RECORD_SIZE];
    int abs_len = 0;
    int mixed_len = 0;
    bool heartbeat_due = false;

    float dt = (float)PLAYER_MOTION_NET_MIN_REPEAT_TICKS * SIM_DT;
    players[1].ship->pos = v2(108.0f + 64.0f * dt,
                             -192.0f - 32.0f * dt);
    int records = serialize_player_motion_mixed_q_for_recipient(
        abs_buf, &abs_len, mixed_buf, &mixed_len,
        &heartbeat_due,
        &recipient, players, 0,
        100u + PLAYER_MOTION_NET_MIN_REPEAT_TICKS);
    ASSERT(!heartbeat_due);
    ASSERT_EQ_INT(records, 0);
    ASSERT_EQ_INT(abs_len, PLAYER_MOTION_Q_MSG_HEADER);
    ASSERT_EQ_INT(mixed_len, PLAYER_MOTIONM_Q_MSG_HEADER);

    vec2 recent_pos = players[1].ship->pos;
    server_player_motion_delta_note_q(
        &recipient, 1,
        player_motion_q_encode(recent_pos.x, PLAYER_MOTION_Q_POS_SCALE),
        player_motion_q_encode(recent_pos.y, PLAYER_MOTION_Q_POS_SCALE),
        players[1].ship->vel,
        player_motion_q_angle(players[1].ship->angle),
        100u + PLAYER_MOTION_NET_MIN_REPEAT_TICKS);
    dt = (float)(PLAYER_MOTION_NET_HEARTBEAT_TICKS -
                 PLAYER_MOTION_NET_MIN_REPEAT_TICKS) * SIM_DT;
    players[1].ship->pos = v2(recent_pos.x + 64.0f * dt,
                             recent_pos.y - 32.0f * dt);
    records = serialize_player_motion_mixed_q_for_recipient(
        abs_buf, &abs_len, mixed_buf, &mixed_len,
        &heartbeat_due,
        &recipient, players, 0,
        100u + PLAYER_MOTION_NET_HEARTBEAT_TICKS);
    ASSERT(heartbeat_due);
    ASSERT_EQ_INT(records, 0);
    ASSERT_EQ_INT(abs_len, PLAYER_MOTION_Q_MSG_HEADER);
    ASSERT_EQ_INT(mixed_len, PLAYER_MOTIONM_Q_MSG_HEADER);

    server_player_motion_delta_note_q(
        &recipient, 1, 27, -48, players[1].ship->vel,
        player_motion_q_angle(players[1].ship->angle), 100u);
    recipient.replication->player_motion_delta_heartbeat_tick = 100u;
    dt = (float)PLAYER_MOTION_NET_HEARTBEAT_TICKS * SIM_DT;
    players[1].ship->pos = v2(108.0f + 64.0f * dt,
                             -192.0f - 32.0f * dt);
    records = serialize_player_motion_mixed_q_for_recipient(
        abs_buf, &abs_len, mixed_buf, &mixed_len,
        &heartbeat_due,
        &recipient, players, 0,
        100u + PLAYER_MOTION_NET_HEARTBEAT_TICKS);
    ASSERT(heartbeat_due);
    ASSERT_EQ_INT(records, 1);
    ASSERT_EQ_INT(abs_buf[1], 0);
    ASSERT_EQ_INT(mixed_buf[0], NET_MSG_WORLD_PLAYER_MOTIONM_Q);
    ASSERT_EQ_INT(mixed_buf[1], 1);
    ASSERT_EQ_INT(mixed_len,
                  PLAYER_MOTIONM_Q_MSG_HEADER +
                  PLAYER_MOTIONM_Q_POSE_RECORD_SIZE);
}

TEST(test_player_motion_delta_q_falls_back_when_delta_exceeds_i8) {
    SERVER_PLAYER_ARRAY(players, MAX_PLAYERS);
    SERVER_PLAYER_DECL(recipient);

    players[1].connected = true;
    players[1].session_ready = true;
    players[1].ship->pos = v2(800.0f, 0.0f);
    players[1].ship->vel = v2(400.0f, 0.0f);
    server_player_motion_delta_note_q(
        &recipient, 1, 0, 0, v2(0.0f, 0.0f), 0, 100u);

    uint8_t abs_buf[PLAYER_MOTION_Q_MSG_HEADER +
                    MAX_PLAYERS * PLAYER_MOTION_Q_RECORD_SIZE];
    uint8_t delta_buf[PLAYER_MOTIOND_Q_MSG_HEADER +
                      MAX_PLAYERS * PLAYER_MOTIOND_Q_RECORD_SIZE];
    uint8_t posed_buf[PLAYER_POSED_Q_MSG_HEADER +
                      MAX_PLAYERS * PLAYER_POSED_Q_RECORD_SIZE];
    int abs_len = 0;
    int delta_len = 0;
    int posed_len = 0;
    int records = serialize_player_motion_split_q_for_recipient(
        abs_buf, &abs_len, delta_buf, &delta_len,
        posed_buf, &posed_len,
        &recipient, players, 0, 101u);

    ASSERT_EQ_INT(records, 1);
    ASSERT_EQ_INT(abs_buf[1], 1);
    ASSERT_EQ_INT(abs_len,
                  PLAYER_MOTION_Q_MSG_HEADER + PLAYER_MOTION_Q_RECORD_SIZE);
    ASSERT_EQ_INT(delta_buf[1], 0);
    ASSERT_EQ_INT(posed_buf[1], 0);
    ASSERT_EQ_INT(delta_len, PLAYER_MOTIOND_Q_MSG_HEADER);
}

TEST(test_player_dock_stream_excludes_recipient_and_updates_status_flags) {
    SERVER_PLAYER_ARRAY(players, MAX_PLAYERS);

    players[0].connected = true;
    players[0].session_ready = true;
    players[0].docked = false;

    players[1].connected = true;
    players[1].session_ready = true;
    players[1].docked = true;
    players[1].actual_thrusting = true;

    players[3].connected = true;
    players[3].session_ready = true;
    players[3].docked = false;
    players[3].actual_thrusting = true;

    uint8_t buf[PLAYER_DOCK_MSG_HEADER +
                MAX_PLAYERS * PLAYER_DOCK_RECORD_SIZE];
    int len = serialize_player_dock_status_for_recipient(buf, players, 0);

    ASSERT_EQ_INT(buf[0], NET_MSG_WORLD_PLAYER_DOCK_Q);
    ASSERT_EQ_INT(buf[1], 2);
    ASSERT_EQ_INT(len,
                  PLAYER_DOCK_MSG_HEADER + 2 * PLAYER_DOCK_RECORD_SIZE);
    const uint8_t *p0 = &buf[PLAYER_DOCK_MSG_HEADER];
    const uint8_t *p1 = &buf[PLAYER_DOCK_MSG_HEADER + PLAYER_DOCK_RECORD_SIZE];
    ASSERT_EQ_INT(p0[0], 1);
    ASSERT_EQ_INT(p0[1], 5);
    ASSERT_EQ_INT(p1[0], 3);
    ASSERT_EQ_INT(p1[1], 1);
}

TEST(test_world_players_semantic_hash_ignores_pose_and_input_ack_tail) {
    ASSERT_EQ_INT((int)WORLD_PLAYERS_SEMANTIC_HEARTBEAT_MS, 16000);
    ASSERT(world_players_semantic_heartbeat_due(0ull, 10ull));
    ASSERT(!world_players_semantic_heartbeat_due(
        1000ull, 1000ull + WORLD_PLAYERS_SEMANTIC_HEARTBEAT_MS - 1ull));
    ASSERT(world_players_semantic_heartbeat_due(
        1000ull, 1000ull + WORLD_PLAYERS_SEMANTIC_HEARTBEAT_MS));

    SERVER_PLAYER_ARRAY(players, MAX_PLAYERS);
    players[0].connected = true;
    players[0].ship->pos = v2(10.0f, 20.0f);
    players[0].ship->vel = v2(1.0f, 2.0f);
    players[0].ship->angle = 0.5f;
    players[0].last_input_seq = 7;
    players[0].last_input_tick = 111u;

    uint8_t a[2 + MAX_PLAYERS * PLAYER_RECORD_SIZE];
    uint8_t b[2 + MAX_PLAYERS * PLAYER_RECORD_SIZE];
    int alen = serialize_all_player_states(a, players, 1000u);
    int blen = serialize_all_player_states(b, players, 1001u);

    ASSERT_EQ_INT(alen, blen);
    ASSERT_EQ_INT((int)read_u32_le(&a[2 + 69]), 1000);
    ASSERT_EQ_INT((int)read_u32_le(&b[2 + 69]), 1001);
    uint64_t ahash = net_world_players_semantic_hash(a, alen);
    uint64_t bhash = net_world_players_semantic_hash(b, blen);
    ASSERT(ahash == bhash);

    players[0].last_input_seq = 8;
    blen = serialize_all_player_states(b, players, 1001u);
    bhash = net_world_players_semantic_hash(b, blen);
    ASSERT(ahash == bhash);

    players[0].last_input_seq = 7;
    players[0].last_input_tick = 112u;
    blen = serialize_all_player_states(b, players, 1001u);
    bhash = net_world_players_semantic_hash(b, blen);
    ASSERT(ahash == bhash);

    players[0].last_input_tick = 111u;
    players[0].ship->pos.x += 1.0f;
    blen = serialize_all_player_states(b, players, 1001u);
    bhash = net_world_players_semantic_hash(b, blen);
    ASSERT(ahash == bhash);

    players[0].actual_thrusting = true;
    blen = serialize_all_player_states(b, players, 1001u);
    bhash = net_world_players_semantic_hash(b, blen);
    ASSERT(ahash == bhash);

    players[0].actual_thrusting = false;
    players[0].ship->pos.x = 10.0f;
    players[0].docked = true;
    alen = serialize_all_player_states(a, players, 1000u);
    ahash = net_world_players_semantic_hash(a, alen);

    players[0].ship->pos.x += 100.0f;
    players[0].ship->pos.y -= 25.0f;
    players[0].ship->vel = v2(3.0f, -4.0f);
    players[0].ship->angle += 0.75f;
    blen = serialize_all_player_states(b, players, 1001u);
    bhash = net_world_players_semantic_hash(b, blen);
    ASSERT(ahash == bhash);

    players[0].docked = false;
    blen = serialize_all_player_states(b, players, 1001u);
    bhash = net_world_players_semantic_hash(b, blen);
    ASSERT(ahash == bhash);

    players[0].beam_active = true;
    blen = serialize_all_player_states(b, players, 1001u);
    bhash = net_world_players_semantic_hash(b, blen);
    ASSERT(ahash != bhash);
}

TEST(test_roundtrip_asteroids) {
    asteroid_t asteroids[MAX_ASTEROIDS];
    memset(asteroids, 0, sizeof(asteroids));

    /* Set up 3 active asteroids with different properties */
    asteroids[0].active = true;
    asteroids[0].net_dirty = true;
    asteroids[0].fracture_child = false;
    asteroids[0].tier = ASTEROID_TIER_XL;
    asteroids[0].commodity = COMMODITY_FERRITE_ORE;
    asteroids[0].pos = v2(500.0f, -300.0f);
    asteroids[0].vel = v2(1.0f, -1.0f);
    asteroids[0].hp = 150.0f;
    asteroids[0].ore = 0.0f;
    asteroids[0].radius = 65.0f;

    asteroids[5].active = true;
    asteroids[5].net_dirty = true;
    asteroids[5].fracture_child = true;
    asteroids[5].tier = ASTEROID_TIER_S;
    asteroids[5].commodity = COMMODITY_CRYSTAL_ORE;
    asteroids[5].pos = v2(-100.0f, 200.0f);
    asteroids[5].vel = v2(-3.0f, 0.5f);
    asteroids[5].hp = 12.0f;
    asteroids[5].ore = 10.5f;
    asteroids[5].radius = 14.0f;
    asteroids[5].crystal_stage = CRYSTAL_STAGE_INTERMEDIATE;
    asteroids[5].phase = ASTEROID_PHASE_GAS_RICH;

    uint8_t buf[ASTEROID_MSG_HEADER + MAX_ASTEROIDS * ASTEROID_RECORD_SIZE];
    bool sent[MAX_ASTEROIDS] = {0};
    vec2 view_pos = v2(0.0f, 0.0f); /* both asteroids are within 3000u */
    int len = serialize_asteroids_for_player(buf, asteroids, view_pos, sent);

    ASSERT_EQ_INT(buf[0], NET_MSG_WORLD_ASTEROIDS);
    ASSERT_EQ_INT(buf[1] | (buf[2] << 8), 2);  /* 2 visible asteroids (uint16 count) */
    ASSERT_EQ_INT(len, ASTEROID_MSG_HEADER + 2 * ASTEROID_RECORD_SIZE);

    /* First asteroid (index 0) */
    uint8_t *p0 = &buf[ASTEROID_MSG_HEADER];
    ASSERT_EQ_INT(p0[0] | (p0[1] << 8), 0);  /* uint16 index */
    ASSERT(p0[2] & 1);         /* active */
    ASSERT(!(p0[2] & 2));      /* not fracture_child */
    ASSERT_EQ_INT((p0[2] >> 2) & 0x7, ASTEROID_TIER_XL);
    ASSERT_EQ_INT((p0[2] >> 5) & 0x7, COMMODITY_FERRITE_ORE);
    ASSERT_EQ_FLOAT(read_f32_le(&p0[3]), 500.0f, 0.1f);   /* pos.x */
    ASSERT_EQ_FLOAT(read_f32_le(&p0[19]), 150.0f, 0.1f);  /* hp */

    /* Second asteroid (index 5) */
    uint8_t *p1 = &buf[ASTEROID_MSG_HEADER + ASTEROID_RECORD_SIZE];
    ASSERT_EQ_INT(p1[0] | (p1[1] << 8), 5);  /* uint16 index */
    ASSERT(p1[2] & 1);         /* active */
    ASSERT(p1[2] & 2);         /* fracture_child */
    ASSERT_EQ_INT((p1[2] >> 2) & 0x7, ASTEROID_TIER_S);
    ASSERT_EQ_INT((p1[2] >> 5) & 0x7, COMMODITY_CRYSTAL_ORE);
    ASSERT_EQ_FLOAT(read_f32_le(&p1[23]), 10.5f, 0.1f);  /* ore */
    ASSERT_EQ_FLOAT(read_f32_le(&p1[27]), 14.0f, 0.1f);  /* radius */
    ASSERT_EQ_INT(p1[33], CRYSTAL_STAGE_INTERMEDIATE);
    ASSERT_EQ_INT(p1[34], ASTEROID_PHASE_GAS_RICH);
}

TEST(test_asteroid_identity_budget_trickles_background_first_visible) {
    ASSERT_EQ_INT(asteroid_net_background_identity_budget_at_tick(0u), 8);
    ASSERT_EQ_INT(asteroid_net_background_identity_budget_at_tick(1u), 0);
    ASSERT_EQ_INT(asteroid_net_background_identity_budget_at_tick(2u), 0);
    ASSERT_EQ_INT(asteroid_net_background_identity_budget_at_tick(3u), 0);
    ASSERT_EQ_INT(asteroid_net_background_identity_budget_at_tick(4u), 8);
    ASSERT_EQ_INT(
        asteroid_net_background_identity_budget_at_tick_for_players(4u, 1), 8);
    ASSERT_EQ_INT(
        asteroid_net_background_identity_budget_at_tick_for_players(4u, 4), 8);
    ASSERT_EQ_INT(
        asteroid_net_background_identity_budget_at_tick_for_players(4u, 5), 4);
    ASSERT_EQ_INT(
        asteroid_net_background_identity_budget_at_tick_for_players(4u, 9), 2);
    ASSERT_EQ_INT(
        asteroid_net_background_identity_budget_at_tick_for_players(4u, 17), 1);
    ASSERT_EQ_INT(
        asteroid_net_background_identity_budget_at_tick_for_players(5u, 17), 0);

    asteroid_t asteroids[MAX_ASTEROIDS];
    memset(asteroids, 0, sizeof(asteroids));
    bool sent[MAX_ASTEROIDS] = {0};

    asteroids[2].active = true;
    asteroids[2].pos = v2(1000.0f, 0.0f);
    asteroids[2].radius = 16.0f;
    asteroids[3].active = true;
    asteroids[3].pos = v2(2100.0f, 0.0f);
    asteroids[3].radius = 16.0f;
    asteroids[4].active = true;
    asteroids[4].pos = v2(2200.0f, 0.0f);
    asteroids[4].radius = 16.0f;

    uint8_t buf[ASTEROID_MSG_HEADER + MAX_ASTEROIDS * ASTEROID_RECORD_SIZE];
    uint8_t asteroids_q[ASTEROID_Q_MSG_HEADER +
                        MAX_ASTEROIDS * ASTEROID_Q_RECORD_SIZE];
    uint8_t asteroids8_q[ASTEROID8_Q_MSG_HEADER +
                         256 * ASTEROID8_Q_RECORD_SIZE];
    int asteroids_q_len = 0;
    int asteroids8_q_len = 0;

    int len = serialize_asteroids_for_player_split_ext_state_budget_at_tick(
        buf, asteroids_q, &asteroids_q_len, asteroids8_q, &asteroids8_q_len,
        NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
        NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
        asteroids, v2(0.0f, 0.0f), sent, NULL, NULL, NULL,
        NULL, NULL, NULL, NULL, 10u, 1);

    ASSERT_EQ_INT(len, ASTEROID_MSG_HEADER);
    ASSERT_EQ_INT(asteroids8_q[0], NET_MSG_WORLD_ASTEROIDS8_Q);
    ASSERT_EQ_INT(asteroids8_q[1], 2);
    ASSERT(sent[2]);
    ASSERT(sent[3]);
    ASSERT(!sent[4]);
    ASSERT_EQ_INT(asteroids8_q_len,
                  ASTEROID8_Q_MSG_HEADER + 2 * ASTEROID8_Q_RECORD_SIZE);

    asteroids_q_len = 0;
    asteroids8_q_len = 0;
    len = serialize_asteroids_for_player_split_ext_state_budget_at_tick(
        buf, asteroids_q, &asteroids_q_len, asteroids8_q, &asteroids8_q_len,
        NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
        NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
        asteroids, v2(0.0f, 0.0f), sent, NULL, NULL, NULL,
        NULL, NULL, NULL, NULL, 11u, 1);

    ASSERT_EQ_INT(len, ASTEROID_MSG_HEADER);
    ASSERT_EQ_INT(asteroids8_q[1], 1);
    ASSERT(sent[4]);
    ASSERT_EQ_INT(asteroids8_q_len,
                  ASTEROID8_Q_MSG_HEADER + ASTEROID8_Q_RECORD_SIZE);
}

TEST(test_asteroid_delta_suppresses_clean_static_repeat) {
    asteroid_t asteroids[MAX_ASTEROIDS];
    memset(asteroids, 0, sizeof(asteroids));
    bool sent[MAX_ASTEROIDS] = {0};

    asteroids[3].active = true;
    asteroids[3].pos = v2(20.0f, 30.0f);
    asteroids[3].radius = 18.0f;
    asteroids[3].hp = 50.0f;
    asteroids[3].vel = v2(0.0f, 0.0f);
    asteroids[3].net_dirty = false;
    sent[3] = true;

    uint8_t buf[ASTEROID_MSG_HEADER + MAX_ASTEROIDS * ASTEROID_RECORD_SIZE];
    int len = serialize_asteroids_for_player(buf, asteroids, v2(0.0f, 0.0f), sent);

    ASSERT_EQ_INT(buf[0], NET_MSG_WORLD_ASTEROIDS);
    ASSERT_EQ_INT(buf[1] | (buf[2] << 8), 0);
    ASSERT_EQ_INT(len, ASTEROID_MSG_HEADER);
    ASSERT(sent[3]);
}

TEST(test_asteroid_delta_sends_dirty_or_moving_repeat) {
    asteroid_t asteroids[MAX_ASTEROIDS];
    memset(asteroids, 0, sizeof(asteroids));
    bool sent[MAX_ASTEROIDS] = {0};

    asteroids[4].active = true;
    asteroids[4].pos = v2(10.0f, 0.0f);
    asteroids[4].radius = 12.0f;
    asteroids[4].hp = 20.0f;
    asteroids[4].net_dirty = true;
    sent[4] = true;

    asteroids[6].active = true;
    asteroids[6].pos = v2(30.0f, 0.0f);
    asteroids[6].radius = 10.0f;
    asteroids[6].hp = 10.0f;
    asteroids[6].vel = v2(0.25f, 0.0f);
    sent[6] = true;

    uint8_t buf[ASTEROID_MSG_HEADER + MAX_ASTEROIDS * ASTEROID_RECORD_SIZE];
    int len = serialize_asteroids_for_player(buf, asteroids, v2(0.0f, 0.0f), sent);

    ASSERT_EQ_INT(buf[0], NET_MSG_WORLD_ASTEROIDS);
    ASSERT_EQ_INT(buf[1] | (buf[2] << 8), 2);
    ASSERT_EQ_INT(len, ASTEROID_MSG_HEADER + 2 * ASTEROID_RECORD_SIZE);
    ASSERT_EQ_INT(buf[ASTEROID_MSG_HEADER] |
                  (buf[ASTEROID_MSG_HEADER + 1] << 8), 4);
    ASSERT_EQ_INT(buf[ASTEROID_MSG_HEADER + ASTEROID_RECORD_SIZE] |
                  (buf[ASTEROID_MSG_HEADER + ASTEROID_RECORD_SIZE + 1] << 8), 6);
}

TEST(test_asteroid_delta_throttles_clean_moving_repeat_by_tick) {
    ASSERT_EQ_INT((int)ASTEROID_NET_MOVING_REPEAT_TICKS, 36);

    asteroid_t asteroids[MAX_ASTEROIDS];
    memset(asteroids, 0, sizeof(asteroids));
    bool sent[MAX_ASTEROIDS] = {0};
    uint32_t motion_sent_tick[MAX_ASTEROIDS] = {0};
    vec2 motion_sent_pos[MAX_ASTEROIDS] = {0};
    vec2 motion_sent_vel[MAX_ASTEROIDS] = {0};

    asteroids[6].active = true;
    asteroids[6].pos = v2(30.0f, 0.0f);
    asteroids[6].radius = 10.0f;
    asteroids[6].hp = 10.0f;
    asteroids[6].vel = v2(12.0f, 0.0f);
    sent[6] = true;
    motion_sent_tick[6] = 100u;
    motion_sent_pos[6] = v2(30.0f, 0.0f);
    motion_sent_vel[6] = asteroids[6].vel;

    uint8_t buf[ASTEROID_MSG_HEADER + MAX_ASTEROIDS * ASTEROID_RECORD_SIZE];
    int len = serialize_asteroids_for_player_at_tick(
        buf, asteroids, v2(0.0f, 0.0f), sent, motion_sent_tick,
        motion_sent_pos, motion_sent_vel, 110u);

    ASSERT_EQ_INT(buf[0], NET_MSG_WORLD_ASTEROIDS);
    ASSERT_EQ_INT(buf[1] | (buf[2] << 8), 0);
    ASSERT_EQ_INT(len, ASTEROID_MSG_HEADER);
    ASSERT_EQ_INT((int)motion_sent_tick[6], 100);

    len = serialize_asteroids_for_player_at_tick(
        buf, asteroids, v2(0.0f, 0.0f), sent, motion_sent_tick,
        motion_sent_pos, motion_sent_vel, 124u);

    ASSERT_EQ_INT(buf[1] | (buf[2] << 8), 0);
    ASSERT_EQ_INT(len, ASTEROID_MSG_HEADER);
    ASSERT_EQ_INT((int)motion_sent_tick[6], 100);

    asteroids[6].pos = v2(55.0f, 0.0f);
    len = serialize_asteroids_for_player_at_tick(
        buf, asteroids, v2(0.0f, 0.0f), sent, motion_sent_tick,
        motion_sent_pos, motion_sent_vel,
        100u + ASTEROID_NET_MOVING_REPEAT_TICKS);

    ASSERT_EQ_INT(buf[1] | (buf[2] << 8), 1);
    ASSERT_EQ_INT(len, ASTEROID_MSG_HEADER + ASTEROID_RECORD_SIZE);
    ASSERT_EQ_INT(buf[ASTEROID_MSG_HEADER] |
                  (buf[ASTEROID_MSG_HEADER + 1] << 8), 6);
    ASSERT_EQ_INT((int)motion_sent_tick[6],
                  100 + ASTEROID_NET_MOVING_REPEAT_TICKS);

    asteroids[6].vel = v2(0.0f, 0.0f);
    len = serialize_asteroids_for_player_at_tick(
        buf, asteroids, v2(0.0f, 0.0f), sent, motion_sent_tick,
        motion_sent_pos, motion_sent_vel,
        101u + ASTEROID_NET_MOVING_REPEAT_TICKS);

    ASSERT_EQ_INT(buf[1] | (buf[2] << 8), 1);
    ASSERT_EQ_INT(buf[ASTEROID_MSG_HEADER] |
                  (buf[ASTEROID_MSG_HEADER + 1] << 8), 6);
    ASSERT_EQ_INT((int)motion_sent_tick[6], 0);

    len = serialize_asteroids_for_player_at_tick(
        buf, asteroids, v2(0.0f, 0.0f), sent, motion_sent_tick,
        motion_sent_pos, motion_sent_vel, 126u);
    ASSERT_EQ_INT(buf[1] | (buf[2] << 8), 0);
    ASSERT_EQ_INT(len, ASTEROID_MSG_HEADER);

    asteroids[6].net_dirty = true;
    len = serialize_asteroids_for_player_at_tick(
        buf, asteroids, v2(0.0f, 0.0f), sent, motion_sent_tick,
        motion_sent_pos, motion_sent_vel, 127u);

    ASSERT_EQ_INT(buf[1] | (buf[2] << 8), 1);
    ASSERT_EQ_INT(buf[ASTEROID_MSG_HEADER] |
                  (buf[ASTEROID_MSG_HEADER + 1] << 8), 6);
}

TEST(test_asteroid_delta_towed_fragments_use_tighter_motion_gate) {
    ASSERT_EQ_INT((int)ASTEROID_NET_TOWED_MOVING_REPEAT_TICKS, 12);
    ASSERT_EQ_FLOAT(ASTEROID_NET_TOWED_PREDICT_ERROR_SQ, 25.0f, 0.001f);

    asteroid_t asteroids[MAX_ASTEROIDS];
    memset(asteroids, 0, sizeof(asteroids));
    bool sent[MAX_ASTEROIDS] = {0};
    uint32_t motion_sent_tick[MAX_ASTEROIDS] = {0};
    vec2 motion_sent_pos[MAX_ASTEROIDS] = {0};
    vec2 motion_sent_vel[MAX_ASTEROIDS] = {0};

    asteroids[6].active = true;
    asteroids[6].tier = ASTEROID_TIER_S;
    asteroids[6].fracture_child = true;
    asteroid_set_player_tractor(&asteroids[6], 0);
    asteroids[6].pos = v2(6.0f, 0.0f);
    asteroids[6].radius = 10.0f;
    asteroids[6].hp = 10.0f;
    asteroids[6].vel = v2(12.0f, 0.0f);
    sent[6] = true;
    motion_sent_tick[6] = 100u;
    motion_sent_pos[6] = v2(0.0f, 0.0f);
    motion_sent_vel[6] = v2(0.0f, 0.0f);

    uint8_t full[ASTEROID_MSG_HEADER + MAX_ASTEROIDS * ASTEROID_RECORD_SIZE];
    uint8_t motion[ASTEROID_MOTION_MSG_HEADER +
                   MAX_ASTEROIDS * ASTEROID_MOTION_RECORD_SIZE];
    uint8_t motion_q[ASTEROID_MOTION_Q_MSG_HEADER +
                     MAX_ASTEROIDS * ASTEROID_MOTION_Q_RECORD_SIZE];
    int motion_len = 0;
    int motion_q_len = 0;
    int full_len = serialize_asteroids_for_player_split_ext_at_tick(
        full, motion, &motion_len, motion_q, &motion_q_len,
        NULL, NULL, NULL, NULL, NULL, NULL,
        asteroids, v2(0.0f, 0.0f), sent,
        motion_sent_tick, motion_sent_pos, motion_sent_vel, 112u);

    ASSERT_EQ_INT(full_len, ASTEROID_MSG_HEADER);
    ASSERT_EQ_INT(motion_len, ASTEROID_MOTION_MSG_HEADER);
    ASSERT_EQ_INT(motion_q[0], NET_MSG_WORLD_ASTEROID_MOTION_Q);
    ASSERT_EQ_INT(motion_q[1] | (motion_q[2] << 8), 1);
    ASSERT_EQ_INT(motion_q_len,
                  ASTEROID_MOTION_Q_MSG_HEADER + ASTEROID_MOTION_Q_RECORD_SIZE);
    ASSERT_EQ_INT((int)motion_sent_tick[6], 112);
}

TEST(test_asteroid_delta_throttles_far_slow_moving_repeat) {
    ASSERT_EQ_INT((int)ASTEROID_NET_FAR_SLOW_MOVING_REPEAT_TICKS, 240);
    ASSERT_EQ_INT((int)ASTEROID_NET_FAR_SLOW_MOTION_HEARTBEAT_TICKS, 1200);
    ASSERT_EQ_FLOAT(ASTEROID_NET_FAR_SLOW_PREDICT_ERROR_SQ,
                    80.0f * 80.0f, 0.001f);

    asteroid_t asteroids[MAX_ASTEROIDS];
    memset(asteroids, 0, sizeof(asteroids));
    bool sent[MAX_ASTEROIDS] = {0};
    uint32_t motion_sent_tick[MAX_ASTEROIDS] = {0};
    vec2 motion_sent_pos[MAX_ASTEROIDS] = {0};
    vec2 motion_sent_vel[MAX_ASTEROIDS] = {0};

    asteroids[9].active = true;
    asteroids[9].pos = v2(1600.0f, 0.0f);
    asteroids[9].radius = 11.0f;
    asteroids[9].hp = 10.0f;
    asteroids[9].vel = v2(2.0f, 0.0f);
    sent[9] = true;
    motion_sent_tick[9] = 100u;
    motion_sent_pos[9] = asteroids[9].pos;
    motion_sent_vel[9] = asteroids[9].vel;

    uint8_t buf[ASTEROID_MSG_HEADER + MAX_ASTEROIDS * ASTEROID_RECORD_SIZE];
    int len = serialize_asteroids_for_player_at_tick(
        buf, asteroids, v2(0.0f, 0.0f), sent, motion_sent_tick,
        motion_sent_pos, motion_sent_vel,
        100u + ASTEROID_NET_MOVING_REPEAT_TICKS);

    ASSERT_EQ_INT(buf[0], NET_MSG_WORLD_ASTEROIDS);
    ASSERT_EQ_INT(buf[1] | (buf[2] << 8), 0);
    ASSERT_EQ_INT(len, ASTEROID_MSG_HEADER);
    ASSERT_EQ_INT((int)motion_sent_tick[9], 100);

    len = serialize_asteroids_for_player_at_tick(
        buf, asteroids, v2(0.0f, 0.0f), sent, motion_sent_tick,
        motion_sent_pos, motion_sent_vel,
        100u + ASTEROID_NET_FAR_MOVING_REPEAT_TICKS);

    ASSERT_EQ_INT(buf[1] | (buf[2] << 8), 0);
    ASSERT_EQ_INT(len, ASTEROID_MSG_HEADER);
    ASSERT_EQ_INT((int)motion_sent_tick[9], 100);

    asteroids[9].pos = v2(1602.0f, 0.0f);
    len = serialize_asteroids_for_player_at_tick(
        buf, asteroids, v2(0.0f, 0.0f), sent, motion_sent_tick,
        motion_sent_pos, motion_sent_vel,
        100u + ASTEROID_NET_FAR_SLOW_MOVING_REPEAT_TICKS);

    ASSERT_EQ_INT(buf[1] | (buf[2] << 8), 0);
    ASSERT_EQ_INT(len, ASTEROID_MSG_HEADER);
    ASSERT_EQ_INT((int)motion_sent_tick[9], 100);

    asteroids[9].pos = v2(1600.5f, 0.0f);
    len = serialize_asteroids_for_player_at_tick(
        buf, asteroids, v2(0.0f, 0.0f), sent, motion_sent_tick,
        motion_sent_pos, motion_sent_vel,
        100u + ASTEROID_NET_FAR_SLOW_MOTION_HEARTBEAT_TICKS);

    ASSERT_EQ_INT(buf[1] | (buf[2] << 8), 1);
    ASSERT_EQ_INT(len, ASTEROID_MSG_HEADER + ASTEROID_RECORD_SIZE);
    ASSERT_EQ_INT(buf[ASTEROID_MSG_HEADER] |
                  (buf[ASTEROID_MSG_HEADER + 1] << 8), 9);
    ASSERT_EQ_INT((int)motion_sent_tick[9],
                  100 + ASTEROID_NET_FAR_SLOW_MOTION_HEARTBEAT_TICKS);
}

TEST(test_asteroid_delta_relaxes_outer_near_slow_motion) {
    ASSERT_EQ_FLOAT(ASTEROID_NET_INTERACTION_RADIUS_SQ,
                    600.0f * 600.0f, 0.001f);
    ASSERT_EQ_INT((int)asteroid_net_moving_repeat_ticks(
                      900.0f * 900.0f, 2.0f * 2.0f),
                  ASTEROID_NET_FAR_SLOW_MOVING_REPEAT_TICKS);
    ASSERT_EQ_INT((int)asteroid_net_moving_repeat_ticks(
                      900.0f * 900.0f, 40.0f * 40.0f),
                  ASTEROID_NET_MOVING_REPEAT_TICKS);

    asteroid_t asteroids[MAX_ASTEROIDS];
    memset(asteroids, 0, sizeof(asteroids));
    bool sent[MAX_ASTEROIDS] = {0};
    uint32_t motion_sent_tick[MAX_ASTEROIDS] = {0};
    vec2 motion_sent_pos[MAX_ASTEROIDS] = {0};
    vec2 motion_sent_vel[MAX_ASTEROIDS] = {0};

    asteroids[9].active = true;
    asteroids[9].pos = v2(900.0f, 0.0f);
    asteroids[9].radius = 11.0f;
    asteroids[9].hp = 10.0f;
    asteroids[9].vel = v2(2.0f, 0.0f);
    sent[9] = true;
    motion_sent_tick[9] = 100u;
    motion_sent_pos[9] = asteroids[9].pos;
    motion_sent_vel[9] = asteroids[9].vel;

    uint8_t buf[ASTEROID_MSG_HEADER + MAX_ASTEROIDS * ASTEROID_RECORD_SIZE];
    asteroids[9].pos = v2(950.0f, 0.0f);
    int len = serialize_asteroids_for_player_at_tick(
        buf, asteroids, v2(0.0f, 0.0f), sent, motion_sent_tick,
        motion_sent_pos, motion_sent_vel,
        100u + ASTEROID_NET_MOVING_REPEAT_TICKS);

    ASSERT_EQ_INT(buf[0], NET_MSG_WORLD_ASTEROIDS);
    ASSERT_EQ_INT(buf[1] | (buf[2] << 8), 0);
    ASSERT_EQ_INT(len, ASTEROID_MSG_HEADER);
    ASSERT_EQ_INT((int)motion_sent_tick[9], 100);

    asteroids[9].pos = v2(902.0f, 0.0f);
    len = serialize_asteroids_for_player_at_tick(
        buf, asteroids, v2(0.0f, 0.0f), sent, motion_sent_tick,
        motion_sent_pos, motion_sent_vel,
        100u + ASTEROID_NET_FAR_SLOW_MOVING_REPEAT_TICKS);

    ASSERT_EQ_INT(buf[1] | (buf[2] << 8), 0);
    ASSERT_EQ_INT(len, ASTEROID_MSG_HEADER);
    ASSERT_EQ_INT((int)motion_sent_tick[9], 100);

    asteroids[9].pos = v2(900.5f, 0.0f);
    len = serialize_asteroids_for_player_at_tick(
        buf, asteroids, v2(0.0f, 0.0f), sent, motion_sent_tick,
        motion_sent_pos, motion_sent_vel,
        100u + ASTEROID_NET_FAR_SLOW_MOTION_HEARTBEAT_TICKS);

    ASSERT_EQ_INT(buf[1] | (buf[2] << 8), 1);
    ASSERT_EQ_INT(len, ASTEROID_MSG_HEADER + ASTEROID_RECORD_SIZE);
    ASSERT_EQ_INT((int)motion_sent_tick[9],
                  100 + ASTEROID_NET_FAR_SLOW_MOTION_HEARTBEAT_TICKS);
}

TEST(test_asteroid_delta_keeps_old_far_cadence_quiet) {
    asteroid_t asteroids[MAX_ASTEROIDS];
    memset(asteroids, 0, sizeof(asteroids));
    bool sent[MAX_ASTEROIDS] = {0};
    uint32_t motion_sent_tick[MAX_ASTEROIDS] = {0};
    vec2 motion_sent_pos[MAX_ASTEROIDS] = {0};
    vec2 motion_sent_vel[MAX_ASTEROIDS] = {0};

    asteroids[9].active = true;
    asteroids[9].pos = v2(1600.0f, 0.0f);
    asteroids[9].radius = 11.0f;
    asteroids[9].hp = 10.0f;
    asteroids[9].vel = v2(0.25f, 0.0f);
    sent[9] = true;
    motion_sent_tick[9] = 100u;
    motion_sent_pos[9] = v2(1500.0f, 0.0f);
    motion_sent_vel[9] = asteroids[9].vel;

    uint8_t buf[ASTEROID_MSG_HEADER + MAX_ASTEROIDS * ASTEROID_RECORD_SIZE];
    int len = serialize_asteroids_for_player_at_tick(
        buf, asteroids, v2(0.0f, 0.0f), sent, motion_sent_tick,
        motion_sent_pos, motion_sent_vel, 190u);

    ASSERT_EQ_INT(buf[0], NET_MSG_WORLD_ASTEROIDS);
    ASSERT_EQ_INT(buf[1] | (buf[2] << 8), 0);
    ASSERT_EQ_INT(len, ASTEROID_MSG_HEADER);
    ASSERT_EQ_INT((int)motion_sent_tick[9], 100);
}

TEST(test_asteroid_delta_keeps_old_near_heartbeat_quiet) {
    asteroid_t asteroids[MAX_ASTEROIDS];
    memset(asteroids, 0, sizeof(asteroids));
    bool sent[MAX_ASTEROIDS] = {0};
    uint32_t motion_sent_tick[MAX_ASTEROIDS] = {0};
    vec2 motion_sent_pos[MAX_ASTEROIDS] = {0};
    vec2 motion_sent_vel[MAX_ASTEROIDS] = {0};

    asteroids[7].active = true;
    asteroids[7].pos = v2(100.0f, 0.0f);
    asteroids[7].radius = 11.0f;
    asteroids[7].hp = 10.0f;
    asteroids[7].vel = v2(12.0f, 0.0f);
    sent[7] = true;
    motion_sent_tick[7] = 100u;
    motion_sent_pos[7] = asteroids[7].pos;
    motion_sent_vel[7] = asteroids[7].vel;

    uint8_t buf[ASTEROID_MSG_HEADER + MAX_ASTEROIDS * ASTEROID_RECORD_SIZE];
    asteroids[7].pos = v2(112.0f, 0.0f);
    int len = serialize_asteroids_for_player_at_tick(
        buf, asteroids, v2(0.0f, 0.0f), sent, motion_sent_tick,
        motion_sent_pos, motion_sent_vel, 220u);

    ASSERT_EQ_INT(buf[0], NET_MSG_WORLD_ASTEROIDS);
    ASSERT_EQ_INT(buf[1] | (buf[2] << 8), 0);
    ASSERT_EQ_INT(len, ASTEROID_MSG_HEADER);
    ASSERT_EQ_INT((int)motion_sent_tick[7], 100);

    asteroids[7].pos = v2(124.0f, 0.0f);
    len = serialize_asteroids_for_player_at_tick(
        buf, asteroids, v2(0.0f, 0.0f), sent, motion_sent_tick,
        motion_sent_pos, motion_sent_vel,
        100u + ASTEROID_NET_MOTION_HEARTBEAT_TICKS);

    ASSERT_EQ_INT(buf[1] | (buf[2] << 8), 1);
    ASSERT_EQ_INT(len, ASTEROID_MSG_HEADER + ASTEROID_RECORD_SIZE);
    ASSERT_EQ_INT((int)motion_sent_tick[7],
                  100 + ASTEROID_NET_MOTION_HEARTBEAT_TICKS);
}

TEST(test_asteroid_delta_relaxes_slow_near_safety_heartbeat) {
    ASSERT_EQ_INT((int)ASTEROID_NET_SLOW_MOTION_HEARTBEAT_TICKS, 720);

    asteroid_t asteroids[MAX_ASTEROIDS];
    memset(asteroids, 0, sizeof(asteroids));
    bool sent[MAX_ASTEROIDS] = {0};
    uint32_t motion_sent_tick[MAX_ASTEROIDS] = {0};
    vec2 motion_sent_pos[MAX_ASTEROIDS] = {0};
    vec2 motion_sent_vel[MAX_ASTEROIDS] = {0};

    asteroids[7].active = true;
    asteroids[7].pos = v2(100.0f, 0.0f);
    asteroids[7].radius = 11.0f;
    asteroids[7].hp = 10.0f;
    asteroids[7].vel = v2(2.0f, 0.0f);
    sent[7] = true;
    motion_sent_tick[7] = 100u;
    motion_sent_pos[7] = asteroids[7].pos;
    motion_sent_vel[7] = asteroids[7].vel;

    uint8_t buf[ASTEROID_MSG_HEADER + MAX_ASTEROIDS * ASTEROID_RECORD_SIZE];
    asteroids[7].pos = v2(104.0f, 0.0f);
    int len = serialize_asteroids_for_player_at_tick(
        buf, asteroids, v2(0.0f, 0.0f), sent, motion_sent_tick,
        motion_sent_pos, motion_sent_vel,
        100u + ASTEROID_NET_MOTION_HEARTBEAT_TICKS);

    ASSERT_EQ_INT(buf[0], NET_MSG_WORLD_ASTEROIDS);
    ASSERT_EQ_INT(buf[1] | (buf[2] << 8), 0);
    ASSERT_EQ_INT(len, ASTEROID_MSG_HEADER);
    ASSERT_EQ_INT((int)motion_sent_tick[7], 100);

    asteroids[7].pos = v2(108.0f, 0.0f);
    len = serialize_asteroids_for_player_at_tick(
        buf, asteroids, v2(0.0f, 0.0f), sent, motion_sent_tick,
        motion_sent_pos, motion_sent_vel,
        100u + ASTEROID_NET_SLOW_MOTION_HEARTBEAT_TICKS);

    ASSERT_EQ_INT(buf[1] | (buf[2] << 8), 1);
    ASSERT_EQ_INT(len, ASTEROID_MSG_HEADER + ASTEROID_RECORD_SIZE);
    ASSERT_EQ_INT((int)motion_sent_tick[7],
                  100 + ASTEROID_NET_SLOW_MOTION_HEARTBEAT_TICKS);
}

TEST(test_asteroid_delta_relaxes_crawl_safety_heartbeat) {
    ASSERT_EQ_INT((int)ASTEROID_NET_CRAWL_MOTION_HEARTBEAT_TICKS, 4800);

    asteroid_t asteroids[MAX_ASTEROIDS];
    memset(asteroids, 0, sizeof(asteroids));
    bool sent[MAX_ASTEROIDS] = {0};
    uint32_t motion_sent_tick[MAX_ASTEROIDS] = {0};
    vec2 motion_sent_pos[MAX_ASTEROIDS] = {0};
    vec2 motion_sent_vel[MAX_ASTEROIDS] = {0};

    asteroids[14].active = true;
    asteroids[14].pos = v2(100.0f, 0.0f);
    asteroids[14].radius = 11.0f;
    asteroids[14].hp = 10.0f;
    asteroids[14].vel = v2(0.5f, 0.0f);
    sent[14] = true;
    motion_sent_tick[14] = 100u;
    motion_sent_pos[14] = asteroids[14].pos;
    motion_sent_vel[14] = asteroids[14].vel;

    uint8_t buf[ASTEROID_MSG_HEADER + MAX_ASTEROIDS * ASTEROID_RECORD_SIZE];
    asteroids[14].pos = v2(102.0f, 0.0f);
    int len = serialize_asteroids_for_player_at_tick(
        buf, asteroids, v2(0.0f, 0.0f), sent, motion_sent_tick,
        motion_sent_pos, motion_sent_vel,
        100u + ASTEROID_NET_SLOW_MOTION_HEARTBEAT_TICKS);

    ASSERT_EQ_INT(buf[0], NET_MSG_WORLD_ASTEROIDS);
    ASSERT_EQ_INT(buf[1] | (buf[2] << 8), 0);
    ASSERT_EQ_INT(len, ASTEROID_MSG_HEADER);
    ASSERT_EQ_INT((int)motion_sent_tick[14], 100);

    asteroids[14].pos = v2(105.0f, 0.0f);
    len = serialize_asteroids_for_player_at_tick(
        buf, asteroids, v2(0.0f, 0.0f), sent, motion_sent_tick,
        motion_sent_pos, motion_sent_vel,
        100u + ASTEROID_NET_CRAWL_MOTION_HEARTBEAT_TICKS);

    ASSERT_EQ_INT(buf[1] | (buf[2] << 8), 1);
    ASSERT_EQ_INT(len, ASTEROID_MSG_HEADER + ASTEROID_RECORD_SIZE);
    ASSERT_EQ_INT((int)motion_sent_tick[14],
                  100 + ASTEROID_NET_CRAWL_MOTION_HEARTBEAT_TICKS);
}

TEST(test_asteroid_delta_sends_crawl_when_prediction_diverges) {
    asteroid_t asteroids[MAX_ASTEROIDS];
    memset(asteroids, 0, sizeof(asteroids));
    bool sent[MAX_ASTEROIDS] = {0};
    uint32_t motion_sent_tick[MAX_ASTEROIDS] = {0};
    vec2 motion_sent_pos[MAX_ASTEROIDS] = {0};
    vec2 motion_sent_vel[MAX_ASTEROIDS] = {0};

    asteroids[15].active = true;
    asteroids[15].pos = v2(100.0f, 0.0f);
    asteroids[15].radius = 11.0f;
    asteroids[15].hp = 10.0f;
    asteroids[15].vel = v2(0.5f, 0.0f);
    sent[15] = true;
    motion_sent_tick[15] = 100u;
    motion_sent_pos[15] = asteroids[15].pos;
    motion_sent_vel[15] = asteroids[15].vel;

    uint8_t buf[ASTEROID_MSG_HEADER + MAX_ASTEROIDS * ASTEROID_RECORD_SIZE];
    asteroids[15].pos = v2(170.0f, 0.0f);
    int len = serialize_asteroids_for_player_at_tick(
        buf, asteroids, v2(0.0f, 0.0f), sent, motion_sent_tick,
        motion_sent_pos, motion_sent_vel,
        100u + ASTEROID_NET_CRAWL_MOVING_REPEAT_TICKS);

    ASSERT_EQ_INT(buf[0], NET_MSG_WORLD_ASTEROIDS);
    ASSERT_EQ_INT(buf[1] | (buf[2] << 8), 1);
    ASSERT_EQ_INT(len, ASTEROID_MSG_HEADER + ASTEROID_RECORD_SIZE);
    ASSERT_EQ_INT((int)motion_sent_tick[15],
                  100 + ASTEROID_NET_CRAWL_MOVING_REPEAT_TICKS);
}

TEST(test_asteroid_delta_keeps_old_very_far_cadence_quiet) {
    ASSERT_EQ_INT((int)ASTEROID_NET_VERY_FAR_SLOW_MOVING_REPEAT_TICKS, 1200);

    asteroid_t asteroids[MAX_ASTEROIDS];
    memset(asteroids, 0, sizeof(asteroids));
    bool sent[MAX_ASTEROIDS] = {0};
    uint32_t motion_sent_tick[MAX_ASTEROIDS] = {0};
    vec2 motion_sent_pos[MAX_ASTEROIDS] = {0};
    vec2 motion_sent_vel[MAX_ASTEROIDS] = {0};

    asteroids[12].active = true;
    asteroids[12].pos = v2(2300.0f, 0.0f);
    asteroids[12].radius = 12.0f;
    asteroids[12].hp = 10.0f;
    asteroids[12].vel = v2(0.25f, 0.0f);
    sent[12] = true;
    motion_sent_tick[12] = 100u;
    motion_sent_pos[12] = asteroids[12].pos;
    motion_sent_vel[12] = asteroids[12].vel;

    uint8_t buf[ASTEROID_MSG_HEADER + MAX_ASTEROIDS * ASTEROID_RECORD_SIZE];
    int len = serialize_asteroids_for_player_at_tick(
        buf, asteroids, v2(0.0f, 0.0f), sent, motion_sent_tick,
        motion_sent_pos, motion_sent_vel,
        100u + ASTEROID_NET_VERY_FAR_MOVING_REPEAT_TICKS);

    ASSERT_EQ_INT(buf[0], NET_MSG_WORLD_ASTEROIDS);
    ASSERT_EQ_INT(buf[1] | (buf[2] << 8), 0);
    ASSERT_EQ_INT(len, ASTEROID_MSG_HEADER);
    ASSERT_EQ_INT((int)motion_sent_tick[12], 100);

    asteroids[12].pos = v2(2301.0f, 0.0f);
    len = serialize_asteroids_for_player_at_tick(
        buf, asteroids, v2(0.0f, 0.0f), sent, motion_sent_tick,
        motion_sent_pos, motion_sent_vel, 700u);

    ASSERT_EQ_INT(buf[1] | (buf[2] << 8), 0);
    ASSERT_EQ_INT(len, ASTEROID_MSG_HEADER);
    ASSERT_EQ_INT((int)motion_sent_tick[12], 100);

    asteroids[12].pos = v2(2301.0f, 0.0f);
    len = serialize_asteroids_for_player_at_tick(
        buf, asteroids, v2(0.0f, 0.0f), sent, motion_sent_tick,
        motion_sent_pos, motion_sent_vel,
        100u + ASTEROID_NET_VERY_FAR_SLOW_MOVING_REPEAT_TICKS);

    ASSERT_EQ_INT(buf[1] | (buf[2] << 8), 0);
    ASSERT_EQ_INT(len, ASTEROID_MSG_HEADER);
    ASSERT_EQ_INT((int)motion_sent_tick[12], 100);
}

TEST(test_asteroid_delta_relaxes_far_fast_moving_repeat) {
    ASSERT_EQ_INT((int)ASTEROID_NET_FAR_MOTION_HEARTBEAT_TICKS, 720);

    asteroid_t asteroids[MAX_ASTEROIDS];
    memset(asteroids, 0, sizeof(asteroids));
    bool sent[MAX_ASTEROIDS] = {0};
    uint32_t motion_sent_tick[MAX_ASTEROIDS] = {0};
    vec2 motion_sent_pos[MAX_ASTEROIDS] = {0};
    vec2 motion_sent_vel[MAX_ASTEROIDS] = {0};

    asteroids[10].active = true;
    asteroids[10].pos = v2(2200.0f, 0.0f);
    asteroids[10].radius = 12.0f;
    asteroids[10].hp = 10.0f;
    asteroids[10].vel = v2(80.0f, 0.0f);
    sent[10] = true;
    motion_sent_tick[10] = 100u;
    motion_sent_pos[10] = asteroids[10].pos;
    motion_sent_vel[10] = asteroids[10].vel;

    uint8_t buf[ASTEROID_MSG_HEADER + MAX_ASTEROIDS * ASTEROID_RECORD_SIZE];
    asteroids[10].pos = v2(2300.0f, 0.0f);
    int len = serialize_asteroids_for_player_at_tick(
        buf, asteroids, v2(0.0f, 0.0f), sent, motion_sent_tick,
        motion_sent_pos, motion_sent_vel,
        100u + ASTEROID_NET_MOVING_REPEAT_TICKS);

    ASSERT_EQ_INT(buf[0], NET_MSG_WORLD_ASTEROIDS);
    ASSERT_EQ_INT(buf[1] | (buf[2] << 8), 0);
    ASSERT_EQ_INT(len, ASTEROID_MSG_HEADER);
    ASSERT_EQ_INT((int)motion_sent_tick[10], 100);

    asteroids[10].pos = v2(2350.0f, 0.0f);
    len = serialize_asteroids_for_player_at_tick(
        buf, asteroids, v2(0.0f, 0.0f), sent, motion_sent_tick,
        motion_sent_pos, motion_sent_vel,
        100u + ASTEROID_NET_FAR_MOVING_REPEAT_TICKS);

    ASSERT_EQ_INT(buf[1] | (buf[2] << 8), 0);
    ASSERT_EQ_INT(len, ASTEROID_MSG_HEADER);
    ASSERT_EQ_INT((int)motion_sent_tick[10], 100);

    len = serialize_asteroids_for_player_at_tick(
        buf, asteroids, v2(0.0f, 0.0f), sent, motion_sent_tick,
        motion_sent_pos, motion_sent_vel,
        100u + ASTEROID_NET_VERY_FAR_MOVING_REPEAT_TICKS - 1u);

    ASSERT_EQ_INT(buf[1] | (buf[2] << 8), 0);
    ASSERT_EQ_INT(len, ASTEROID_MSG_HEADER);
    ASSERT_EQ_INT((int)motion_sent_tick[10], 100);

    len = serialize_asteroids_for_player_at_tick(
        buf, asteroids, v2(0.0f, 0.0f), sent, motion_sent_tick,
        motion_sent_pos, motion_sent_vel,
        100u + ASTEROID_NET_FAR_MOTION_HEARTBEAT_TICKS);

    ASSERT_EQ_INT(buf[1] | (buf[2] << 8), 1);
    ASSERT_EQ_INT(len, ASTEROID_MSG_HEADER + ASTEROID_RECORD_SIZE);
    ASSERT_EQ_INT((int)motion_sent_tick[10],
                  100 + ASTEROID_NET_FAR_MOTION_HEARTBEAT_TICKS);

    asteroids[10].pos = v2(1700.0f, 0.0f);
    len = serialize_asteroids_for_player_at_tick(
        buf, asteroids, v2(0.0f, 0.0f), sent, motion_sent_tick,
        motion_sent_pos, motion_sent_vel,
        100u + ASTEROID_NET_FAR_MOTION_HEARTBEAT_TICKS +
            ASTEROID_NET_MOVING_REPEAT_TICKS);

    ASSERT_EQ_INT(buf[1] | (buf[2] << 8), 1);
    ASSERT_EQ_INT(len, ASTEROID_MSG_HEADER + ASTEROID_RECORD_SIZE);
}

TEST(test_asteroid_delta_uses_far_error_budget_for_far_fast_motion) {
    asteroid_t asteroids[MAX_ASTEROIDS];
    memset(asteroids, 0, sizeof(asteroids));
    bool sent[MAX_ASTEROIDS] = {0};
    uint32_t motion_sent_tick[MAX_ASTEROIDS] = {0};
    vec2 motion_sent_pos[MAX_ASTEROIDS] = {0};
    vec2 motion_sent_vel[MAX_ASTEROIDS] = {0};

    asteroids[11].active = true;
    asteroids[11].pos = v2(1850.0f, 0.0f);
    asteroids[11].radius = 12.0f;
    asteroids[11].hp = 10.0f;
    asteroids[11].vel = v2(80.0f, 0.0f);
    sent[11] = true;
    motion_sent_tick[11] = 100u;
    motion_sent_pos[11] = asteroids[11].pos;
    motion_sent_vel[11] = asteroids[11].vel;

    uint8_t buf[ASTEROID_MSG_HEADER + MAX_ASTEROIDS * ASTEROID_RECORD_SIZE];
    asteroids[11].pos = v2(1950.0f, 0.0f);
    int len = serialize_asteroids_for_player_at_tick(
        buf, asteroids, v2(0.0f, 0.0f), sent, motion_sent_tick,
        motion_sent_pos, motion_sent_vel,
        100u + ASTEROID_NET_FAR_MOVING_REPEAT_TICKS);

    ASSERT_EQ_INT(buf[0], NET_MSG_WORLD_ASTEROIDS);
    ASSERT_EQ_INT(buf[1] | (buf[2] << 8), 0);
    ASSERT_EQ_INT(len, ASTEROID_MSG_HEADER);
    ASSERT_EQ_INT((int)motion_sent_tick[11], 100);

    asteroids[11].pos = v2(1981.0f, 0.0f);
    len = serialize_asteroids_for_player_at_tick(
        buf, asteroids, v2(0.0f, 0.0f), sent, motion_sent_tick,
        motion_sent_pos, motion_sent_vel,
        100u + ASTEROID_NET_FAR_MOVING_REPEAT_TICKS);

    ASSERT_EQ_INT(buf[1] | (buf[2] << 8), 1);
    ASSERT_EQ_INT(len, ASTEROID_MSG_HEADER + ASTEROID_RECORD_SIZE);
    ASSERT_EQ_INT((int)motion_sent_tick[11],
                  100 + ASTEROID_NET_FAR_MOVING_REPEAT_TICKS);
}

TEST(test_asteroid_delta_uses_very_far_error_budget) {
    ASSERT_EQ_INT((int)ASTEROID_NET_VERY_FAR_MOTION_HEARTBEAT_TICKS, 2400);
    ASSERT_EQ_FLOAT(ASTEROID_NET_VERY_FAR_PREDICT_ERROR_SQ,
                    128.0f * 128.0f, 0.001f);

    asteroid_t asteroids[MAX_ASTEROIDS];
    memset(asteroids, 0, sizeof(asteroids));
    bool sent[MAX_ASTEROIDS] = {0};
    uint32_t motion_sent_tick[MAX_ASTEROIDS] = {0};
    vec2 motion_sent_pos[MAX_ASTEROIDS] = {0};
    vec2 motion_sent_vel[MAX_ASTEROIDS] = {0};

    asteroids[12].active = true;
    asteroids[12].pos = v2(2200.0f, 0.0f);
    asteroids[12].radius = 12.0f;
    asteroids[12].hp = 10.0f;
    asteroids[12].vel = v2(10.0f, 0.0f);
    sent[12] = true;
    motion_sent_tick[12] = 100u;
    motion_sent_pos[12] = asteroids[12].pos;
    motion_sent_vel[12] = asteroids[12].vel;

    uint8_t buf[ASTEROID_MSG_HEADER + MAX_ASTEROIDS * ASTEROID_RECORD_SIZE];
    asteroids[12].pos = v2(2325.0f, 0.0f);
    int len = serialize_asteroids_for_player_at_tick(
        buf, asteroids, v2(0.0f, 0.0f), sent, motion_sent_tick,
        motion_sent_pos, motion_sent_vel, 700u);

    ASSERT_EQ_INT(buf[0], NET_MSG_WORLD_ASTEROIDS);
    ASSERT_EQ_INT(buf[1] | (buf[2] << 8), 0);
    ASSERT_EQ_INT(len, ASTEROID_MSG_HEADER);
    ASSERT_EQ_INT((int)motion_sent_tick[12], 100);

    asteroids[12].pos = v2(2375.0f, 0.0f);
    len = serialize_asteroids_for_player_at_tick(
        buf, asteroids, v2(0.0f, 0.0f), sent, motion_sent_tick,
        motion_sent_pos, motion_sent_vel,
        100u + ASTEROID_NET_VERY_FAR_SLOW_MOVING_REPEAT_TICKS);

    ASSERT_EQ_INT(buf[1] | (buf[2] << 8), 0);
    ASSERT_EQ_INT(len, ASTEROID_MSG_HEADER);
    ASSERT_EQ_INT((int)motion_sent_tick[12], 100);

    asteroids[12].pos = v2(2405.0f, 0.0f);
    len = serialize_asteroids_for_player_at_tick(
        buf, asteroids, v2(0.0f, 0.0f), sent, motion_sent_tick,
        motion_sent_pos, motion_sent_vel,
        100u + ASTEROID_NET_VERY_FAR_SLOW_MOVING_REPEAT_TICKS);

    ASSERT_EQ_INT(buf[1] | (buf[2] << 8), 0);
    ASSERT_EQ_INT(len, ASTEROID_MSG_HEADER);
    ASSERT_EQ_INT((int)motion_sent_tick[12], 100);

    asteroids[12].pos = v2(2435.0f, 0.0f);
    len = serialize_asteroids_for_player_at_tick(
        buf, asteroids, v2(0.0f, 0.0f), sent, motion_sent_tick,
        motion_sent_pos, motion_sent_vel,
        100u + ASTEROID_NET_VERY_FAR_SLOW_MOVING_REPEAT_TICKS);

    ASSERT_EQ_INT(buf[1] | (buf[2] << 8), 1);
    ASSERT_EQ_INT(len, ASTEROID_MSG_HEADER + ASTEROID_RECORD_SIZE);
    ASSERT_EQ_INT((int)motion_sent_tick[12],
                  100 + ASTEROID_NET_VERY_FAR_SLOW_MOVING_REPEAT_TICKS);

    asteroids[12].pos = v2(2410.0f, 0.0f);
    motion_sent_tick[12] = 100u;
    motion_sent_pos[12] = v2(2200.0f, 0.0f);
    motion_sent_vel[12] = asteroids[12].vel;
    len = serialize_asteroids_for_player_at_tick(
        buf, asteroids, v2(0.0f, 0.0f), sent, motion_sent_tick,
        motion_sent_pos, motion_sent_vel,
        100u + ASTEROID_NET_VERY_FAR_MOTION_HEARTBEAT_TICKS);

    ASSERT_EQ_INT(buf[1] | (buf[2] << 8), 1);
    ASSERT_EQ_INT(len, ASTEROID_MSG_HEADER + ASTEROID_RECORD_SIZE);
    ASSERT_EQ_INT((int)motion_sent_tick[12],
                  100 + ASTEROID_NET_VERY_FAR_MOTION_HEARTBEAT_TICKS);
}

TEST(test_asteroid_delta_uses_motion_stream_for_clean_moving_repeat) {
    asteroid_t asteroids[MAX_ASTEROIDS];
    memset(asteroids, 0, sizeof(asteroids));
    bool sent[MAX_ASTEROIDS] = {0};
    uint32_t motion_sent_tick[MAX_ASTEROIDS] = {0};
    vec2 motion_sent_pos[MAX_ASTEROIDS] = {0};
    vec2 motion_sent_vel[MAX_ASTEROIDS] = {0};

    asteroids[6].active = true;
    asteroids[6].pos = v2(30.0f, 40.0f);
    asteroids[6].radius = 10.0f;
    asteroids[6].hp = 10.0f;
    asteroids[6].ore = 3.0f;
    asteroids[6].vel = v2(2.0f, -2.0f);
    sent[6] = true;
    motion_sent_tick[6] = 100u;
    motion_sent_pos[6] = v2(0.0f, 0.0f);
    motion_sent_vel[6] = asteroids[6].vel;

    uint8_t full[ASTEROID_MSG_HEADER + MAX_ASTEROIDS * ASTEROID_RECORD_SIZE];
    uint8_t motion[ASTEROID_MOTION_MSG_HEADER +
                   MAX_ASTEROIDS * ASTEROID_MOTION_RECORD_SIZE];
    int motion_len = 0;
    int full_len = serialize_asteroids_for_player_split_at_tick(
        full, motion, &motion_len, NULL, NULL, NULL, NULL,
        asteroids, v2(0.0f, 0.0f), sent, motion_sent_tick,
        motion_sent_pos, motion_sent_vel,
        100u + ASTEROID_NET_MOVING_REPEAT_TICKS);

    ASSERT_EQ_INT(full[0], NET_MSG_WORLD_ASTEROIDS);
    ASSERT_EQ_INT(full[1] | (full[2] << 8), 0);
    ASSERT_EQ_INT(full_len, ASTEROID_MSG_HEADER);
    ASSERT_EQ_INT(motion[0], NET_MSG_WORLD_ASTEROID_MOTION);
    ASSERT_EQ_INT(motion[1] | (motion[2] << 8), 1);
    ASSERT_EQ_INT(motion_len,
                  ASTEROID_MOTION_MSG_HEADER + ASTEROID_MOTION_RECORD_SIZE);

    const uint8_t *p = &motion[ASTEROID_MOTION_MSG_HEADER];
    ASSERT_EQ_INT(p[0] | (p[1] << 8), 6);
    ASSERT_EQ_FLOAT(read_f32_le(&p[2]), 30.0f, 0.01f);
    ASSERT_EQ_FLOAT(read_f32_le(&p[6]), 40.0f, 0.01f);
    ASSERT_EQ_FLOAT(read_f32_le(&p[10]), 2.0f, 0.01f);
    ASSERT_EQ_FLOAT(read_f32_le(&p[14]), -2.0f, 0.01f);
    ASSERT_EQ_INT((int)motion_sent_tick[6],
                  100 + ASTEROID_NET_MOVING_REPEAT_TICKS);
}

TEST(test_asteroid_delta_quantizes_near_motion_when_available) {
    asteroid_t asteroids[MAX_ASTEROIDS];
    memset(asteroids, 0, sizeof(asteroids));
    bool sent[MAX_ASTEROIDS] = {0};
    uint32_t motion_sent_tick[MAX_ASTEROIDS] = {0};
    vec2 motion_sent_pos[MAX_ASTEROIDS] = {0};
    vec2 motion_sent_vel[MAX_ASTEROIDS] = {0};

    asteroids[6].active = true;
    asteroids[6].pos = v2(32.0f, 40.0f);
    asteroids[6].radius = 10.0f;
    asteroids[6].hp = 10.0f;
    asteroids[6].vel = v2(2.0f, -2.0f);
    sent[6] = true;
    motion_sent_tick[6] = 100u;
    motion_sent_pos[6] = v2(0.0f, 0.0f);
    motion_sent_vel[6] = asteroids[6].vel;

    uint8_t full[ASTEROID_MSG_HEADER + MAX_ASTEROIDS * ASTEROID_RECORD_SIZE];
    uint8_t motion[ASTEROID_MOTION_MSG_HEADER +
                   MAX_ASTEROIDS * ASTEROID_MOTION_RECORD_SIZE];
    uint8_t motion_q[ASTEROID_MOTION_Q_MSG_HEADER +
                     MAX_ASTEROIDS * ASTEROID_MOTION_Q_RECORD_SIZE];
    int motion_len = 0;
    int motion_q_len = 0;
    int full_len = serialize_asteroids_for_player_split_at_tick(
        full, motion, &motion_len, motion_q, &motion_q_len,
        NULL, NULL,
        asteroids, v2(0.0f, 0.0f), sent,
        motion_sent_tick, motion_sent_pos, motion_sent_vel,
        100u + ASTEROID_NET_MOVING_REPEAT_TICKS);

    ASSERT_EQ_INT(full[0], NET_MSG_WORLD_ASTEROIDS);
    ASSERT_EQ_INT(full[1] | (full[2] << 8), 0);
    ASSERT_EQ_INT(full_len, ASTEROID_MSG_HEADER);
    ASSERT_EQ_INT(motion[0], NET_MSG_WORLD_ASTEROID_MOTION);
    ASSERT_EQ_INT(motion[1] | (motion[2] << 8), 0);
    ASSERT_EQ_INT(motion_len, ASTEROID_MOTION_MSG_HEADER);

    ASSERT_EQ_INT(motion_q[0], NET_MSG_WORLD_ASTEROID_MOTION_Q);
    ASSERT_EQ_INT(motion_q[1] | (motion_q[2] << 8), 1);
    ASSERT_EQ_INT(motion_q_len,
                  ASTEROID_MOTION_Q_MSG_HEADER + ASTEROID_MOTION_Q_RECORD_SIZE);

    const uint8_t *p = &motion_q[ASTEROID_MOTION_Q_MSG_HEADER];
    ASSERT_EQ_INT(p[0] | (p[1] << 8), 6);
    ASSERT_EQ_INT((int)(int16_t)(p[2] | ((uint16_t)p[3] << 8)), 8);
    ASSERT_EQ_INT((int)(int16_t)(p[4] | ((uint16_t)p[5] << 8)), 10);
    ASSERT_EQ_INT((int)(int16_t)(p[6] | ((uint16_t)p[7] << 8)), 8);
    ASSERT_EQ_INT((int)(int16_t)(p[8] | ((uint16_t)p[9] << 8)), -8);
    ASSERT_EQ_INT((int)motion_sent_tick[6],
                  100 + ASTEROID_NET_MOVING_REPEAT_TICKS);
}

TEST(test_asteroid_delta_elides_unchanged_quantized_velocity) {
    asteroid_t asteroids[MAX_ASTEROIDS];
    memset(asteroids, 0, sizeof(asteroids));
    bool sent[MAX_ASTEROIDS] = {0};
    uint32_t motion_sent_tick[MAX_ASTEROIDS] = {0};
    vec2 motion_sent_pos[MAX_ASTEROIDS] = {0};
    vec2 motion_sent_vel[MAX_ASTEROIDS] = {0};

    asteroids[6].active = true;
    asteroids[6].pos = v2(64.0f, 40.0f);
    asteroids[6].radius = 10.0f;
    asteroids[6].hp = 10.0f;
    asteroids[6].vel = v2(2.0f, -2.0f);
    sent[6] = true;
    motion_sent_tick[6] = 100u;
    motion_sent_pos[6] = v2(0.0f, 0.0f);
    motion_sent_vel[6] = asteroids[6].vel;

    uint8_t full[ASTEROID_MSG_HEADER + MAX_ASTEROIDS * ASTEROID_RECORD_SIZE];
    uint8_t motion[ASTEROID_MOTION_MSG_HEADER +
                   MAX_ASTEROIDS * ASTEROID_MOTION_RECORD_SIZE];
    uint8_t motion_q[ASTEROID_MOTION_Q_MSG_HEADER +
                     MAX_ASTEROIDS * ASTEROID_MOTION_Q_RECORD_SIZE];
    uint8_t pos_q[ASTEROID_POS_Q_MSG_HEADER +
                  MAX_ASTEROIDS * ASTEROID_POS_Q_RECORD_SIZE];
    int motion_len = 0;
    int motion_q_len = 0;
    int pos_q_len = 0;
    int full_len = serialize_asteroids_for_player_split_ext_at_tick(
        full, motion, &motion_len, motion_q, &motion_q_len,
        pos_q, &pos_q_len, NULL, NULL, NULL, NULL,
        asteroids, v2(0.0f, 0.0f), sent,
        motion_sent_tick, motion_sent_pos, motion_sent_vel,
        100u + ASTEROID_NET_MOVING_REPEAT_TICKS);

    ASSERT_EQ_INT(full[0], NET_MSG_WORLD_ASTEROIDS);
    ASSERT_EQ_INT(full[1] | (full[2] << 8), 0);
    ASSERT_EQ_INT(full_len, ASTEROID_MSG_HEADER);
    ASSERT_EQ_INT(motion[0], NET_MSG_WORLD_ASTEROID_MOTION);
    ASSERT_EQ_INT(motion[1] | (motion[2] << 8), 0);
    ASSERT_EQ_INT(motion_len, ASTEROID_MOTION_MSG_HEADER);
    ASSERT_EQ_INT(motion_q[0], NET_MSG_WORLD_ASTEROID_MOTION_Q);
    ASSERT_EQ_INT(motion_q[1] | (motion_q[2] << 8), 0);
    ASSERT_EQ_INT(motion_q_len, ASTEROID_MOTION_Q_MSG_HEADER);

    ASSERT_EQ_INT(pos_q[0], NET_MSG_WORLD_ASTEROID_POS_Q);
    ASSERT_EQ_INT(pos_q[1] | (pos_q[2] << 8), 1);
    ASSERT_EQ_INT(pos_q_len,
                  ASTEROID_POS_Q_MSG_HEADER + ASTEROID_POS_Q_RECORD_SIZE);
    const uint8_t *p = &pos_q[ASTEROID_POS_Q_MSG_HEADER];
    ASSERT_EQ_INT(p[0] | (p[1] << 8), 6);
    ASSERT_EQ_INT((int)(int16_t)(p[2] | ((uint16_t)p[3] << 8)), 16);
    ASSERT_EQ_INT((int)(int16_t)(p[4] | ((uint16_t)p[5] << 8)), 10);
    ASSERT_EQ_INT((int)motion_sent_tick[6],
                  100 + ASTEROID_NET_MOVING_REPEAT_TICKS);

    asteroids[6].pos = v2(96.0f, 40.0f);
    asteroids[6].vel = v2(12.0f, -2.0f);
    motion_sent_tick[6] = 100u;
    motion_sent_pos[6] = v2(0.0f, 0.0f);
    motion_sent_vel[6] = v2(2.0f, -2.0f);
    motion_q_len = 0;
    pos_q_len = 0;
    full_len = serialize_asteroids_for_player_split_ext_at_tick(
        full, motion, &motion_len, motion_q, &motion_q_len,
        pos_q, &pos_q_len, NULL, NULL, NULL, NULL,
        asteroids, v2(0.0f, 0.0f), sent,
        motion_sent_tick, motion_sent_pos, motion_sent_vel,
        100u + ASTEROID_NET_MOVING_REPEAT_TICKS);

    ASSERT_EQ_INT(full_len, ASTEROID_MSG_HEADER);
    ASSERT_EQ_INT(motion_q[1] | (motion_q[2] << 8), 1);
    ASSERT_EQ_INT(motion_q_len,
                  ASTEROID_MOTION_Q_MSG_HEADER + ASTEROID_MOTION_Q_RECORD_SIZE);
    ASSERT_EQ_INT(pos_q[1] | (pos_q[2] << 8), 0);
    ASSERT_EQ_INT(pos_q_len, ASTEROID_POS_Q_MSG_HEADER);
}

TEST(test_asteroid_delta_uses_pos8_q_for_low_index_position_only) {
    asteroid_t asteroids[MAX_ASTEROIDS];
    memset(asteroids, 0, sizeof(asteroids));
    bool sent[MAX_ASTEROIDS] = {0};
    uint32_t motion_sent_tick[MAX_ASTEROIDS] = {0};
    vec2 motion_sent_pos[MAX_ASTEROIDS] = {0};
    vec2 motion_sent_vel[MAX_ASTEROIDS] = {0};

    asteroids[6].active = true;
    asteroids[6].pos = v2(64.0f, 40.0f);
    asteroids[6].radius = 10.0f;
    asteroids[6].hp = 10.0f;
    asteroids[6].vel = v2(2.0f, -2.0f);
    sent[6] = true;
    motion_sent_tick[6] = 100u;
    motion_sent_pos[6] = v2(0.0f, 0.0f);
    motion_sent_vel[6] = asteroids[6].vel;

    asteroids[300].active = true;
    asteroids[300].pos = v2(96.0f, -12.0f);
    asteroids[300].radius = 10.0f;
    asteroids[300].hp = 10.0f;
    asteroids[300].vel = v2(2.0f, -2.0f);
    sent[300] = true;
    motion_sent_tick[300] = 100u;
    motion_sent_pos[300] = v2(0.0f, 0.0f);
    motion_sent_vel[300] = asteroids[300].vel;

    uint8_t full[ASTEROID_MSG_HEADER + MAX_ASTEROIDS * ASTEROID_RECORD_SIZE];
    uint8_t motion[ASTEROID_MOTION_MSG_HEADER +
                   MAX_ASTEROIDS * ASTEROID_MOTION_RECORD_SIZE];
    uint8_t motion_q[ASTEROID_MOTION_Q_MSG_HEADER +
                     MAX_ASTEROIDS * ASTEROID_MOTION_Q_RECORD_SIZE];
    uint8_t pos_q[ASTEROID_POS_Q_MSG_HEADER +
                  MAX_ASTEROIDS * ASTEROID_POS_Q_RECORD_SIZE];
    uint8_t pos8_q[ASTEROID_POS8_Q_MSG_HEADER +
                   256 * ASTEROID_POS8_Q_RECORD_SIZE];
    int motion_len = 0;
    int motion_q_len = 0;
    int pos_q_len = 0;
    int pos8_q_len = 0;
    int full_len = serialize_asteroids_for_player_split_ext_at_tick(
        full, motion, &motion_len, motion_q, &motion_q_len,
        pos_q, &pos_q_len, pos8_q, &pos8_q_len, NULL, NULL,
        asteroids, v2(0.0f, 0.0f), sent,
        motion_sent_tick, motion_sent_pos, motion_sent_vel,
        100u + ASTEROID_NET_MOVING_REPEAT_TICKS);

    ASSERT_EQ_INT(full[0], NET_MSG_WORLD_ASTEROIDS);
    ASSERT_EQ_INT(full_len, ASTEROID_MSG_HEADER);
    ASSERT_EQ_INT(motion_q_len, ASTEROID_MOTION_Q_MSG_HEADER);

    ASSERT_EQ_INT(pos8_q[0], NET_MSG_WORLD_ASTEROID_POS8_Q);
    ASSERT_EQ_INT(pos8_q[1], 1);
    ASSERT_EQ_INT(pos8_q_len,
                  ASTEROID_POS8_Q_MSG_HEADER + ASTEROID_POS8_Q_RECORD_SIZE);
    const uint8_t *p8 = &pos8_q[ASTEROID_POS8_Q_MSG_HEADER];
    ASSERT_EQ_INT(p8[0], 6);
    ASSERT_EQ_INT((int)(int16_t)read_u16_le(&p8[1]), 16);
    ASSERT_EQ_INT((int)(int16_t)read_u16_le(&p8[3]), 10);

    ASSERT_EQ_INT(pos_q[0], NET_MSG_WORLD_ASTEROID_POS_Q);
    ASSERT_EQ_INT(pos_q[1] | (pos_q[2] << 8), 1);
    ASSERT_EQ_INT(pos_q_len,
                  ASTEROID_POS_Q_MSG_HEADER + ASTEROID_POS_Q_RECORD_SIZE);
    const uint8_t *p = &pos_q[ASTEROID_POS_Q_MSG_HEADER];
    ASSERT_EQ_INT(p[0] | (p[1] << 8), 300);
    ASSERT_EQ_INT((int)(int16_t)read_u16_le(&p[2]), 24);
    ASSERT_EQ_INT((int)(int16_t)read_u16_le(&p[4]), -3);
}

TEST(test_asteroid_position_only_prefers_signed_byte_delta_streams) {
    asteroid_t asteroids[MAX_ASTEROIDS];
    memset(asteroids, 0, sizeof(asteroids));
    bool sent[MAX_ASTEROIDS] = {0};
    uint32_t motion_sent_tick[MAX_ASTEROIDS] = {0};
    vec2 motion_sent_pos[MAX_ASTEROIDS] = {0};
    vec2 motion_sent_vel[MAX_ASTEROIDS] = {0};

    asteroids[6].active = true;
    asteroids[6].pos = v2(64.0f, 40.0f);
    asteroids[6].radius = 10.0f;
    asteroids[6].hp = 10.0f;
    asteroids[6].vel = v2(2.0f, -2.0f);
    sent[6] = true;
    motion_sent_tick[6] = 100u;
    motion_sent_pos[6] = v2(60.0f, 36.0f);
    motion_sent_vel[6] = asteroids[6].vel;

    asteroids[300].active = true;
    asteroids[300].pos = v2(96.0f, -12.0f);
    asteroids[300].radius = 10.0f;
    asteroids[300].hp = 10.0f;
    asteroids[300].vel = v2(2.0f, -2.0f);
    sent[300] = true;
    motion_sent_tick[300] = 100u;
    motion_sent_pos[300] = v2(92.0f, -16.0f);
    motion_sent_vel[300] = asteroids[300].vel;

    uint8_t full[ASTEROID_MSG_HEADER + MAX_ASTEROIDS * ASTEROID_RECORD_SIZE];
    uint8_t motion[ASTEROID_MOTION_MSG_HEADER +
                   MAX_ASTEROIDS * ASTEROID_MOTION_RECORD_SIZE];
    uint8_t motion_q[ASTEROID_MOTION_Q_MSG_HEADER +
                     MAX_ASTEROIDS * ASTEROID_MOTION_Q_RECORD_SIZE];
    uint8_t posd_q[ASTEROID_POSD_Q_MSG_HEADER +
                   MAX_ASTEROIDS * ASTEROID_POSD_Q_RECORD_SIZE];
    uint8_t posd8_q[ASTEROID_POSD8_Q_MSG_HEADER +
                    256 * ASTEROID_POSD8_Q_RECORD_SIZE];
    uint8_t pos_q[ASTEROID_POS_Q_MSG_HEADER +
                  MAX_ASTEROIDS * ASTEROID_POS_Q_RECORD_SIZE];
    uint8_t pos8_q[ASTEROID_POS8_Q_MSG_HEADER +
                   256 * ASTEROID_POS8_Q_RECORD_SIZE];
    int motion_len = 0;
    int motion_q_len = 0;
    int posd_q_len = 0;
    int posd8_q_len = 0;
    int pos_q_len = 0;
    int pos8_q_len = 0;
    int full_len = serialize_asteroids_for_player_split_ext_state_at_tick(
        full, NULL, NULL, NULL, NULL,
        motion, &motion_len, motion_q, &motion_q_len,
        posd_q, &posd_q_len, posd8_q, &posd8_q_len,
        pos_q, &pos_q_len, pos8_q, &pos8_q_len,
        NULL, NULL, NULL, NULL,
        asteroids, v2(0.0f, 0.0f), sent,
        motion_sent_tick, motion_sent_pos, motion_sent_vel,
        NULL, NULL, NULL, 100u + ASTEROID_NET_SLOW_MOTION_HEARTBEAT_TICKS);

    ASSERT_EQ_INT(full_len, ASTEROID_MSG_HEADER);
    ASSERT_EQ_INT(motion_len, ASTEROID_MOTION_MSG_HEADER);
    ASSERT_EQ_INT(motion_q_len, ASTEROID_MOTION_Q_MSG_HEADER);

    ASSERT_EQ_INT(posd8_q[0], NET_MSG_WORLD_ASTEROID_POSD8_Q);
    ASSERT_EQ_INT(posd8_q[1], 1);
    ASSERT_EQ_INT(posd8_q_len,
                  ASTEROID_POSD8_Q_MSG_HEADER + ASTEROID_POSD8_Q_RECORD_SIZE);
    const uint8_t *p8 = &posd8_q[ASTEROID_POSD8_Q_MSG_HEADER];
    ASSERT_EQ_INT(p8[0], 6);
    ASSERT_EQ_INT((int)(int8_t)p8[1], 1);
    ASSERT_EQ_INT((int)(int8_t)p8[2], 1);

    ASSERT_EQ_INT(posd_q[0], NET_MSG_WORLD_ASTEROID_POSD_Q);
    ASSERT_EQ_INT(posd_q[1] | (posd_q[2] << 8), 1);
    ASSERT_EQ_INT(posd_q_len,
                  ASTEROID_POSD_Q_MSG_HEADER + ASTEROID_POSD_Q_RECORD_SIZE);
    const uint8_t *p = &posd_q[ASTEROID_POSD_Q_MSG_HEADER];
    ASSERT_EQ_INT(p[0] | (p[1] << 8), 300);
    ASSERT_EQ_INT((int)(int8_t)p[2], 1);
    ASSERT_EQ_INT((int)(int8_t)p[3], 1);

    ASSERT_EQ_INT(pos8_q[1], 0);
    ASSERT_EQ_INT(pos8_q_len, ASTEROID_POS8_Q_MSG_HEADER);
    ASSERT_EQ_INT(pos_q[1] | (pos_q[2] << 8), 0);
    ASSERT_EQ_INT(pos_q_len, ASTEROID_POS_Q_MSG_HEADER);
    ASSERT_EQ_FLOAT(motion_sent_pos[6].x, 64.0f, 0.01f);
    ASSERT_EQ_FLOAT(motion_sent_pos[300].x, 96.0f, 0.01f);
}

TEST(test_asteroid_position_delta_falls_back_when_delta_exceeds_i8) {
    asteroid_t asteroids[MAX_ASTEROIDS];
    memset(asteroids, 0, sizeof(asteroids));
    bool sent[MAX_ASTEROIDS] = {0};
    uint32_t motion_sent_tick[MAX_ASTEROIDS] = {0};
    vec2 motion_sent_pos[MAX_ASTEROIDS] = {0};
    vec2 motion_sent_vel[MAX_ASTEROIDS] = {0};

    asteroids[6].active = true;
    asteroids[6].pos = v2(64.0f, 40.0f);
    asteroids[6].radius = 10.0f;
    asteroids[6].hp = 10.0f;
    asteroids[6].vel = v2(2.0f, -2.0f);
    sent[6] = true;
    motion_sent_tick[6] = 100u;
    motion_sent_pos[6] = v2(-1000.0f, 40.0f);
    motion_sent_vel[6] = asteroids[6].vel;

    uint8_t full[ASTEROID_MSG_HEADER + MAX_ASTEROIDS * ASTEROID_RECORD_SIZE];
    uint8_t motion[ASTEROID_MOTION_MSG_HEADER +
                   MAX_ASTEROIDS * ASTEROID_MOTION_RECORD_SIZE];
    uint8_t motion_q[ASTEROID_MOTION_Q_MSG_HEADER +
                     MAX_ASTEROIDS * ASTEROID_MOTION_Q_RECORD_SIZE];
    uint8_t posd_q[ASTEROID_POSD_Q_MSG_HEADER +
                   MAX_ASTEROIDS * ASTEROID_POSD_Q_RECORD_SIZE];
    uint8_t posd8_q[ASTEROID_POSD8_Q_MSG_HEADER +
                    256 * ASTEROID_POSD8_Q_RECORD_SIZE];
    uint8_t pos_q[ASTEROID_POS_Q_MSG_HEADER +
                  MAX_ASTEROIDS * ASTEROID_POS_Q_RECORD_SIZE];
    uint8_t pos8_q[ASTEROID_POS8_Q_MSG_HEADER +
                   256 * ASTEROID_POS8_Q_RECORD_SIZE];
    int motion_len = 0;
    int motion_q_len = 0;
    int posd_q_len = 0;
    int posd8_q_len = 0;
    int pos_q_len = 0;
    int pos8_q_len = 0;
    int full_len = serialize_asteroids_for_player_split_ext_state_at_tick(
        full, NULL, NULL, NULL, NULL,
        motion, &motion_len, motion_q, &motion_q_len,
        posd_q, &posd_q_len, posd8_q, &posd8_q_len,
        pos_q, &pos_q_len, pos8_q, &pos8_q_len,
        NULL, NULL, NULL, NULL,
        asteroids, v2(0.0f, 0.0f), sent,
        motion_sent_tick, motion_sent_pos, motion_sent_vel,
        NULL, NULL, NULL, 100u + ASTEROID_NET_SLOW_MOTION_HEARTBEAT_TICKS);

    ASSERT_EQ_INT(full_len, ASTEROID_MSG_HEADER);
    ASSERT_EQ_INT(posd8_q[1], 0);
    ASSERT_EQ_INT(posd8_q_len, ASTEROID_POSD8_Q_MSG_HEADER);
    ASSERT_EQ_INT(posd_q[1] | (posd_q[2] << 8), 0);
    ASSERT_EQ_INT(posd_q_len, ASTEROID_POSD_Q_MSG_HEADER);

    ASSERT_EQ_INT(pos8_q[0], NET_MSG_WORLD_ASTEROID_POS8_Q);
    ASSERT_EQ_INT(pos8_q[1], 1);
    ASSERT_EQ_INT(pos8_q_len,
                  ASTEROID_POS8_Q_MSG_HEADER + ASTEROID_POS8_Q_RECORD_SIZE);
    const uint8_t *p8 = &pos8_q[ASTEROID_POS8_Q_MSG_HEADER];
    ASSERT_EQ_INT(p8[0], 6);
    ASSERT_EQ_INT((int)(int16_t)read_u16_le(&p8[1]), 16);
    ASSERT_EQ_INT((int)(int16_t)read_u16_le(&p8[3]), 10);
}

TEST(test_asteroid_identity_prefers_compact_quantized_upserts) {
    asteroid_t asteroids[MAX_ASTEROIDS];
    memset(asteroids, 0, sizeof(asteroids));
    bool sent[MAX_ASTEROIDS] = {0};
    uint32_t motion_sent_tick[MAX_ASTEROIDS] = {0};
    vec2 motion_sent_pos[MAX_ASTEROIDS] = {0};
    vec2 motion_sent_vel[MAX_ASTEROIDS] = {0};

    asteroids[6].active = true;
    asteroids[6].pos = v2(64.0f, 40.0f);
    asteroids[6].vel = v2(2.0f, -2.0f);
    asteroids[6].radius = 10.5f;
    asteroids[6].hp = 12.25f;
    asteroids[6].ore = 7.5f;
    asteroids[6].smelt_progress = 0.5f;
    asteroids[6].grade = MINING_GRADE_RARE;
    asteroids[6].crystal_stage = CRYSTAL_STAGE_INTERMEDIATE;
    asteroids[6].phase = ASTEROID_PHASE_GAS_RICH;
    asteroids[6].tier = ASTEROID_TIER_L;
    asteroids[6].commodity = COMMODITY_CUPRITE_ORE;
    asteroids[6].fracture_child = true;

    asteroids[300].active = true;
    asteroids[300].pos = v2(96.0f, -12.0f);
    asteroids[300].vel = v2(3.0f, 1.0f);
    asteroids[300].radius = 18.0f;
    asteroids[300].hp = 20.0f;
    asteroids[300].ore = 11.0f;

    uint8_t full[ASTEROID_MSG_HEADER + MAX_ASTEROIDS * ASTEROID_RECORD_SIZE];
    uint8_t asteroids_q[ASTEROID_Q_MSG_HEADER +
                        MAX_ASTEROIDS * ASTEROID_Q_RECORD_SIZE];
    uint8_t asteroids8_q[ASTEROID8_Q_MSG_HEADER +
                         256 * ASTEROID8_Q_RECORD_SIZE];
    uint8_t motion[ASTEROID_MOTION_MSG_HEADER +
                   MAX_ASTEROIDS * ASTEROID_MOTION_RECORD_SIZE];
    uint8_t motion_q[ASTEROID_MOTION_Q_MSG_HEADER +
                     MAX_ASTEROIDS * ASTEROID_MOTION_Q_RECORD_SIZE];
    int asteroids_q_len = 0;
    int asteroids8_q_len = 0;
    int motion_len = 0;
    int motion_q_len = 0;
    int full_len = serialize_asteroids_for_player_split_ext_state_at_tick(
        full, asteroids_q, &asteroids_q_len,
        asteroids8_q, &asteroids8_q_len,
        motion, &motion_len, motion_q, &motion_q_len,
        NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
        NULL, NULL, NULL, NULL,
        asteroids, v2(0.0f, 0.0f), sent,
        motion_sent_tick, motion_sent_pos, motion_sent_vel,
        NULL, NULL, NULL, 100u);

    ASSERT_EQ_INT(full[0], NET_MSG_WORLD_ASTEROIDS);
    ASSERT_EQ_INT(full[1] | (full[2] << 8), 0);
    ASSERT_EQ_INT(full_len, ASTEROID_MSG_HEADER);

    ASSERT_EQ_INT(asteroids8_q[0], NET_MSG_WORLD_ASTEROIDS8_Q);
    ASSERT_EQ_INT(asteroids8_q[1], 1);
    ASSERT_EQ_INT(asteroids8_q_len,
                  ASTEROID8_Q_MSG_HEADER + ASTEROID8_Q_RECORD_SIZE);
    const uint8_t *p8 = &asteroids8_q[ASTEROID8_Q_MSG_HEADER];
    ASSERT_EQ_INT(p8[0], 6);
    ASSERT_EQ_INT(p8[1] & 1, 1);
    ASSERT_EQ_INT(p8[1] & (1 << 1), (1 << 1));
    ASSERT_EQ_INT((int)(int16_t)read_u16_le(&p8[2]), 16);
    ASSERT_EQ_INT((int)(int16_t)read_u16_le(&p8[4]), 10);
    ASSERT_EQ_INT((int)(int16_t)read_u16_le(&p8[6]), 8);
    ASSERT_EQ_INT((int)(int16_t)read_u16_le(&p8[8]), -8);
    ASSERT_EQ_INT(read_u16_le(&p8[10]), 98);
    ASSERT_EQ_INT(read_u16_le(&p8[12]), 60);
    ASSERT_EQ_INT(read_u16_le(&p8[14]), 84);
    ASSERT_EQ_INT(p8[16], 127);
    ASSERT_EQ_INT(p8[17],
                  MINING_GRADE_RARE |
                  (CRYSTAL_STAGE_INTERMEDIATE << 3) |
                  (ASTEROID_PHASE_GAS_RICH << 5));

    ASSERT_EQ_INT(asteroids_q[0], NET_MSG_WORLD_ASTEROIDS_Q);
    ASSERT_EQ_INT(asteroids_q[1] | (asteroids_q[2] << 8), 1);
    ASSERT_EQ_INT(asteroids_q_len,
                  ASTEROID_Q_MSG_HEADER + ASTEROID_Q_RECORD_SIZE);
    const uint8_t *p = &asteroids_q[ASTEROID_Q_MSG_HEADER];
    ASSERT_EQ_INT(p[0] | (p[1] << 8), 300);
    ASSERT_EQ_INT((int)(int16_t)read_u16_le(&p[3]), 24);
    ASSERT_EQ_INT((int)(int16_t)read_u16_le(&p[5]), -3);
    ASSERT_EQ_INT((int)(int16_t)read_u16_le(&p[7]), 12);
    ASSERT_EQ_INT((int)(int16_t)read_u16_le(&p[9]), 4);
    ASSERT_EQ_INT(read_u16_le(&p[11]), 160);
    ASSERT_EQ_INT(read_u16_le(&p[13]), 88);
    ASSERT_EQ_INT(read_u16_le(&p[15]), 144);

    ASSERT(sent[6]);
    ASSERT(sent[300]);
    ASSERT_EQ_INT((int)motion_sent_tick[6], 100);
    ASSERT_EQ_FLOAT(motion_sent_pos[300].x, 96.0f, 0.01f);
    ASSERT_EQ_FLOAT(motion_sent_vel[300].x, 3.0f, 0.01f);
}

TEST(test_asteroid_delta_uses_pos_q_for_small_velocity_drift) {
    ASSERT_EQ_FLOAT(ASTEROID_NET_POS_ONLY_DRIFT_WINDOW_SEC, 0.5f, 0.001f);
    ASSERT_EQ_FLOAT(ASTEROID_NET_POS_ONLY_NEAR_DRIFT_SQ, 16.0f, 0.001f);

    asteroid_t asteroids[MAX_ASTEROIDS];
    memset(asteroids, 0, sizeof(asteroids));
    bool sent[MAX_ASTEROIDS] = {0};
    uint32_t motion_sent_tick[MAX_ASTEROIDS] = {0};
    vec2 motion_sent_pos[MAX_ASTEROIDS] = {0};
    vec2 motion_sent_vel[MAX_ASTEROIDS] = {0};

    asteroids[6].active = true;
    asteroids[6].pos = v2(30.0f, 0.0f);
    asteroids[6].radius = 10.0f;
    asteroids[6].hp = 10.0f;
    asteroids[6].vel = v2(12.0f, 0.0f);
    sent[6] = true;
    motion_sent_tick[6] = 100u;
    motion_sent_pos[6] = v2(0.0f, 0.0f);
    motion_sent_vel[6] = v2(15.0f, 0.0f);

    uint8_t full[ASTEROID_MSG_HEADER + MAX_ASTEROIDS * ASTEROID_RECORD_SIZE];
    uint8_t motion[ASTEROID_MOTION_MSG_HEADER +
                   MAX_ASTEROIDS * ASTEROID_MOTION_RECORD_SIZE];
    uint8_t motion_q[ASTEROID_MOTION_Q_MSG_HEADER +
                     MAX_ASTEROIDS * ASTEROID_MOTION_Q_RECORD_SIZE];
    uint8_t pos_q[ASTEROID_POS_Q_MSG_HEADER +
                  MAX_ASTEROIDS * ASTEROID_POS_Q_RECORD_SIZE];
    int motion_len = 0;
    int motion_q_len = 0;
    int pos_q_len = 0;
    int full_len = serialize_asteroids_for_player_split_ext_at_tick(
        full, motion, &motion_len, motion_q, &motion_q_len,
        pos_q, &pos_q_len, NULL, NULL, NULL, NULL,
        asteroids, v2(0.0f, 0.0f), sent,
        motion_sent_tick, motion_sent_pos, motion_sent_vel,
        100u + ASTEROID_NET_MOTION_HEARTBEAT_TICKS);

    ASSERT_EQ_INT(full[0], NET_MSG_WORLD_ASTEROIDS);
    ASSERT_EQ_INT(full[1] | (full[2] << 8), 0);
    ASSERT_EQ_INT(full_len, ASTEROID_MSG_HEADER);
    ASSERT_EQ_INT(motion[0], NET_MSG_WORLD_ASTEROID_MOTION);
    ASSERT_EQ_INT(motion[1] | (motion[2] << 8), 0);
    ASSERT_EQ_INT(motion_len, ASTEROID_MOTION_MSG_HEADER);
    ASSERT_EQ_INT(motion_q[0], NET_MSG_WORLD_ASTEROID_MOTION_Q);
    ASSERT_EQ_INT(motion_q[1] | (motion_q[2] << 8), 0);
    ASSERT_EQ_INT(motion_q_len, ASTEROID_MOTION_Q_MSG_HEADER);

    ASSERT_EQ_INT(pos_q[0], NET_MSG_WORLD_ASTEROID_POS_Q);
    ASSERT_EQ_INT(pos_q[1] | (pos_q[2] << 8), 1);
    ASSERT_EQ_INT(pos_q_len,
                  ASTEROID_POS_Q_MSG_HEADER + ASTEROID_POS_Q_RECORD_SIZE);
    ASSERT_EQ_FLOAT(motion_sent_pos[6].x, 30.0f, 0.01f);
    ASSERT_EQ_FLOAT(motion_sent_vel[6].x, 15.0f, 0.01f);

    asteroids[6].vel = v2(24.0f, 0.0f);
    motion_sent_tick[6] = 100u;
    motion_sent_pos[6] = v2(0.0f, 0.0f);
    motion_sent_vel[6] = v2(15.0f, 0.0f);
    motion_q_len = 0;
    pos_q_len = 0;
    full_len = serialize_asteroids_for_player_split_ext_at_tick(
        full, motion, &motion_len, motion_q, &motion_q_len,
        pos_q, &pos_q_len, NULL, NULL, NULL, NULL,
        asteroids, v2(0.0f, 0.0f), sent,
        motion_sent_tick, motion_sent_pos, motion_sent_vel,
        100u + ASTEROID_NET_MOTION_HEARTBEAT_TICKS);

    ASSERT_EQ_INT(full_len, ASTEROID_MSG_HEADER);
    ASSERT_EQ_INT(motion_q[1] | (motion_q[2] << 8), 1);
    ASSERT_EQ_INT(motion_q_len,
                  ASTEROID_MOTION_Q_MSG_HEADER + ASTEROID_MOTION_Q_RECORD_SIZE);
    ASSERT_EQ_INT(pos_q[1] | (pos_q[2] << 8), 0);
    ASSERT_EQ_INT(pos_q_len, ASTEROID_POS_Q_MSG_HEADER);
    ASSERT_EQ_FLOAT(motion_sent_vel[6].x, 24.0f, 0.01f);
}

TEST(test_asteroid_delta_uses_quantized_motion_stream_for_far_repeat) {
    asteroid_t asteroids[MAX_ASTEROIDS];
    memset(asteroids, 0, sizeof(asteroids));
    bool sent[MAX_ASTEROIDS] = {0};
    uint32_t motion_sent_tick[MAX_ASTEROIDS] = {0};
    vec2 motion_sent_pos[MAX_ASTEROIDS] = {0};
    vec2 motion_sent_vel[MAX_ASTEROIDS] = {0};

    asteroids[8].active = true;
    asteroids[8].pos = v2(2350.0f, -120.0f);
    asteroids[8].radius = 10.0f;
    asteroids[8].hp = 10.0f;
    asteroids[8].vel = v2(12.5f, -3.0f);
    sent[8] = true;
    motion_sent_tick[8] = 100u;
    motion_sent_pos[8] = v2(2090.0f, -120.0f);
    motion_sent_vel[8] = asteroids[8].vel;

    uint8_t full[ASTEROID_MSG_HEADER + MAX_ASTEROIDS * ASTEROID_RECORD_SIZE];
    uint8_t motion[ASTEROID_MOTION_MSG_HEADER +
                   MAX_ASTEROIDS * ASTEROID_MOTION_RECORD_SIZE];
    uint8_t motion_q[ASTEROID_MOTION_Q_MSG_HEADER +
                     MAX_ASTEROIDS * ASTEROID_MOTION_Q_RECORD_SIZE];
    int motion_len = 0;
    int motion_q_len = 0;
    int full_len = serialize_asteroids_for_player_split_at_tick(
        full, motion, &motion_len, motion_q, &motion_q_len,
        NULL, NULL,
        asteroids, v2(0.0f, 0.0f), sent,
        motion_sent_tick, motion_sent_pos, motion_sent_vel,
        100u + ASTEROID_NET_VERY_FAR_SLOW_MOVING_REPEAT_TICKS);

    ASSERT_EQ_INT(full[0], NET_MSG_WORLD_ASTEROIDS);
    ASSERT_EQ_INT(full[1] | (full[2] << 8), 0);
    ASSERT_EQ_INT(full_len, ASTEROID_MSG_HEADER);
    ASSERT_EQ_INT(motion[0], NET_MSG_WORLD_ASTEROID_MOTION);
    ASSERT_EQ_INT(motion[1] | (motion[2] << 8), 0);
    ASSERT_EQ_INT(motion_len, ASTEROID_MOTION_MSG_HEADER);

    ASSERT_EQ_INT(motion_q[0], NET_MSG_WORLD_ASTEROID_MOTION_Q);
    ASSERT_EQ_INT(motion_q[1] | (motion_q[2] << 8), 1);
    ASSERT_EQ_INT(motion_q_len,
                  ASTEROID_MOTION_Q_MSG_HEADER + ASTEROID_MOTION_Q_RECORD_SIZE);

    const uint8_t *p = &motion_q[ASTEROID_MOTION_Q_MSG_HEADER];
    ASSERT_EQ_INT(p[0] | (p[1] << 8), 8);
    ASSERT_EQ_INT((int)(int16_t)(p[2] | ((uint16_t)p[3] << 8)), 588);
    ASSERT_EQ_INT((int)(int16_t)(p[4] | ((uint16_t)p[5] << 8)), -30);
    ASSERT_EQ_INT((int)(int16_t)(p[6] | ((uint16_t)p[7] << 8)), 50);
    ASSERT_EQ_INT((int)(int16_t)(p[8] | ((uint16_t)p[9] << 8)), -12);
    ASSERT_EQ_INT((int)motion_sent_tick[8],
                  100 + ASTEROID_NET_VERY_FAR_SLOW_MOVING_REPEAT_TICKS);
}

TEST(test_asteroid_delta_uses_quantized_motion_stream_for_settling) {
    asteroid_t asteroids[MAX_ASTEROIDS];
    memset(asteroids, 0, sizeof(asteroids));
    bool sent[MAX_ASTEROIDS] = {0};
    uint32_t motion_sent_tick[MAX_ASTEROIDS] = {0};
    vec2 motion_sent_pos[MAX_ASTEROIDS] = {0};
    vec2 motion_sent_vel[MAX_ASTEROIDS] = {0};

    asteroids[6].active = true;
    asteroids[6].pos = v2(44.0f, 52.0f);
    asteroids[6].radius = 10.0f;
    asteroids[6].hp = 10.0f;
    asteroids[6].vel = v2(0.0f, 0.0f);
    sent[6] = true;
    motion_sent_tick[6] = 124u;
    motion_sent_pos[6] = v2(40.0f, 48.0f);
    motion_sent_vel[6] = v2(0.25f, -0.25f);

    uint8_t full[ASTEROID_MSG_HEADER + MAX_ASTEROIDS * ASTEROID_RECORD_SIZE];
    uint8_t motion[ASTEROID_MOTION_MSG_HEADER +
                   MAX_ASTEROIDS * ASTEROID_MOTION_RECORD_SIZE];
    uint8_t motion_q[ASTEROID_MOTION_Q_MSG_HEADER +
                     MAX_ASTEROIDS * ASTEROID_MOTION_Q_RECORD_SIZE];
    int motion_len = 0;
    int motion_q_len = 0;
    int full_len = serialize_asteroids_for_player_split_at_tick(
        full, motion, &motion_len, motion_q, &motion_q_len,
        NULL, NULL,
        asteroids, v2(0.0f, 0.0f), sent,
        motion_sent_tick, motion_sent_pos, motion_sent_vel, 125u);

    ASSERT_EQ_INT(full[0], NET_MSG_WORLD_ASTEROIDS);
    ASSERT_EQ_INT(full[1] | (full[2] << 8), 0);
    ASSERT_EQ_INT(full_len, ASTEROID_MSG_HEADER);
    ASSERT_EQ_INT(motion[0], NET_MSG_WORLD_ASTEROID_MOTION);
    ASSERT_EQ_INT(motion[1] | (motion[2] << 8), 0);
    ASSERT_EQ_INT(motion_len, ASTEROID_MOTION_MSG_HEADER);

    ASSERT_EQ_INT(motion_q[0], NET_MSG_WORLD_ASTEROID_MOTION_Q);
    ASSERT_EQ_INT(motion_q[1] | (motion_q[2] << 8), 1);
    ASSERT_EQ_INT(motion_q_len,
                  ASTEROID_MOTION_Q_MSG_HEADER + ASTEROID_MOTION_Q_RECORD_SIZE);

    const uint8_t *p = &motion_q[ASTEROID_MOTION_Q_MSG_HEADER];
    ASSERT_EQ_INT(p[0] | (p[1] << 8), 6);
    ASSERT_EQ_INT((int)(int16_t)(p[2] | ((uint16_t)p[3] << 8)), 11);
    ASSERT_EQ_INT((int)(int16_t)(p[4] | ((uint16_t)p[5] << 8)), 13);
    ASSERT_EQ_INT((int)(int16_t)(p[6] | ((uint16_t)p[7] << 8)), 0);
    ASSERT_EQ_INT((int)(int16_t)(p[8] | ((uint16_t)p[9] << 8)), 0);
    ASSERT_EQ_INT((int)motion_sent_tick[6], 0);
}

TEST(test_asteroid_delta_uses_compact_state_stream_for_known_dirty) {
    asteroid_t asteroids[MAX_ASTEROIDS];
    memset(asteroids, 0, sizeof(asteroids));
    bool sent[MAX_ASTEROIDS] = {0};
    uint32_t motion_sent_tick[MAX_ASTEROIDS] = {0};
    vec2 motion_sent_pos[MAX_ASTEROIDS] = {0};
    vec2 motion_sent_vel[MAX_ASTEROIDS] = {0};

    asteroids[7].active = true;
    asteroids[7].net_dirty = true;
    asteroids[7].pos = v2(80.0f, 90.0f);
    asteroids[7].radius = 14.5f;
    asteroids[7].hp = 6.25f;
    asteroids[7].ore = 2.5f;
    asteroids[7].smelt_progress = 0.5f;
    asteroids[7].grade = MINING_GRADE_RARE;
    asteroids[7].crystal_stage = CRYSTAL_STAGE_INTERMEDIATE;
    asteroids[7].phase = ASTEROID_PHASE_GAS_RICH;
    sent[7] = true;

    uint8_t full[ASTEROID_MSG_HEADER + MAX_ASTEROIDS * ASTEROID_RECORD_SIZE];
    uint8_t motion[ASTEROID_MOTION_MSG_HEADER +
                   MAX_ASTEROIDS * ASTEROID_MOTION_RECORD_SIZE];
    uint8_t motion_q[ASTEROID_MOTION_Q_MSG_HEADER +
                     MAX_ASTEROIDS * ASTEROID_MOTION_Q_RECORD_SIZE];
    uint8_t state_q[ASTEROID_STATE_Q_MSG_HEADER +
                    MAX_ASTEROIDS * ASTEROID_STATE_Q_RECORD_SIZE];
    int motion_len = 0;
    int motion_q_len = 0;
    int state_q_len = 0;
    int full_len = serialize_asteroids_for_player_split_at_tick(
        full, motion, &motion_len, motion_q, &motion_q_len,
        state_q, &state_q_len, asteroids, v2(0.0f, 0.0f), sent,
        motion_sent_tick, motion_sent_pos, motion_sent_vel, 125u);

    ASSERT_EQ_INT(full[0], NET_MSG_WORLD_ASTEROIDS);
    ASSERT_EQ_INT(full[1] | (full[2] << 8), 0);
    ASSERT_EQ_INT(full_len, ASTEROID_MSG_HEADER);
    ASSERT_EQ_INT(motion[0], NET_MSG_WORLD_ASTEROID_MOTION);
    ASSERT_EQ_INT(motion[1] | (motion[2] << 8), 0);
    ASSERT_EQ_INT(motion_len, ASTEROID_MOTION_MSG_HEADER);
    ASSERT_EQ_INT(motion_q[0], NET_MSG_WORLD_ASTEROID_MOTION_Q);
    ASSERT_EQ_INT(motion_q[1] | (motion_q[2] << 8), 0);
    ASSERT_EQ_INT(motion_q_len, ASTEROID_MOTION_Q_MSG_HEADER);

    ASSERT_EQ_INT(state_q[0], NET_MSG_WORLD_ASTEROID_STATE_Q);
    ASSERT_EQ_INT(state_q[1] | (state_q[2] << 8), 1);
    ASSERT_EQ_INT(state_q_len,
                  ASTEROID_STATE_Q_MSG_HEADER + ASTEROID_STATE_Q_RECORD_SIZE);
    const uint8_t *p = &state_q[ASTEROID_STATE_Q_MSG_HEADER];
    ASSERT_EQ_INT(p[0] | (p[1] << 8), 7);
    ASSERT_EQ_FLOAT(read_f32_le(&p[2]), 6.25f, 0.01f);
    ASSERT_EQ_FLOAT(read_f32_le(&p[6]), 2.5f, 0.01f);
    ASSERT_EQ_FLOAT(read_f32_le(&p[10]), 14.5f, 0.01f);
    ASSERT_EQ_INT(p[14], 127);
    ASSERT_EQ_INT(p[15], MINING_GRADE_RARE);
    ASSERT_EQ_INT(p[16], CRYSTAL_STAGE_INTERMEDIATE);
    ASSERT_EQ_INT(p[17], ASTEROID_PHASE_GAS_RICH);
}

TEST(test_asteroid_identity_change_forces_full_upsert) {
    asteroid_t asteroids[MAX_ASTEROIDS];
    memset(asteroids, 0, sizeof(asteroids));
    bool sent[MAX_ASTEROIDS] = {0};
    uint32_t identity_sent_sig[MAX_ASTEROIDS] = {0};
    uint32_t state_sent_tick[MAX_ASTEROIDS] = {0};
    uint32_t state_sent_sig[MAX_ASTEROIDS] = {0};
    uint32_t state_sent_semantic_sig[MAX_ASTEROIDS] = {0};

    asteroid_t parent = {0};
    parent.active = true;
    parent.fracture_child = false;
    parent.tier = ASTEROID_TIER_M;
    parent.commodity = COMMODITY_FERRITE_ORE;

    asteroids[7].active = true;
    asteroids[7].net_dirty = true;
    asteroids[7].fracture_child = true;
    asteroids[7].tier = ASTEROID_TIER_S;
    asteroids[7].commodity = COMMODITY_FERRITE_ORE;
    asteroids[7].pos = v2(80.0f, 90.0f);
    asteroids[7].vel = v2(8.0f, -3.0f);
    asteroids[7].radius = 12.0f;
    asteroids[7].hp = 18.0f;
    asteroids[7].ore = 10.0f;
    sent[7] = true;
    identity_sent_sig[7] = asteroid_identity_signature(&parent);
    state_sent_tick[7] = 100u;
    state_sent_sig[7] = 1234u;
    state_sent_semantic_sig[7] = 5678u;

    uint8_t full[ASTEROID_MSG_HEADER + MAX_ASTEROIDS * ASTEROID_RECORD_SIZE];
    uint8_t motion[ASTEROID_MOTION_MSG_HEADER +
                   MAX_ASTEROIDS * ASTEROID_MOTION_RECORD_SIZE];
    uint8_t motion_q[ASTEROID_MOTION_Q_MSG_HEADER +
                     MAX_ASTEROIDS * ASTEROID_MOTION_Q_RECORD_SIZE];
    uint8_t state_q[ASTEROID_STATE_Q_MSG_HEADER +
                    MAX_ASTEROIDS * ASTEROID_STATE_Q_RECORD_SIZE];
    int motion_len = 0;
    int motion_q_len = 0;
    int state_q_len = 0;

    int full_len = serialize_asteroids_for_player_split_ext_state_budget_at_tick(
        full, NULL, NULL, NULL, NULL,
        motion, &motion_len, motion_q, &motion_q_len,
        NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
        state_q, &state_q_len, NULL, NULL,
        asteroids, v2(0.0f, 0.0f), sent,
        NULL, NULL, NULL, identity_sent_sig,
        state_sent_tick, state_sent_sig, state_sent_semantic_sig,
        101u, -1);

    ASSERT_EQ_INT(full_len, ASTEROID_MSG_HEADER + ASTEROID_RECORD_SIZE);
    ASSERT_EQ_INT(full[1] | (full[2] << 8), 1);
    ASSERT_EQ_INT(state_q[1] | (state_q[2] << 8), 0);
    ASSERT_EQ_INT(state_q_len, ASTEROID_STATE_Q_MSG_HEADER);

    const uint8_t *p = &full[ASTEROID_MSG_HEADER];
    ASSERT_EQ_INT(p[0] | (p[1] << 8), 7);
    ASSERT(p[2] & 1);
    ASSERT(p[2] & (1 << 1));
    ASSERT_EQ_INT((p[2] >> 2) & 0x7, ASTEROID_TIER_S);
    ASSERT_EQ_FLOAT(read_f32_le(&p[19]), 18.0f, 0.01f);
    ASSERT_EQ_FLOAT(read_f32_le(&p[23]), 10.0f, 0.01f);
    ASSERT_EQ_INT((int)identity_sent_sig[7],
                  (int)asteroid_identity_signature(&asteroids[7]));
    ASSERT_EQ_INT((int)state_sent_tick[7], 101);
}

TEST(test_asteroid_cache_invalidation_preserves_pending_removal) {
    static server_player_t sp;
    static server_replication_t replication;
    static asteroid_t asteroids[MAX_ASTEROIDS];
    static uint8_t full[ASTEROID_MSG_HEADER +
                        MAX_ASTEROIDS * ASTEROID_RECORD_SIZE];
    static uint8_t remove_buf[ASTEROID_REMOVE_MSG_HEADER +
                              MAX_ASTEROIDS * ASTEROID_REMOVE_RECORD_SIZE];
    memset(&sp, 0, sizeof(sp));
    memset(&replication, 0, sizeof(replication));
    sp.replication = &replication;
    memset(asteroids, 0, sizeof(asteroids));

    sp.replication->asteroid_sent[7] = true;
    sp.replication->asteroid_motion_sent_tick[7] = 90u;
    sp.replication->asteroid_motion_sent_pos[7] = v2(10.0f, 20.0f);
    sp.replication->asteroid_motion_sent_vel[7] = v2(3.0f, 4.0f);
    sp.replication->asteroid_identity_sent_sig[7] = 123u;
    sp.replication->asteroid_state_sent_tick[7] = 90u;
    sp.replication->asteroid_state_sent_sig[7] = 456u;
    sp.replication->asteroid_state_sent_semantic_sig[7] = 789u;

    server_player_invalidate_asteroid_stream_caches(&sp);

    ASSERT(sp.replication->asteroid_sent[7]);
    ASSERT_EQ_INT((int)sp.replication->asteroid_motion_sent_tick[7], 0);
    ASSERT_EQ_INT((int)sp.replication->asteroid_identity_sent_sig[7], 0);
    ASSERT_EQ_INT((int)sp.replication->asteroid_state_sent_tick[7], 0);

    int remove_len = 0;
    int full_len = serialize_asteroids_for_player_split_ext_state_budget_at_tick(
        full, NULL, NULL, NULL, NULL,
        NULL, NULL, NULL, NULL,
        NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
        NULL, NULL, remove_buf, &remove_len,
        asteroids, v2(0.0f, 0.0f), sp.replication->asteroid_sent,
        sp.replication->asteroid_motion_sent_tick, sp.replication->asteroid_motion_sent_pos,
        sp.replication->asteroid_motion_sent_vel, sp.replication->asteroid_identity_sent_sig,
        sp.replication->asteroid_state_sent_tick, sp.replication->asteroid_state_sent_sig,
        sp.replication->asteroid_state_sent_semantic_sig, 100u, -1);

    ASSERT_EQ_INT(full_len, ASTEROID_MSG_HEADER);
    ASSERT_EQ_INT(remove_len,
                  ASTEROID_REMOVE_MSG_HEADER + ASTEROID_REMOVE_RECORD_SIZE);
    ASSERT_EQ_INT(remove_buf[0], NET_MSG_WORLD_ASTEROID_REMOVE);
    ASSERT_EQ_INT(remove_buf[1] | (remove_buf[2] << 8), 1);
    ASSERT_EQ_INT(remove_buf[3] | (remove_buf[4] << 8), 7);
    ASSERT(!sp.replication->asteroid_sent[7]);
}

TEST(test_asteroid_delta_coalesces_dirty_state_stream_per_player) {
    ASSERT_EQ_INT((int)ASTEROID_STATE_Q_NUMERIC_REPEAT_TICKS, 240);
    ASSERT_EQ_INT((int)ASTEROID_STATE_Q_HEARTBEAT_TICKS, 960);

    asteroid_t asteroids[MAX_ASTEROIDS];
    memset(asteroids, 0, sizeof(asteroids));
    bool sent[MAX_ASTEROIDS] = {0};
    uint32_t motion_sent_tick[MAX_ASTEROIDS] = {0};
    vec2 motion_sent_pos[MAX_ASTEROIDS] = {0};
    vec2 motion_sent_vel[MAX_ASTEROIDS] = {0};
    uint32_t state_sent_tick[MAX_ASTEROIDS] = {0};
    uint32_t state_sent_sig[MAX_ASTEROIDS] = {0};
    uint32_t state_sent_semantic_sig[MAX_ASTEROIDS] = {0};

    asteroids[7].active = true;
    asteroids[7].net_dirty = true;
    asteroids[7].pos = v2(80.0f, 90.0f);
    asteroids[7].radius = 14.5f;
    asteroids[7].hp = 6.25f;
    asteroids[7].ore = 2.5f;
    asteroids[7].smelt_progress = 0.5f;
    asteroids[7].grade = MINING_GRADE_RARE;
    asteroids[7].crystal_stage = CRYSTAL_STAGE_INTERMEDIATE;
    asteroids[7].phase = ASTEROID_PHASE_GAS_RICH;
    sent[7] = true;

    uint8_t full[ASTEROID_MSG_HEADER + MAX_ASTEROIDS * ASTEROID_RECORD_SIZE];
    uint8_t motion[ASTEROID_MOTION_MSG_HEADER +
                   MAX_ASTEROIDS * ASTEROID_MOTION_RECORD_SIZE];
    uint8_t motion_q[ASTEROID_MOTION_Q_MSG_HEADER +
                     MAX_ASTEROIDS * ASTEROID_MOTION_Q_RECORD_SIZE];
    uint8_t state_q[ASTEROID_STATE_Q_MSG_HEADER +
                    MAX_ASTEROIDS * ASTEROID_STATE_Q_RECORD_SIZE];
    int motion_len = 0;
    int motion_q_len = 0;
    int state_q_len = 0;
    int full_len = serialize_asteroids_for_player_split_ext_state_at_tick(
        full, NULL, NULL, NULL, NULL,
        motion, &motion_len, motion_q, &motion_q_len,
        NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
        state_q, &state_q_len, NULL, NULL,
        asteroids, v2(0.0f, 0.0f), sent,
        motion_sent_tick, motion_sent_pos, motion_sent_vel,
        state_sent_tick, state_sent_sig, state_sent_semantic_sig, 100u);

    ASSERT_EQ_INT(full_len, ASTEROID_MSG_HEADER);
    ASSERT_EQ_INT(state_q[1] | (state_q[2] << 8), 1);
    ASSERT_EQ_INT(state_q_len,
                  ASTEROID_STATE_Q_MSG_HEADER + ASTEROID_STATE_Q_RECORD_SIZE);
    ASSERT_EQ_INT((int)state_sent_tick[7], 100);
    ASSERT(state_sent_sig[7] != 0u);
    ASSERT(state_sent_semantic_sig[7] != 0u);

    asteroids[7].hp = 8.25f;
    asteroids[7].ore = 4.5f;
    asteroids[7].smelt_progress = 0.75f;
    full_len = serialize_asteroids_for_player_split_ext_state_at_tick(
        full, NULL, NULL, NULL, NULL,
        motion, &motion_len, motion_q, &motion_q_len,
        NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
        state_q, &state_q_len, NULL, NULL,
        asteroids, v2(0.0f, 0.0f), sent,
        motion_sent_tick, motion_sent_pos, motion_sent_vel,
        state_sent_tick, state_sent_sig, state_sent_semantic_sig,
        100u + ASTEROID_STATE_Q_NUMERIC_REPEAT_TICKS - 1u);

    ASSERT_EQ_INT(full_len, ASTEROID_MSG_HEADER);
    ASSERT_EQ_INT(state_q[1] | (state_q[2] << 8), 0);
    ASSERT_EQ_INT(state_q_len, ASTEROID_STATE_Q_MSG_HEADER);
    ASSERT_EQ_INT((int)state_sent_tick[7], 100);

    full_len = serialize_asteroids_for_player_split_ext_state_at_tick(
        full, NULL, NULL, NULL, NULL,
        motion, &motion_len, motion_q, &motion_q_len,
        NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
        state_q, &state_q_len, NULL, NULL,
        asteroids, v2(0.0f, 0.0f), sent,
        motion_sent_tick, motion_sent_pos, motion_sent_vel,
        state_sent_tick, state_sent_sig, state_sent_semantic_sig,
        100u + ASTEROID_STATE_Q_NUMERIC_REPEAT_TICKS);

    ASSERT_EQ_INT(full_len, ASTEROID_MSG_HEADER);
    ASSERT_EQ_INT(state_q[1] | (state_q[2] << 8), 1);
    ASSERT_EQ_INT((int)state_sent_tick[7],
                  100 + ASTEROID_STATE_Q_NUMERIC_REPEAT_TICKS);
    const uint8_t *p = &state_q[ASTEROID_STATE_Q_MSG_HEADER];
    ASSERT_EQ_FLOAT(read_f32_le(&p[2]), 8.25f, 0.01f);
    ASSERT_EQ_FLOAT(read_f32_le(&p[6]), 4.5f, 0.01f);
    ASSERT_EQ_INT(p[14], 191);

    uint32_t numeric_tick = state_sent_tick[7];
    asteroids[7].crystal_stage = CRYSTAL_STAGE_RAW;
    full_len = serialize_asteroids_for_player_split_ext_state_at_tick(
        full, NULL, NULL, NULL, NULL,
        motion, &motion_len, motion_q, &motion_q_len,
        NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
        state_q, &state_q_len, NULL, NULL,
        asteroids, v2(0.0f, 0.0f), sent,
        motion_sent_tick, motion_sent_pos, motion_sent_vel,
        state_sent_tick, state_sent_sig, state_sent_semantic_sig,
        numeric_tick + 1u);

    ASSERT_EQ_INT(full_len, ASTEROID_MSG_HEADER);
    ASSERT_EQ_INT(state_q[1] | (state_q[2] << 8), 1);
    ASSERT_EQ_INT((int)state_sent_tick[7], (int)(numeric_tick + 1u));
    p = &state_q[ASTEROID_STATE_Q_MSG_HEADER];
    ASSERT_EQ_INT(p[16], CRYSTAL_STAGE_RAW);

    asteroids[7].active = false;
    full_len = serialize_asteroids_for_player_split_ext_state_at_tick(
        full, NULL, NULL, NULL, NULL,
        motion, &motion_len, motion_q, &motion_q_len,
        NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
        state_q, &state_q_len, NULL, NULL,
        asteroids, v2(0.0f, 0.0f), sent,
        motion_sent_tick, motion_sent_pos, motion_sent_vel,
        state_sent_tick, state_sent_sig, state_sent_semantic_sig,
        numeric_tick + 2u);

    ASSERT_EQ_INT(full_len, ASTEROID_MSG_HEADER + ASTEROID_RECORD_SIZE);
    ASSERT_EQ_INT(full[1] | (full[2] << 8), 1);
    ASSERT(!sent[7]);
    ASSERT_EQ_INT((int)state_sent_tick[7], 0);
    ASSERT_EQ_INT((int)state_sent_sig[7], 0);
    ASSERT_EQ_INT((int)state_sent_semantic_sig[7], 0);
}

TEST(test_asteroid_delta_sends_inactive_removal) {
    asteroid_t asteroids[MAX_ASTEROIDS];
    memset(asteroids, 0, sizeof(asteroids));
    bool sent[MAX_ASTEROIDS] = {0};
    sent[7] = true;

    uint8_t buf[ASTEROID_MSG_HEADER + MAX_ASTEROIDS * ASTEROID_RECORD_SIZE];
    int len = serialize_asteroids_for_player(buf, asteroids, v2(0.0f, 0.0f), sent);

    ASSERT_EQ_INT(buf[0], NET_MSG_WORLD_ASTEROIDS);
    ASSERT_EQ_INT(buf[1] | (buf[2] << 8), 1);
    ASSERT_EQ_INT(len, ASTEROID_MSG_HEADER + ASTEROID_RECORD_SIZE);
    uint8_t *p = &buf[ASTEROID_MSG_HEADER];
    ASSERT_EQ_INT(p[0] | (p[1] << 8), 7);
    ASSERT_EQ_INT(p[2] & 1, 0);
    ASSERT(!sent[7]);
}

TEST(test_asteroid_delta_uses_compact_removal_stream_when_available) {
    asteroid_t asteroids[MAX_ASTEROIDS];
    memset(asteroids, 0, sizeof(asteroids));
    bool sent[MAX_ASTEROIDS] = {0};
    sent[7] = true;

    uint32_t motion_sent_tick[MAX_ASTEROIDS] = {0};
    vec2 motion_sent_pos[MAX_ASTEROIDS];
    vec2 motion_sent_vel[MAX_ASTEROIDS];
    uint32_t state_sent_tick[MAX_ASTEROIDS] = {0};
    uint32_t state_sent_sig[MAX_ASTEROIDS] = {0};
    uint32_t state_sent_semantic_sig[MAX_ASTEROIDS] = {0};
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        motion_sent_pos[i] = v2(0.0f, 0.0f);
        motion_sent_vel[i] = v2(0.0f, 0.0f);
    }
    motion_sent_tick[7] = 100u;
    motion_sent_pos[7] = v2(10.0f, 20.0f);
    motion_sent_vel[7] = v2(1.0f, 2.0f);
    state_sent_tick[7] = 100u;
    state_sent_sig[7] = 123u;
    state_sent_semantic_sig[7] = 456u;

    uint8_t full[ASTEROID_MSG_HEADER + MAX_ASTEROIDS * ASTEROID_RECORD_SIZE];
    uint8_t motion[ASTEROID_MOTION_MSG_HEADER +
                   MAX_ASTEROIDS * ASTEROID_MOTION_RECORD_SIZE];
    uint8_t motion_q[ASTEROID_MOTION_Q_MSG_HEADER +
                     MAX_ASTEROIDS * ASTEROID_MOTION_Q_RECORD_SIZE];
    uint8_t pos_q[ASTEROID_POS_Q_MSG_HEADER +
                  MAX_ASTEROIDS * ASTEROID_POS_Q_RECORD_SIZE];
    uint8_t state_q[ASTEROID_STATE_Q_MSG_HEADER +
                    MAX_ASTEROIDS * ASTEROID_STATE_Q_RECORD_SIZE];
    uint8_t remove[ASTEROID_REMOVE_MSG_HEADER +
                   MAX_ASTEROIDS * ASTEROID_REMOVE_RECORD_SIZE];
    int motion_len = 0;
    int motion_q_len = 0;
    int pos_q_len = 0;
    int state_q_len = 0;
    int remove_len = 0;
    int full_len = serialize_asteroids_for_player_split_ext_state_at_tick(
        full, NULL, NULL, NULL, NULL,
        motion, &motion_len, motion_q, &motion_q_len,
        NULL, NULL, NULL, NULL,
        pos_q, &pos_q_len, NULL, NULL, state_q, &state_q_len,
        remove, &remove_len,
        asteroids, v2(0.0f, 0.0f), sent,
        motion_sent_tick, motion_sent_pos, motion_sent_vel,
        state_sent_tick, state_sent_sig, state_sent_semantic_sig, 120u);

    ASSERT_EQ_INT(full[0], NET_MSG_WORLD_ASTEROIDS);
    ASSERT_EQ_INT(full[1] | (full[2] << 8), 0);
    ASSERT_EQ_INT(full_len, ASTEROID_MSG_HEADER);
    ASSERT_EQ_INT(remove[0], NET_MSG_WORLD_ASTEROID_REMOVE);
    ASSERT_EQ_INT(remove[1] | (remove[2] << 8), 1);
    ASSERT_EQ_INT(remove_len,
                  ASTEROID_REMOVE_MSG_HEADER + ASTEROID_REMOVE_RECORD_SIZE);
    ASSERT_EQ_INT(remove[ASTEROID_REMOVE_MSG_HEADER] |
                  (remove[ASTEROID_REMOVE_MSG_HEADER + 1] << 8), 7);
    ASSERT_EQ_INT(motion_len, ASTEROID_MOTION_MSG_HEADER);
    ASSERT_EQ_INT(motion_q_len, ASTEROID_MOTION_Q_MSG_HEADER);
    ASSERT_EQ_INT(pos_q_len, ASTEROID_POS_Q_MSG_HEADER);
    ASSERT_EQ_INT(state_q_len, ASTEROID_STATE_Q_MSG_HEADER);
    ASSERT(!sent[7]);
    ASSERT_EQ_INT((int)motion_sent_tick[7], 0);
    ASSERT_EQ_INT((int)state_sent_tick[7], 0);
    ASSERT_EQ_INT((int)state_sent_sig[7], 0);
    ASSERT_EQ_INT((int)state_sent_semantic_sig[7], 0);
}

TEST(test_roundtrip_asteroids_full_skips_inactive_slots) {
    asteroid_t asteroids[MAX_ASTEROIDS];
    memset(asteroids, 0, sizeof(asteroids));

    /* Join-time full sync is active-only; the client clears local asteroid
     * buffers before entering remote-authoritative mode. */
    asteroids[0].active = true;
    asteroids[0].tier = ASTEROID_TIER_L;
    asteroids[0].commodity = COMMODITY_CUPRITE_ORE;
    asteroids[0].pos = v2(42.0f, -9.0f);
    asteroids[0].hp = 77.0f;
    asteroids[0].radius = 33.0f;

    asteroids[5].active = true;
    asteroids[5].fracture_child = true;
    asteroids[5].tier = ASTEROID_TIER_M;
    asteroids[5].commodity = COMMODITY_CRYSTAL_ORE;
    asteroids[5].pos = v2(-12.0f, 88.0f);
    asteroids[5].ore = 11.0f;
    asteroids[5].radius = 21.0f;
    asteroids[5].crystal_stage = CRYSTAL_STAGE_INTERMEDIATE;
    asteroids[5].phase = ASTEROID_PHASE_GAS_RICH;

    uint8_t *buf = calloc(1, ASTEROID_MSG_HEADER + MAX_ASTEROIDS * ASTEROID_RECORD_SIZE);
    int len = serialize_asteroids_full(buf, asteroids);

    ASSERT_EQ_INT(buf[0], NET_MSG_WORLD_ASTEROIDS);
    int full_count = buf[1] | (buf[2] << 8);
    ASSERT_EQ_INT(full_count, 2);  /* only active slots sent */
    ASSERT_EQ_INT(len, ASTEROID_MSG_HEADER + 2 * ASTEROID_RECORD_SIZE);

    /* First active slot (index 0) */
    uint8_t *p0 = &buf[ASTEROID_MSG_HEADER];
    ASSERT_EQ_INT(p0[0] | (p0[1] << 8), 0);
    ASSERT(p0[2] & 1);
    ASSERT_EQ_INT((p0[2] >> 2) & 0x7, ASTEROID_TIER_L);
    ASSERT_EQ_FLOAT(read_f32_le(&p0[3]), 42.0f, 0.1f);
    ASSERT_EQ_FLOAT(read_f32_le(&p0[19]), 77.0f, 0.1f);

    /* Inactive slots are skipped in full snapshot (too many at 2048).
     * Second record should be the other active slot (index 5). */
    uint8_t *p5 = &buf[ASTEROID_MSG_HEADER + ASTEROID_RECORD_SIZE];
    ASSERT_EQ_INT(p5[0] | (p5[1] << 8), 5);
    ASSERT(p5[2] & 1);
    ASSERT(p5[2] & 2);
    ASSERT_EQ_INT((p5[2] >> 2) & 0x7, ASTEROID_TIER_M);
    ASSERT_EQ_FLOAT(read_f32_le(&p5[23]), 11.0f, 0.1f);
    ASSERT_EQ_FLOAT(read_f32_le(&p5[27]), 21.0f, 0.1f);
    ASSERT_EQ_INT(p5[33], CRYSTAL_STAGE_INTERMEDIATE);
    ASSERT_EQ_INT(p5[34], ASTEROID_PHASE_GAS_RICH);
    free(buf);
}

TEST(test_roundtrip_cargo_pods) {
    cargo_pod_t pods[MAX_CARGO_PODS];
    memset(pods, 0, sizeof(pods));
    pods[3].active = true;
    pods[3].kind = CARGO_POD_CARGO;
    pods[3].commodity = COMMODITY_REPAIR_KIT;
    pods[3].quantity = 20;
    pods[3].manifest_count = 20;
    for (uint16_t i = 0; i < pods[3].manifest_count; i++) {
        pods[3].manifest_units[i].commodity = COMMODITY_REPAIR_KIT;
        pods[3].manifest_units[i].grade = MINING_GRADE_RARE;
    }
    pods[3].pos = v2(123.0f, -45.0f);
    pods[3].vel = v2(1.5f, -2.0f);
    pods[3].radius = 18.0f;
    pods[3].rotation = 0.75f;
    cargo_pod_set_player_tractor(&pods[3], 2);
    pods[4].active = true;
    pods[4].kind = CARGO_POD_CARGO;
    pods[4].commodity = COMMODITY_FRAME;
    pods[4].quantity = 5;
    pods[4].manifest_count = 5;
    pods[4].shipment_id = 77;
    for (uint16_t i = 0; i < pods[4].manifest_count; i++)
        pods[4].manifest_units[i].commodity = COMMODITY_FRAME;
    pods[4].pos = v2(7.0f, 8.0f);
    cargo_pod_clear_tractor(&pods[4]);
    cargo_pod_set_module_tractor(&pods[4], 2, 5);

    uint8_t buf[2 + MAX_CARGO_PODS * CARGO_POD_RECORD_SIZE];
    int len = serialize_cargo_pods(buf, pods);

    ASSERT_EQ_INT(buf[0], NET_MSG_WORLD_CARGO_PODS);
    ASSERT_EQ_INT(buf[1], 2);
    ASSERT_EQ_INT(len, 2 + 2 * CARGO_POD_RECORD_SIZE);
    uint8_t *p = &buf[2];
    ASSERT_EQ_INT(p[0], 3);
    ASSERT_EQ_INT(p[1], CARGO_POD_CARGO);
    ASSERT_EQ_INT(p[2], COMMODITY_REPAIR_KIT);
    ASSERT_EQ_INT(p[3], 2);
    ASSERT_EQ_FLOAT(read_f32_le(&p[4]), 123.0f, 0.1f);
    ASSERT_EQ_FLOAT(read_f32_le(&p[8]), -45.0f, 0.1f);
    ASSERT_EQ_FLOAT(read_f32_le(&p[20]), 18.0f, 0.1f);
    ASSERT_EQ_FLOAT(read_f32_le(&p[24]), 0.75f, 0.01f);
    ASSERT_EQ_INT(read_u16_le(&p[28]), 20);
    ASSERT_EQ_INT(read_u16_le(&p[30]), 20);
    ASSERT_EQ_INT(read_u16_le(&p[32]), 0);
    ASSERT(p[34] & CARGO_POD_SUMMARY_EXACT_MATERIAL);
    ASSERT(!(p[34] & CARGO_POD_SUMMARY_SHIPMENT_BOUND));
    ASSERT_EQ_INT(p[35], MINING_GRADE_RARE);

    p = &buf[2 + CARGO_POD_RECORD_SIZE];
    ASSERT_EQ_INT(p[0], 4);
    ASSERT_EQ_INT(read_u16_le(&p[28]), 5);
    ASSERT_EQ_INT(read_u16_le(&p[30]), 5);
    ASSERT_EQ_INT(read_u16_le(&p[32]), 77);
    ASSERT(!(p[34] & CARGO_POD_SUMMARY_EXACT_MATERIAL));
    ASSERT(p[34] & CARGO_POD_SUMMARY_SHIPMENT_BOUND);
    ASSERT_EQ_INT(p[35], MINING_GRADE_COMMON);
    ASSERT_EQ_INT(p[36], 3);
    ASSERT_EQ_INT(p[37], 6);
}

TEST(test_roundtrip_cargo_pods_q_quantizes_visual_pose) {
    cargo_pod_t pods[MAX_CARGO_PODS];
    memset(pods, 0, sizeof(pods));
    pods[3].active = true;
    pods[3].kind = CARGO_POD_CARGO;
    pods[3].commodity = COMMODITY_REPAIR_KIT;
    pods[3].quantity = 20;
    pods[3].manifest_count = 20;
    for (uint16_t i = 0; i < pods[3].manifest_count; i++) {
        pods[3].manifest_units[i].commodity = COMMODITY_REPAIR_KIT;
        pods[3].manifest_units[i].grade = MINING_GRADE_RARE;
    }
    pods[3].pos = v2(124.0f, -44.0f);
    pods[3].vel = v2(1.5f, -2.0f);
    pods[3].radius = 18.0f;
    pods[3].rotation = 0.75f;
    cargo_pod_set_player_tractor(&pods[3], 2);
    pods[4].active = true;
    pods[4].kind = CARGO_POD_CARGO;
    pods[4].commodity = COMMODITY_FRAME;
    pods[4].quantity = 5;
    pods[4].manifest_count = 5;
    pods[4].shipment_id = 77;
    for (uint16_t i = 0; i < pods[4].manifest_count; i++)
        pods[4].manifest_units[i].commodity = COMMODITY_FRAME;
    pods[4].pos = v2(8.0f, 8.0f);
    cargo_pod_clear_tractor(&pods[4]);
    cargo_pod_set_module_tractor(&pods[4], 2, 5);

    uint8_t buf[2 + MAX_CARGO_PODS * CARGO_POD_Q_RECORD_SIZE];
    int len = serialize_cargo_pods_q(buf, pods);

    ASSERT_EQ_INT(buf[0], NET_MSG_WORLD_CARGO_PODS_Q);
    ASSERT_EQ_INT(buf[1], 2);
    ASSERT_EQ_INT(len, 2 + 2 * CARGO_POD_Q_RECORD_SIZE);
    uint8_t *p = &buf[2];
    ASSERT_EQ_INT(p[0], 3);
    ASSERT_EQ_INT(p[1], CARGO_POD_CARGO);
    ASSERT_EQ_INT(p[2], COMMODITY_REPAIR_KIT);
    ASSERT_EQ_INT(p[3], 2);
    ASSERT_EQ_INT((int)(int16_t)read_u16_le(&p[4]), 31);
    ASSERT_EQ_INT((int)(int16_t)read_u16_le(&p[6]), -11);
    ASSERT_EQ_INT((int)(int16_t)read_u16_le(&p[8]), 6);
    ASSERT_EQ_INT((int)(int16_t)read_u16_le(&p[10]), -8);
    ASSERT_EQ_FLOAT(read_f32_le(&p[12]), 18.0f, 0.1f);
    ASSERT_EQ_FLOAT(((float)read_u16_le(&p[16]) / 65536.0f) * TWO_PI_F,
                    0.75f, 0.001f);
    ASSERT_EQ_INT(read_u16_le(&p[18]), 20);
    ASSERT_EQ_INT(read_u16_le(&p[20]), 20);
    ASSERT_EQ_INT(read_u16_le(&p[22]), 0);
    ASSERT(p[24] & CARGO_POD_SUMMARY_EXACT_MATERIAL);
    ASSERT(!(p[24] & CARGO_POD_SUMMARY_SHIPMENT_BOUND));
    ASSERT_EQ_INT(p[25], MINING_GRADE_RARE);

    p = &buf[2 + CARGO_POD_Q_RECORD_SIZE];
    ASSERT_EQ_INT(p[0], 4);
    ASSERT_EQ_INT(p[3], 0xFF);
    ASSERT_EQ_INT(read_u16_le(&p[18]), 5);
    ASSERT_EQ_INT(read_u16_le(&p[20]), 5);
    ASSERT_EQ_INT(read_u16_le(&p[22]), 77);
    ASSERT(!(p[24] & CARGO_POD_SUMMARY_EXACT_MATERIAL));
    ASSERT(p[24] & CARGO_POD_SUMMARY_SHIPMENT_BOUND);
    ASSERT_EQ_INT(p[25], MINING_GRADE_COMMON);
    ASSERT_EQ_INT(p[26], 3);
    ASSERT_EQ_INT(p[27], 6);
}

TEST(test_world_cargo_pods_semantic_hash_ignores_pose_drift) {
    cargo_pod_t pods[MAX_CARGO_PODS];
    memset(pods, 0, sizeof(pods));
    pods[3].active = true;
    pods[3].kind = CARGO_POD_CARGO;
    pods[3].commodity = COMMODITY_REPAIR_KIT;
    pods[3].quantity = 20;
    pods[3].manifest_count = 20;
    pods[3].shipment_id = 77;
    pods[3].pos = v2(123.0f, -45.0f);
    pods[3].vel = v2(1.5f, -2.0f);
    pods[3].radius = 18.0f;
    pods[3].rotation = 0.75f;
    cargo_pod_set_player_tractor(&pods[3], 2);
    cargo_pod_set_module_tractor(&pods[3], 2, 5);

    uint8_t a[2 + MAX_CARGO_PODS * CARGO_POD_RECORD_SIZE];
    uint8_t b[2 + MAX_CARGO_PODS * CARGO_POD_RECORD_SIZE];
    int alen = serialize_cargo_pods(a, pods);
    uint64_t ahash = net_world_cargo_pods_semantic_hash(a, alen);

    pods[3].pos = v2(150.0f, -80.0f);
    pods[3].vel = v2(-3.0f, 4.0f);
    pods[3].rotation = 1.5f;
    int blen = serialize_cargo_pods(b, pods);
    uint64_t bhash = net_world_cargo_pods_semantic_hash(b, blen);
    ASSERT_EQ_INT(alen, blen);
    ASSERT(ahash == bhash);

    pods[3].quantity = 19;
    blen = serialize_cargo_pods(b, pods);
    bhash = net_world_cargo_pods_semantic_hash(b, blen);
    ASSERT(ahash != bhash);

    pods[3].quantity = 20;
    cargo_pod_clear_tractor(&pods[3]);
    blen = serialize_cargo_pods(b, pods);
    bhash = net_world_cargo_pods_semantic_hash(b, blen);
    ASSERT(ahash != bhash);
}

TEST(test_world_cargo_pods_q_semantic_hash_ignores_pose_drift) {
    cargo_pod_t pods[MAX_CARGO_PODS];
    memset(pods, 0, sizeof(pods));
    pods[3].active = true;
    pods[3].kind = CARGO_POD_CARGO;
    pods[3].commodity = COMMODITY_REPAIR_KIT;
    pods[3].quantity = 20;
    pods[3].manifest_count = 20;
    pods[3].shipment_id = 77;
    pods[3].pos = v2(124.0f, -44.0f);
    pods[3].vel = v2(1.5f, -2.0f);
    pods[3].radius = 18.0f;
    pods[3].rotation = 0.75f;
    cargo_pod_set_player_tractor(&pods[3], 2);
    cargo_pod_set_module_tractor(&pods[3], 2, 5);

    uint8_t a[2 + MAX_CARGO_PODS * CARGO_POD_Q_RECORD_SIZE];
    uint8_t b[2 + MAX_CARGO_PODS * CARGO_POD_Q_RECORD_SIZE];
    int alen = serialize_cargo_pods_q(a, pods);
    uint64_t ahash = net_world_cargo_pods_semantic_hash(a, alen);

    pods[3].pos = v2(160.0f, -80.0f);
    pods[3].vel = v2(-3.0f, 4.0f);
    pods[3].rotation = 1.5f;
    int blen = serialize_cargo_pods_q(b, pods);
    uint64_t bhash = net_world_cargo_pods_semantic_hash(b, blen);
    ASSERT_EQ_INT(alen, blen);
    ASSERT(ahash == bhash);

    pods[3].radius = 20.0f;
    blen = serialize_cargo_pods_q(b, pods);
    bhash = net_world_cargo_pods_semantic_hash(b, blen);
    ASSERT(ahash != bhash);

    pods[3].radius = 18.0f;
    pods[3].quantity = 19;
    blen = serialize_cargo_pods_q(b, pods);
    bhash = net_world_cargo_pods_semantic_hash(b, blen);
    ASSERT(ahash != bhash);
}

TEST(test_world_cargo_pods_metadata_refresh_uses_sparse_safety_heartbeat) {
    ASSERT_EQ_INT((int)CARGO_POD_NET_METADATA_HEARTBEAT_TICKS, 2400);

    ASSERT(cargo_pod_net_metadata_refresh_due(0u, 10u));
    ASSERT(!cargo_pod_net_metadata_refresh_due(
        100u, 100u + CARGO_POD_NET_METADATA_HEARTBEAT_TICKS - 1u));
    ASSERT(cargo_pod_net_metadata_refresh_due(
        100u, 100u + CARGO_POD_NET_METADATA_HEARTBEAT_TICKS));

    uint32_t last = UINT32_MAX - 12u;
    ASSERT(!cargo_pod_net_metadata_refresh_due(
        last, last + CARGO_POD_NET_METADATA_HEARTBEAT_TICKS - 1u));
    ASSERT(cargo_pod_net_metadata_refresh_due(
        last, last + CARGO_POD_NET_METADATA_HEARTBEAT_TICKS));
}

TEST(test_cargo_pod_delta_uses_compact_removal_stream_when_available) {
    cargo_pod_t pods[MAX_CARGO_PODS];
    memset(pods, 0, sizeof(pods));
    pods[5].active = true;
    pods[5].kind = CARGO_POD_CARGO;
    pods[5].commodity = COMMODITY_FERRITE_INGOT;
    pods[5].quantity = 3;
    pods[5].pos = v2(10.0f, 20.0f);
    pods[5].radius = 12.0f;
    cargo_pod_clear_tractor(&pods[5]);
    pods[6] = pods[5];
    pods[6].commodity = COMMODITY_CUPRITE_INGOT;
    pods[6].quantity = 5;
    pods[6].pos = v2(-30.0f, 15.0f);

    bool sent[MAX_CARGO_PODS] = { false };
    uint64_t sent_sig[MAX_CARGO_PODS] = { 0 };
    uint8_t buf[2 + MAX_CARGO_PODS * CARGO_POD_RECORD_SIZE];
    uint8_t remove[CARGO_POD_REMOVE_MSG_HEADER +
                   MAX_CARGO_PODS * CARGO_POD_REMOVE_RECORD_SIZE];
    int remove_len = 0;

    int len = serialize_cargo_pods_for_player_delta(
        buf, remove, &remove_len, pods, v2(0.0f, 0.0f),
        sent, sent_sig, false);
    ASSERT_EQ_INT(buf[0], NET_MSG_WORLD_CARGO_PODS);
    ASSERT_EQ_INT(buf[1], 2);
    ASSERT_EQ_INT(len, 2 + 2 * CARGO_POD_RECORD_SIZE);
    ASSERT_EQ_INT(remove[0], NET_MSG_WORLD_CARGO_POD_REMOVE);
    ASSERT_EQ_INT(remove[1], 0);
    ASSERT_EQ_INT(remove_len, CARGO_POD_REMOVE_MSG_HEADER);

    len = serialize_cargo_pods_for_player_delta(
        buf, remove, &remove_len, pods, v2(0.0f, 0.0f),
        sent, sent_sig, false);
    ASSERT_EQ_INT(buf[1], 0);
    ASSERT_EQ_INT(len, 2);
    ASSERT_EQ_INT(remove[1], 0);

    pods[5].active = false;
    len = serialize_cargo_pods_for_player_delta(
        buf, remove, &remove_len, pods, v2(0.0f, 0.0f),
        sent, sent_sig, false);
    ASSERT_EQ_INT(buf[1], 0);
    ASSERT_EQ_INT(len, 2);
    ASSERT_EQ_INT(remove[0], NET_MSG_WORLD_CARGO_POD_REMOVE);
    ASSERT_EQ_INT(remove[1], 1);
    ASSERT_EQ_INT(remove_len, CARGO_POD_REMOVE_MSG_HEADER +
                            CARGO_POD_REMOVE_RECORD_SIZE);
    ASSERT_EQ_INT(remove[CARGO_POD_REMOVE_MSG_HEADER], 5);
    ASSERT(!sent[5]);

    pods[6].quantity = 6;
    len = serialize_cargo_pods_for_player_delta(
        buf, remove, &remove_len, pods, v2(0.0f, 0.0f),
        sent, sent_sig, false);
    ASSERT_EQ_INT(buf[1], 1);
    ASSERT_EQ_INT(len, 2 + CARGO_POD_RECORD_SIZE);
    ASSERT_EQ_INT(buf[2], 6);
    ASSERT_EQ_INT(read_u16_le(&buf[2 + 28]), 6);
    ASSERT_EQ_INT(remove[1], 0);
}

TEST(test_cargo_pod_q_delta_uses_compact_identity_and_removal) {
    cargo_pod_t pods[MAX_CARGO_PODS];
    memset(pods, 0, sizeof(pods));
    pods[5].active = true;
    pods[5].kind = CARGO_POD_CARGO;
    pods[5].commodity = COMMODITY_FERRITE_INGOT;
    pods[5].quantity = 3;
    pods[5].pos = v2(12.0f, 20.0f);
    pods[5].radius = 12.0f;
    cargo_pod_clear_tractor(&pods[5]);
    pods[6] = pods[5];
    pods[6].commodity = COMMODITY_CUPRITE_INGOT;
    pods[6].quantity = 5;
    pods[6].pos = v2(-32.0f, 16.0f);

    bool sent[MAX_CARGO_PODS] = { false };
    uint64_t sent_sig[MAX_CARGO_PODS] = { 0 };
    uint8_t buf[2 + MAX_CARGO_PODS * CARGO_POD_Q_RECORD_SIZE];
    uint8_t remove[CARGO_POD_REMOVE_MSG_HEADER +
                   MAX_CARGO_PODS * CARGO_POD_REMOVE_RECORD_SIZE];
    int remove_len = 0;

    int len = serialize_cargo_pods_q_for_player_delta(
        buf, remove, &remove_len, pods, v2(0.0f, 0.0f),
        sent, sent_sig, false);
    ASSERT_EQ_INT(buf[0], NET_MSG_WORLD_CARGO_PODS_Q);
    ASSERT_EQ_INT(buf[1], 2);
    ASSERT_EQ_INT(len, 2 + 2 * CARGO_POD_Q_RECORD_SIZE);
    ASSERT_EQ_INT(remove[0], NET_MSG_WORLD_CARGO_POD_REMOVE);
    ASSERT_EQ_INT(remove[1], 0);
    ASSERT_EQ_INT(remove_len, CARGO_POD_REMOVE_MSG_HEADER);

    len = serialize_cargo_pods_q_for_player_delta(
        buf, remove, &remove_len, pods, v2(0.0f, 0.0f),
        sent, sent_sig, false);
    ASSERT_EQ_INT(buf[1], 0);
    ASSERT_EQ_INT(len, 2);
    ASSERT_EQ_INT(remove[1], 0);

    pods[5].active = false;
    len = serialize_cargo_pods_q_for_player_delta(
        buf, remove, &remove_len, pods, v2(0.0f, 0.0f),
        sent, sent_sig, false);
    ASSERT_EQ_INT(buf[1], 0);
    ASSERT_EQ_INT(len, 2);
    ASSERT_EQ_INT(remove[0], NET_MSG_WORLD_CARGO_POD_REMOVE);
    ASSERT_EQ_INT(remove[1], 1);
    ASSERT_EQ_INT(remove_len, CARGO_POD_REMOVE_MSG_HEADER +
                            CARGO_POD_REMOVE_RECORD_SIZE);
    ASSERT_EQ_INT(remove[CARGO_POD_REMOVE_MSG_HEADER], 5);
    ASSERT(!sent[5]);

    pods[6].quantity = 6;
    len = serialize_cargo_pods_q_for_player_delta(
        buf, remove, &remove_len, pods, v2(0.0f, 0.0f),
        sent, sent_sig, false);
    ASSERT_EQ_INT(buf[0], NET_MSG_WORLD_CARGO_PODS_Q);
    ASSERT_EQ_INT(buf[1], 1);
    ASSERT_EQ_INT(len, 2 + CARGO_POD_Q_RECORD_SIZE);
    ASSERT_EQ_INT(buf[2], 6);
    ASSERT_EQ_INT(read_u16_le(&buf[2 + 18]), 6);
    ASSERT_EQ_INT(remove[1], 0);
}

TEST(test_scaffold_delta_uses_compact_removal_stream_when_available) {
    scaffold_t scaffolds[MAX_SCAFFOLDS];
    memset(scaffolds, 0, sizeof(scaffolds));
    scaffolds[3].active = true;
    scaffolds[3].state = SCAFFOLD_LOOSE;
    scaffolds[3].module_type = MODULE_DOCK;
    scaffolds[3].owner = -1;
    scaffolds[3].pos = v2(10.0f, 20.0f);
    scaffolds[3].radius = 30.0f;
    scaffolds[4] = scaffolds[3];
    scaffolds[4].module_type = MODULE_FURNACE;
    scaffolds[4].pos = v2(-30.0f, 15.0f);

    bool sent[MAX_SCAFFOLDS] = { false };
    uint64_t sent_sig[MAX_SCAFFOLDS] = { 0 };
    uint64_t motion_sig[MAX_SCAFFOLDS] = { 0 };
    uint8_t buf[2 + MAX_SCAFFOLDS * SCAFFOLD_RECORD_SIZE];
    uint8_t motion[SCAFFOLD_MOTION_Q_MSG_HEADER +
                   MAX_SCAFFOLDS * SCAFFOLD_MOTION_Q_RECORD_SIZE];
    uint8_t remove[SCAFFOLD_REMOVE_MSG_HEADER +
                   MAX_SCAFFOLDS * SCAFFOLD_REMOVE_RECORD_SIZE];
    int remove_len = 0;

    int len = serialize_scaffolds_for_player_delta(
        buf, remove, &remove_len, scaffolds, v2(0.0f, 0.0f),
        sent, sent_sig, motion_sig);
    ASSERT_EQ_INT(buf[0], NET_MSG_WORLD_SCAFFOLDS);
    ASSERT_EQ_INT(buf[1], 2);
    ASSERT_EQ_INT(len, 2 + 2 * SCAFFOLD_RECORD_SIZE);
    ASSERT_EQ_INT(remove[0], NET_MSG_WORLD_SCAFFOLD_REMOVE);
    ASSERT_EQ_INT(remove[1], 0);
    ASSERT_EQ_INT(remove_len, SCAFFOLD_REMOVE_MSG_HEADER);
    int motion_len = serialize_scaffold_motion_q_for_player_delta(
        motion, scaffolds, v2(0.0f, 0.0f), sent, motion_sig);
    ASSERT_EQ_INT(motion[0], NET_MSG_WORLD_SCAFFOLD_MOTION_Q);
    ASSERT_EQ_INT(motion[1], 0);
    ASSERT_EQ_INT(motion_len, SCAFFOLD_MOTION_Q_MSG_HEADER);

    len = serialize_scaffolds_for_player_delta(
        buf, remove, &remove_len, scaffolds, v2(0.0f, 0.0f),
        sent, sent_sig, motion_sig);
    ASSERT_EQ_INT(buf[1], 0);
    ASSERT_EQ_INT(len, 2);
    ASSERT_EQ_INT(remove[1], 0);

    scaffolds[3].active = false;
    len = serialize_scaffolds_for_player_delta(
        buf, remove, &remove_len, scaffolds, v2(0.0f, 0.0f),
        sent, sent_sig, motion_sig);
    ASSERT_EQ_INT(buf[1], 0);
    ASSERT_EQ_INT(len, 2);
    ASSERT_EQ_INT(remove[0], NET_MSG_WORLD_SCAFFOLD_REMOVE);
    ASSERT_EQ_INT(remove[1], 1);
    ASSERT_EQ_INT(remove_len, SCAFFOLD_REMOVE_MSG_HEADER +
                            SCAFFOLD_REMOVE_RECORD_SIZE);
    ASSERT_EQ_INT(remove[SCAFFOLD_REMOVE_MSG_HEADER], 3);
    ASSERT(!sent[3]);

    scaffolds[4].pos.x = -25.0f;
    scaffolds[4].vel = v2(2.0f, -1.0f);
    len = serialize_scaffolds_for_player_delta(
        buf, remove, &remove_len, scaffolds, v2(0.0f, 0.0f),
        sent, sent_sig, motion_sig);
    ASSERT_EQ_INT(buf[1], 0);
    ASSERT_EQ_INT(len, 2);
    ASSERT_EQ_INT(remove[1], 0);
    motion_len = serialize_scaffold_motion_q_for_player_delta(
        motion, scaffolds, v2(0.0f, 0.0f), sent, motion_sig);
    ASSERT_EQ_INT(motion[0], NET_MSG_WORLD_SCAFFOLD_MOTION_Q);
    ASSERT_EQ_INT(motion[1], 1);
    ASSERT_EQ_INT(motion_len, SCAFFOLD_MOTION_Q_MSG_HEADER +
                              SCAFFOLD_MOTION_Q_RECORD_SIZE);
    const uint8_t *p = &motion[SCAFFOLD_MOTION_Q_MSG_HEADER];
    ASSERT_EQ_INT(p[0], 4);
    ASSERT_EQ_FLOAT((float)(int16_t)read_u16_le(&p[1]) *
                    SCAFFOLD_MOTION_Q_POS_SCALE,
                    -24.0f, SCAFFOLD_MOTION_Q_POS_SCALE);
    ASSERT_EQ_FLOAT((float)(int16_t)read_u16_le(&p[5]) *
                    SCAFFOLD_MOTION_Q_VEL_SCALE,
                    2.0f, SCAFFOLD_MOTION_Q_VEL_SCALE);
}

TEST(test_cargo_pod_motion_stream_uses_relevance_filter) {
    cargo_pod_t pods[MAX_CARGO_PODS];
    memset(pods, 0, sizeof(pods));

    pods[5].active = true;
    pods[5].kind = CARGO_POD_CARGO;
    pods[5].commodity = COMMODITY_FERRITE_INGOT;
    pods[5].pos = v2(10.0f, -20.0f);
    pods[5].vel = v2(1.25f, -2.5f);
    pods[5].rotation = 0.5f;

    pods[6].active = true;
    pods[6].kind = CARGO_POD_CARGO;
    pods[6].commodity = COMMODITY_CUPRITE_INGOT;
    pods[6].pos = v2(0.0f, 5000.0f);
    pods[6].vel = v2(9.0f, 9.0f);
    pods[6].rotation = 2.0f;

    uint8_t buf[CARGO_POD_MOTION_MSG_HEADER +
                MAX_CARGO_PODS * CARGO_POD_MOTION_RECORD_SIZE];
    int len = serialize_cargo_pod_motion_for_player(
        buf, pods, v2(0.0f, 0.0f));

    ASSERT_EQ_INT(buf[0], NET_MSG_WORLD_CARGO_POD_MOTION);
    ASSERT_EQ_INT(buf[1], 1);
    ASSERT_EQ_INT(len, CARGO_POD_MOTION_MSG_HEADER +
                       CARGO_POD_MOTION_RECORD_SIZE);
    const uint8_t *p = &buf[CARGO_POD_MOTION_MSG_HEADER];
    ASSERT_EQ_INT(p[0], 5);
    ASSERT_EQ_FLOAT(read_f32_le(&p[1]), 10.0f, 0.001f);
    ASSERT_EQ_FLOAT(read_f32_le(&p[5]), -20.0f, 0.001f);
    ASSERT_EQ_FLOAT(read_f32_le(&p[9]), 1.25f, 0.001f);
    ASSERT_EQ_FLOAT(read_f32_le(&p[13]), -2.5f, 0.001f);
    ASSERT_EQ_FLOAT(read_f32_le(&p[17]), 0.5f, 0.001f);
}

TEST(test_cargo_pod_motion_q_stream_quantizes_pose) {
    cargo_pod_t pods[MAX_CARGO_PODS];
    memset(pods, 0, sizeof(pods));

    pods[5].active = true;
    pods[5].kind = CARGO_POD_CARGO;
    pods[5].commodity = COMMODITY_FERRITE_INGOT;
    pods[5].pos = v2(10.0f, -20.0f);
    pods[5].vel = v2(1.25f, -2.5f);
    pods[5].rotation = 0.5f;

    pods[6].active = true;
    pods[6].kind = CARGO_POD_CARGO;
    pods[6].commodity = COMMODITY_CUPRITE_INGOT;
    pods[6].pos = v2(0.0f, 5000.0f);
    pods[6].vel = v2(9.0f, 9.0f);
    pods[6].rotation = 2.0f;

    uint8_t buf[CARGO_POD_MOTION_Q_MSG_HEADER +
                MAX_CARGO_PODS * CARGO_POD_MOTION_Q_RECORD_SIZE];
    int len = serialize_cargo_pod_motion_q_for_player(
        buf, pods, v2(0.0f, 0.0f));

    ASSERT_EQ_INT(buf[0], NET_MSG_WORLD_CARGO_POD_MOTION_Q);
    ASSERT_EQ_INT(buf[1], 1);
    ASSERT_EQ_INT(len, CARGO_POD_MOTION_Q_MSG_HEADER +
                       CARGO_POD_MOTION_Q_RECORD_SIZE);
    const uint8_t *p = &buf[CARGO_POD_MOTION_Q_MSG_HEADER];
    ASSERT_EQ_INT(p[0], 5);
    ASSERT_EQ_INT((int)(int16_t)read_u16_le(&p[1]), 3);
    ASSERT_EQ_INT((int)(int16_t)read_u16_le(&p[3]), -5);
    ASSERT_EQ_INT((int)(int16_t)read_u16_le(&p[5]), 5);
    ASSERT_EQ_INT((int)(int16_t)read_u16_le(&p[7]), -10);

    float decoded_x =
        (float)(int16_t)read_u16_le(&p[1]) * CARGO_POD_MOTION_Q_POS_SCALE;
    float decoded_vx =
        (float)(int16_t)read_u16_le(&p[5]) * CARGO_POD_MOTION_Q_VEL_SCALE;
    float decoded_rot =
        ((float)read_u16_le(&p[9]) / 65536.0f) * TWO_PI_F;
    ASSERT_EQ_FLOAT(decoded_x, 12.0f, 0.001f);
    ASSERT_EQ_FLOAT(decoded_vx, 1.25f, 0.001f);
    ASSERT_EQ_FLOAT(decoded_rot, 0.5f, 0.001f);
}

TEST(test_cargo_pod_motion_linear_q_uses_position_velocity_when_rotation_matches) {
    SERVER_PLAYER_DECL(sp);

    cargo_pod_motion_note_sent(&sp,
                               5,
                               v2(10.0f, -20.0f),
                               v2(2.0f, 0.0f),
                               0.25f,
                               100u);

    ASSERT(cargo_pod_motion_should_send(&sp,
                                        5,
                                        v2(50.0f, -20.0f),
                                        v2(12.0f, -4.0f),
                                        0.25f,
                                        136u));
    ASSERT(cargo_pod_motion_linear_q_eligible(&sp,
                                              5,
                                              0.25f,
                                              136u));

    uint8_t rec[CARGO_POD_LINEAR_Q_RECORD_SIZE];
    serialize_one_cargo_pod_linear_q(rec,
                                     5,
                                     v2(50.0f, -20.0f),
                                     v2(12.0f, -4.0f));
    ASSERT_EQ_INT(rec[0], 5);
    ASSERT_EQ_INT((int)(int16_t)read_u16_le(&rec[1]), 13);
    ASSERT_EQ_INT((int)(int16_t)read_u16_le(&rec[3]), -5);
    ASSERT_EQ_INT((int)(int16_t)read_u16_le(&rec[5]), 48);
    ASSERT_EQ_INT((int)(int16_t)read_u16_le(&rec[7]), -16);

    ASSERT(!cargo_pod_motion_linear_q_eligible(&sp,
                                               6,
                                               0.25f,
                                               136u));
    ASSERT(!cargo_pod_motion_linear_q_eligible(&sp,
                                               5,
                                               1.75f,
                                               136u));
    ASSERT(!cargo_pod_motion_linear_q_eligible(
        &sp, 5, 0.25f, 100u + CARGO_POD_MOTION_HEARTBEAT_TICKS));
}

TEST(test_cargo_pod_motion_prediction_gate_skips_predicted_pose) {
    SERVER_PLAYER_DECL(sp);

    ASSERT_EQ_INT((int)CARGO_POD_MOTION_NET_REPEAT_TICKS, 240);
    ASSERT_EQ_FLOAT(CARGO_POD_MOTION_VEL_ERROR_SQ, 64.0f, 0.001f);
    ASSERT_EQ_FLOAT(CARGO_POD_MOTION_ROT_ERROR, 1.25f, 0.001f);
    ASSERT_EQ_INT((int)CARGO_POD_MOTION_HEARTBEAT_TICKS, 720);

    cargo_pod_motion_note_sent(&sp,
                               5,
                               v2(10.0f, -20.0f),
                               v2(2.0f, 0.0f),
                               0.25f,
                               100u);

    ASSERT(!cargo_pod_motion_should_send(&sp,
                                         5,
                                         v2(10.6f, -20.0f),
                                         v2(2.0f, 0.0f),
                                         0.25f,
                                         136u));
    ASSERT(!cargo_pod_motion_should_send(&sp,
                                         5,
                                         v2(12.0f, -20.0f),
                                         v2(2.0f, 0.0f),
                                         0.25f,
                                         220u));
    ASSERT(!cargo_pod_motion_should_send(&sp,
                                         5,
                                         v2(14.0f, -20.0f),
                                         v2(5.0f, 0.0f),
                                         0.75f,
                                         100u + CARGO_POD_MOTION_NET_REPEAT_TICKS));
    ASSERT(cargo_pod_motion_should_send(&sp,
                                        5,
                                        v2(12.0f, -20.0f),
                                        v2(2.0f, 0.0f),
                                        0.25f,
                                        100u + CARGO_POD_MOTION_HEARTBEAT_TICKS));
}

TEST(test_cargo_pod_motion_prediction_gate_sends_divergence) {
    SERVER_PLAYER_DECL(sp);

    cargo_pod_motion_note_sent(&sp,
                               5,
                               v2(10.0f, -20.0f),
                               v2(2.0f, 0.0f),
                               0.25f,
                               100u);

    ASSERT(cargo_pod_motion_should_send(&sp,
                                        5,
                                        v2(50.0f, -20.0f),
                                        v2(2.0f, 0.0f),
                                        0.25f,
                                        136u));
    ASSERT(cargo_pod_motion_should_send(&sp,
                                        5,
                                        v2(10.6f, -20.0f),
                                        v2(12.0f, 0.0f),
                                        0.25f,
                                        136u));
    ASSERT(cargo_pod_motion_should_send(&sp,
                                        5,
                                        v2(10.6f, -20.0f),
                                        v2(2.0f, 0.0f),
                                        1.75f,
                                        136u));
}

TEST(test_roundtrip_interactions) {
    sim_interactions_t interactions;
    memset(&interactions, 0, sizeof(interactions));
    interactions.count = 2;

    interactions.items[0].type = SIM_INTERACTION_NONE;
    interactions.items[1].type = SIM_INTERACTION_TRACTOR_BEAM;
    interactions.items[1].visual = SIM_INTERACTION_VISUAL_CARGO_POD_MODULE_TRACTOR;
    interactions.items[1].commodity = COMMODITY_FERRITE_INGOT;
    interactions.items[1].flags = 0x5A;
    interactions.items[1].source.type = SIM_INTERACTION_ENTITY_STATION_MODULE;
    interactions.items[1].source.index = 3;
    interactions.items[1].source.aux = 7;
    interactions.items[1].target.type = SIM_INTERACTION_ENTITY_CARGO_POD;
    interactions.items[1].target.index = 42;
    interactions.items[1].target.aux = -1;
    interactions.items[1].source_pos = v2(10.5f, -20.25f);
    interactions.items[1].target_pos = v2(30.0f, 40.75f);
    interactions.items[1].range = 512.0f;
    interactions.items[1].intensity = 0.65f;

    uint8_t buf[2 + SIM_MAX_INTERACTIONS * INTERACTION_RECORD_SIZE];
    int len = serialize_interactions(buf, &interactions);

    ASSERT_EQ_INT(buf[0], NET_MSG_WORLD_INTERACTIONS);
    ASSERT_EQ_INT(buf[1], 1);
    ASSERT_EQ_INT(len, 2 + INTERACTION_RECORD_SIZE);

    uint8_t *p = &buf[2];
    ASSERT_EQ_INT(p[0], SIM_INTERACTION_TRACTOR_BEAM);
    ASSERT_EQ_INT(p[1], SIM_INTERACTION_VISUAL_CARGO_POD_MODULE_TRACTOR);
    ASSERT_EQ_INT(p[2], COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(p[3], 0x5A);
    ASSERT_EQ_INT(p[4], SIM_INTERACTION_ENTITY_STATION_MODULE);
    ASSERT_EQ_INT((int)(int16_t)read_u16_le(&p[5]), 3);
    ASSERT_EQ_INT((int)(int16_t)read_u16_le(&p[7]), 7);
    ASSERT_EQ_INT(p[9], SIM_INTERACTION_ENTITY_CARGO_POD);
    ASSERT_EQ_INT((int)(int16_t)read_u16_le(&p[10]), 42);
    ASSERT_EQ_INT((int)(int16_t)read_u16_le(&p[12]), -1);
    ASSERT_EQ_FLOAT(read_f32_le(&p[14]), 10.5f, 0.001f);
    ASSERT_EQ_FLOAT(read_f32_le(&p[18]), -20.25f, 0.001f);
    ASSERT_EQ_FLOAT(read_f32_le(&p[22]), 30.0f, 0.001f);
    ASSERT_EQ_FLOAT(read_f32_le(&p[26]), 40.75f, 0.001f);
    ASSERT_EQ_FLOAT(read_f32_le(&p[30]), 512.0f, 0.001f);
    ASSERT_EQ_FLOAT(read_f32_le(&p[34]), 0.65f, 0.001f);
}

TEST(test_roundtrip_interactions_q_quantizes_visual_tail) {
    sim_interactions_t interactions;
    memset(&interactions, 0, sizeof(interactions));
    interactions.count = 2;

    interactions.items[0].type = SIM_INTERACTION_NONE;
    interactions.items[1].type = SIM_INTERACTION_TRACTOR_BEAM;
    interactions.items[1].visual = SIM_INTERACTION_VISUAL_CARGO_POD_MODULE_TRACTOR;
    interactions.items[1].commodity = COMMODITY_FERRITE_INGOT;
    interactions.items[1].flags = 0x5A;
    interactions.items[1].source.type = SIM_INTERACTION_ENTITY_STATION_MODULE;
    interactions.items[1].source.index = 3;
    interactions.items[1].source.aux = 7;
    interactions.items[1].target.type = SIM_INTERACTION_ENTITY_CARGO_POD;
    interactions.items[1].target.index = 42;
    interactions.items[1].target.aux = -1;
    interactions.items[1].source_pos = v2(12.0f, -20.0f);
    interactions.items[1].target_pos = v2(32.0f, 40.0f);
    interactions.items[1].range = 512.0f;
    interactions.items[1].intensity = 0.65f;

    uint8_t buf[2 + SIM_MAX_INTERACTIONS * INTERACTION_Q_RECORD_SIZE];
    int len = serialize_interactions_q(buf, &interactions);

    ASSERT_EQ_INT(buf[0], NET_MSG_WORLD_INTERACTIONS_Q);
    ASSERT_EQ_INT(buf[1], 1);
    ASSERT_EQ_INT(len, 2 + INTERACTION_Q_RECORD_SIZE);

    uint8_t *p = &buf[2];
    ASSERT_EQ_INT(p[0], SIM_INTERACTION_TRACTOR_BEAM);
    ASSERT_EQ_INT(p[1], SIM_INTERACTION_VISUAL_CARGO_POD_MODULE_TRACTOR);
    ASSERT_EQ_INT(p[2], COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(p[3], 0x5A);
    ASSERT_EQ_INT(p[4], SIM_INTERACTION_ENTITY_STATION_MODULE);
    ASSERT_EQ_INT((int)(int16_t)read_u16_le(&p[5]), 3);
    ASSERT_EQ_INT((int)(int16_t)read_u16_le(&p[7]), 7);
    ASSERT_EQ_INT(p[9], SIM_INTERACTION_ENTITY_CARGO_POD);
    ASSERT_EQ_INT((int)(int16_t)read_u16_le(&p[10]), 42);
    ASSERT_EQ_INT((int)(int16_t)read_u16_le(&p[12]), -1);
    ASSERT_EQ_INT((int)(int16_t)read_u16_le(&p[14]), 3);
    ASSERT_EQ_INT((int)(int16_t)read_u16_le(&p[16]), -5);
    ASSERT_EQ_INT((int)(int16_t)read_u16_le(&p[18]), 8);
    ASSERT_EQ_INT((int)(int16_t)read_u16_le(&p[20]), 10);
    ASSERT_EQ_INT(read_u16_le(&p[22]), 128);
    ASSERT_EQ_INT(p[24], 166);
}

TEST(test_world_interactions_semantic_hash_ignores_endpoint_drift) {
    sim_interactions_t interactions;
    memset(&interactions, 0, sizeof(interactions));
    interactions.count = 1;
    interactions.items[0] = (sim_interaction_t){
        .type = SIM_INTERACTION_TRACTOR_BEAM,
        .visual = SIM_INTERACTION_VISUAL_CARGO_POD_MODULE_TRACTOR,
        .commodity = COMMODITY_CUPRITE_ORE,
        .source = {
            .type = SIM_INTERACTION_ENTITY_STATION_MODULE,
            .index = 2,
            .aux = 4,
        },
        .target = {
            .type = SIM_INTERACTION_ENTITY_CARGO_POD,
            .index = 7,
            .aux = -1,
        },
        .source_pos = { 10.0f, 20.0f },
        .target_pos = { 30.0f, 40.0f },
        .range = 100.0f,
        .intensity = 0.5f,
    };

    uint8_t a[2 + SIM_MAX_INTERACTIONS * INTERACTION_RECORD_SIZE];
    uint8_t b[2 + SIM_MAX_INTERACTIONS * INTERACTION_RECORD_SIZE];
    int alen = serialize_interactions(a, &interactions);
    uint64_t ahash = net_world_interactions_semantic_hash(a, alen);

    interactions.items[0].source_pos = v2(11.0f, 22.0f);
    interactions.items[0].target_pos = v2(33.0f, 44.0f);
    interactions.items[0].range = 125.0f;
    interactions.items[0].intensity = 0.25f;
    int blen = serialize_interactions(b, &interactions);
    uint64_t bhash = net_world_interactions_semantic_hash(b, blen);
    ASSERT_EQ_INT(alen, blen);
    ASSERT(ahash == bhash);

    interactions.items[0].target.index = 8;
    blen = serialize_interactions(b, &interactions);
    bhash = net_world_interactions_semantic_hash(b, blen);
    ASSERT(ahash != bhash);
}

TEST(test_world_interactions_q_semantic_hash_ignores_visual_tail) {
    sim_interactions_t interactions;
    memset(&interactions, 0, sizeof(interactions));
    interactions.count = 1;
    interactions.items[0] = (sim_interaction_t){
        .type = SIM_INTERACTION_TRACTOR_BEAM,
        .visual = SIM_INTERACTION_VISUAL_CARGO_POD_MODULE_TRACTOR,
        .commodity = COMMODITY_CUPRITE_ORE,
        .source = {
            .type = SIM_INTERACTION_ENTITY_STATION_MODULE,
            .index = 2,
            .aux = 4,
        },
        .target = {
            .type = SIM_INTERACTION_ENTITY_CARGO_POD,
            .index = 7,
            .aux = -1,
        },
        .source_pos = { 10.0f, 20.0f },
        .target_pos = { 30.0f, 40.0f },
        .range = 100.0f,
        .intensity = 0.5f,
    };

    uint8_t a[2 + SIM_MAX_INTERACTIONS * INTERACTION_Q_RECORD_SIZE];
    uint8_t b[2 + SIM_MAX_INTERACTIONS * INTERACTION_Q_RECORD_SIZE];
    int alen = serialize_interactions_q(a, &interactions);
    uint64_t ahash = net_world_interactions_semantic_hash(a, alen);

    interactions.items[0].source_pos = v2(12.0f, 24.0f);
    interactions.items[0].target_pos = v2(36.0f, 48.0f);
    interactions.items[0].range = 128.0f;
    interactions.items[0].intensity = 0.25f;
    int blen = serialize_interactions_q(b, &interactions);
    uint64_t bhash = net_world_interactions_semantic_hash(b, blen);
    ASSERT_EQ_INT(alen, blen);
    ASSERT(ahash == bhash);

    interactions.items[0].target.index = 8;
    blen = serialize_interactions_q(b, &interactions);
    bhash = net_world_interactions_semantic_hash(b, blen);
    ASSERT(ahash != bhash);
}

TEST(test_world_interactions_metadata_refresh_uses_sparse_safety_heartbeat) {
    ASSERT_EQ_INT((int)INTERACTION_NET_METADATA_HEARTBEAT_TICKS, 2400);

    ASSERT(interaction_net_metadata_refresh_due(0u, 10u));
    ASSERT(!interaction_net_metadata_refresh_due(
        100u, 100u + INTERACTION_NET_METADATA_HEARTBEAT_TICKS - 1u));
    ASSERT(interaction_net_metadata_refresh_due(
        100u, 100u + INTERACTION_NET_METADATA_HEARTBEAT_TICKS));

    uint32_t last = UINT32_MAX - 12u;
    ASSERT(!interaction_net_metadata_refresh_due(
        last, last + INTERACTION_NET_METADATA_HEARTBEAT_TICKS - 1u));
    ASSERT(interaction_net_metadata_refresh_due(
        last, last + INTERACTION_NET_METADATA_HEARTBEAT_TICKS));
}

TEST(test_interaction_drift_stream_quantizes_visual_fields) {
    sim_interactions_t interactions;
    memset(&interactions, 0, sizeof(interactions));
    interactions.count = 2;
    interactions.items[0] = (sim_interaction_t){
        .type = SIM_INTERACTION_TRACTOR_BEAM,
        .visual = SIM_INTERACTION_VISUAL_CARGO_POD_MODULE_TRACTOR,
        .commodity = COMMODITY_FERRITE_INGOT,
        .source = {
            .type = SIM_INTERACTION_ENTITY_STATION_MODULE,
            .index = 1,
            .aux = 2,
        },
        .target = {
            .type = SIM_INTERACTION_ENTITY_CARGO_POD,
            .index = 5,
            .aux = -1,
        },
        .source_pos = { .x = 10.0f, .y = -20.0f },
        .target_pos = { .x = 30.0f, .y = 40.0f },
        .range = 128.0f,
        .intensity = 0.5f,
    };
    interactions.items[1].type = SIM_INTERACTION_NONE;

    uint8_t buf[INTERACTION_DRIFT_MSG_HEADER +
                SIM_MAX_INTERACTIONS * INTERACTION_DRIFT_RECORD_SIZE];
    int len = serialize_interaction_drift(buf, &interactions);

    ASSERT_EQ_INT(buf[0], NET_MSG_WORLD_INTERACTION_DRIFT);
    ASSERT_EQ_INT(buf[1], 1);
    ASSERT_EQ_INT(len, INTERACTION_DRIFT_MSG_HEADER +
                       INTERACTION_DRIFT_RECORD_SIZE);
    const uint8_t *p = &buf[INTERACTION_DRIFT_MSG_HEADER];
    ASSERT_EQ_INT(p[0], 0);
    ASSERT_EQ_INT((int)(int16_t)read_u16_le(&p[1]), 3);
    ASSERT_EQ_INT((int)(int16_t)read_u16_le(&p[3]), -5);
    ASSERT_EQ_INT((int)(int16_t)read_u16_le(&p[5]), 8);
    ASSERT_EQ_INT((int)(int16_t)read_u16_le(&p[7]), 10);
    ASSERT_EQ_INT(read_u16_le(&p[9]), 32);
    ASSERT_EQ_INT(p[11], 128);
}

TEST(test_interaction_drift_repeat_uses_visual_cadence) {
    ASSERT_EQ_INT((int)INTERACTION_DRIFT_NET_REPEAT_TICKS, 12);
    ASSERT(interaction_drift_repeat_due(0u, 100u));
    ASSERT(interaction_drift_repeat_due(100u, 340u));
    ASSERT(!interaction_drift_repeat_due(
        100u, 100u + INTERACTION_DRIFT_NET_REPEAT_TICKS - 1u));
    ASSERT(interaction_drift_repeat_due(
        100u, 100u + INTERACTION_DRIFT_NET_REPEAT_TICKS));
}

TEST(test_interaction_streams_use_relevance_filter) {
    sim_interactions_t interactions;
    memset(&interactions, 0, sizeof(interactions));
    interactions.count = 3;

    interactions.items[0] = (sim_interaction_t){
        .type = SIM_INTERACTION_TRACTOR_BEAM,
        .visual = SIM_INTERACTION_VISUAL_CARGO_POD_MODULE_TRACTOR,
        .commodity = COMMODITY_FERRITE_INGOT,
        .source = { .type = SIM_INTERACTION_ENTITY_STATION_MODULE, .index = 1, .aux = 2 },
        .target = { .type = SIM_INTERACTION_ENTITY_CARGO_POD, .index = 3, .aux = -1 },
        .source_pos = { 5000.0f, 5000.0f },
        .target_pos = { 5100.0f, 5000.0f },
        .range = 256.0f,
        .intensity = 0.25f,
    };
    interactions.items[1] = (sim_interaction_t){
        .type = SIM_INTERACTION_TRACTOR_BEAM,
        .visual = SIM_INTERACTION_VISUAL_CARGO_POD_MODULE_TRACTOR,
        .commodity = COMMODITY_CUPRITE_INGOT,
        .source = { .type = SIM_INTERACTION_ENTITY_STATION_MODULE, .index = 4, .aux = 5 },
        .target = { .type = SIM_INTERACTION_ENTITY_CARGO_POD, .index = 6, .aux = -1 },
        .source_pos = { 10.0f, 20.0f },
        .target_pos = { 100.0f, 200.0f },
        .range = 512.0f,
        .intensity = 0.75f,
    };
    interactions.items[2] = (sim_interaction_t){
        .type = SIM_INTERACTION_TRACTOR_BEAM,
        .visual = SIM_INTERACTION_VISUAL_CARGO_POD_MODULE_TRACTOR,
        .commodity = COMMODITY_CRYSTAL_INGOT,
        .source = { .type = SIM_INTERACTION_ENTITY_STATION_MODULE, .index = 7, .aux = 8 },
        .target = { .type = SIM_INTERACTION_ENTITY_CARGO_POD, .index = 9, .aux = -1 },
        .source_pos = { -4000.0f, 0.0f },
        .target_pos = { 4000.0f, 0.0f },
        .range = 8000.0f,
        .intensity = 0.5f,
    };

    uint8_t identity[2 + SIM_MAX_INTERACTIONS * INTERACTION_RECORD_SIZE];
    int ilen = serialize_interactions_for_player(
        identity, &interactions, v2(0.0f, 0.0f));
    ASSERT_EQ_INT(identity[0], NET_MSG_WORLD_INTERACTIONS);
    ASSERT_EQ_INT(identity[1], 2);
    ASSERT_EQ_INT(ilen, 2 + 2 * INTERACTION_RECORD_SIZE);
    const uint8_t *first = &identity[2];
    const uint8_t *second = &identity[2 + INTERACTION_RECORD_SIZE];
    ASSERT_EQ_INT(first[2], COMMODITY_CUPRITE_INGOT);
    ASSERT_EQ_INT((int)(int16_t)read_u16_le(&first[10]), 6);
    ASSERT_EQ_INT(second[2], COMMODITY_CRYSTAL_INGOT);
    ASSERT_EQ_INT((int)(int16_t)read_u16_le(&second[10]), 9);

    uint8_t drift[INTERACTION_DRIFT_MSG_HEADER +
                  SIM_MAX_INTERACTIONS * INTERACTION_DRIFT_RECORD_SIZE];
    int dlen = serialize_interaction_drift_for_player(
        drift, &interactions, v2(0.0f, 0.0f));
    ASSERT_EQ_INT(drift[0], NET_MSG_WORLD_INTERACTION_DRIFT);
    ASSERT_EQ_INT(drift[1], 2);
    ASSERT_EQ_INT(dlen, INTERACTION_DRIFT_MSG_HEADER +
                       2 * INTERACTION_DRIFT_RECORD_SIZE);
    ASSERT_EQ_INT(drift[INTERACTION_DRIFT_MSG_HEADER], 0);
    ASSERT_EQ_INT(drift[INTERACTION_DRIFT_MSG_HEADER +
                        INTERACTION_DRIFT_RECORD_SIZE], 1);
}

TEST(test_roundtrip_npcs) {
    NPC_SHIP_ARRAY(npcs, MAX_NPC_SHIPS);

    npcs[0].active = true;
    npcs[0].role = NPC_ROLE_MINER;
    npcs[0].state = NPC_STATE_MINING;
    npcs[0].thrusting = true;
    npcs[0].ship->pos = v2(800.0f, 400.0f);
    npcs[0].ship->vel = v2(10.0f, -5.0f);
    npcs[0].ship->angle = 1.57f;
    npcs[0].target_asteroid = 512;
    npc_set_towed_fragment_index(&npcs[0], 1024);
    npcs[0].home_station = 2;
    memcpy(npcs[0].session_token, "NPC\002\000\005\064\022", 8);

    npcs[0].tint_r = 0.55f;
    npcs[0].tint_g = 0.25f;
    npcs[0].tint_b = 0.18f;

    uint8_t buf[2 + MAX_NPC_SHIPS * NPC_RECORD_SIZE];
    int len = serialize_npcs(buf, npcs);

    ASSERT_EQ_INT(buf[0], NET_MSG_WORLD_NPCS);
    ASSERT_EQ_INT(buf[1], 1);
    ASSERT_EQ_INT(len, 2 + NPC_RECORD_SIZE);

    uint8_t *p = &buf[2];
    ASSERT_EQ_INT(p[0], 0);
    ASSERT(p[1] & 1);                              /* active */
    ASSERT_EQ_INT((p[1] >> 1) & 0x3, NPC_ROLE_MINER);
    ASSERT_EQ_INT((p[1] >> 3) & 0x7, NPC_STATE_MINING);
    ASSERT(p[1] & (1 << 6));                        /* thrusting */
    ASSERT_EQ_FLOAT(read_f32_le(&p[2]), 800.0f, 0.1f);
    ASSERT_EQ_FLOAT(read_f32_le(&p[18]), 1.57f, 0.01f);
    ASSERT_EQ_INT(read_u16_le(&p[22]), 512);       /* target_asteroid */
    ASSERT_EQ_INT(read_u16_le(&p[24]), 1024);      /* towed_fragment */
    ASSERT_EQ_INT(p[26], (int)(0.55f * 255.0f));
    ASSERT(memcmp(&p[29], npcs[0].session_token, 8) == 0);
    ASSERT_EQ_INT(p[37], 2);
}

TEST(test_npc_snapshot_serializes_embedded_ship_tow_slot) {
    NPC_SHIP_ARRAY(npcs, MAX_NPC_SHIPS);

    npcs[0].active = true;
    npcs[0].role = NPC_ROLE_MINER;
    npcs[0].state = NPC_STATE_RETURN_TO_STATION;
    npc_clear_towed_fragment(&npcs[0]);
    npcs[0].ship->towed_fragments[0] = 77;
    npcs[0].ship->towed_count = 1;

    uint8_t buf[2 + MAX_NPC_SHIPS * NPC_RECORD_SIZE];
    int len = serialize_npcs(buf, npcs);

    ASSERT_EQ_INT(len, 2 + NPC_RECORD_SIZE);
    ASSERT_EQ_INT(read_u16_le(&buf[2 + 24]), 77);
}

TEST(test_world_npcs_semantic_hash_ignores_pose_drift) {
    NPC_SHIP_ARRAY(npcs, MAX_NPC_SHIPS);

    npcs[2].active = true;
    npcs[2].role = NPC_ROLE_HAULER;
    npcs[2].state = NPC_STATE_TRAVEL_TO_DEST;
    npcs[2].thrusting = true;
    npcs[2].ship->pos = v2(100.0f, 200.0f);
    npcs[2].ship->vel = v2(8.0f, -3.0f);
    npcs[2].ship->angle = 0.5f;
    npcs[2].target_asteroid = 12;
    npc_set_towed_fragment_index(&npcs[2], 33);
    npcs[2].home_station = 1;
    memcpy(npcs[2].session_token, "NPCPOSE1", 8);
    npcs[2].tint_r = 0.4f;
    npcs[2].tint_g = 0.6f;
    npcs[2].tint_b = 0.8f;

    uint8_t a[2 + MAX_NPC_SHIPS * NPC_RECORD_SIZE];
    uint8_t b[2 + MAX_NPC_SHIPS * NPC_RECORD_SIZE];
    int alen = serialize_npcs(a, npcs);

    npcs[2].ship->pos = v2(125.0f, 190.0f);
    npcs[2].ship->vel = v2(9.0f, -4.0f);
    npcs[2].ship->angle = 0.7f;
    int blen = serialize_npcs(b, npcs);

    ASSERT_EQ_INT(alen, blen);
    uint64_t ahash = net_world_npcs_semantic_hash(a, alen);
    uint64_t bhash = net_world_npcs_semantic_hash(b, blen);
    ASSERT(ahash == bhash);

    npcs[2].state = NPC_STATE_UNLOADING;
    blen = serialize_npcs(b, npcs);
    bhash = net_world_npcs_semantic_hash(b, blen);
    ASSERT(ahash == bhash);

    npcs[2].state = NPC_STATE_TRAVEL_TO_DEST;
    npcs[2].target_asteroid = 13;
    blen = serialize_npcs(b, npcs);
    bhash = net_world_npcs_semantic_hash(b, blen);
    ASSERT(ahash == bhash);

    npcs[2].target_asteroid = 12;
    npc_set_towed_fragment_index(&npcs[2], 34);
    blen = serialize_npcs(b, npcs);
    bhash = net_world_npcs_semantic_hash(b, blen);
    ASSERT(ahash == bhash);

    npc_set_towed_fragment_index(&npcs[2], 33);
    npcs[2].thrusting = false;
    blen = serialize_npcs(b, npcs);
    bhash = net_world_npcs_semantic_hash(b, blen);
    ASSERT(ahash == bhash);

    npcs[2].tint_r = 0.9f;
    npcs[2].tint_g = 0.2f;
    npcs[2].tint_b = 0.1f;
    blen = serialize_npcs(b, npcs);
    bhash = net_world_npcs_semantic_hash(b, blen);
    ASSERT(ahash == bhash);

    npcs[2].role = NPC_ROLE_MINER;
    blen = serialize_npcs(b, npcs);
    bhash = net_world_npcs_semantic_hash(b, blen);
    ASSERT(ahash != bhash);

    npcs[2].role = NPC_ROLE_HAULER;
    memcpy(npcs[2].session_token, "NPCPOSE2", 8);
    blen = serialize_npcs(b, npcs);
    bhash = net_world_npcs_semantic_hash(b, blen);
    ASSERT(ahash != bhash);
}

TEST(test_world_npcs_metadata_refresh_uses_sparse_safety_heartbeat) {
    ASSERT_EQ_INT((int)NPC_NET_METADATA_HEARTBEAT_TICKS, 2400);

    ASSERT(npc_net_metadata_refresh_due(0u, 10u));
    ASSERT(!npc_net_metadata_refresh_due(
        100u, 100u + NPC_NET_METADATA_HEARTBEAT_TICKS - 1u));
    ASSERT(npc_net_metadata_refresh_due(
        100u, 100u + NPC_NET_METADATA_HEARTBEAT_TICKS));

    uint32_t last = UINT32_MAX - 12u;
    ASSERT(!npc_net_metadata_refresh_due(
        last, last + NPC_NET_METADATA_HEARTBEAT_TICKS - 1u));
    ASSERT(npc_net_metadata_refresh_due(
        last, last + NPC_NET_METADATA_HEARTBEAT_TICKS));
}

TEST(test_world_npc_status_semantic_hash_ignores_thrust_only) {
    vec2 player_pos = v2(0.0f, 0.0f);

    NPC_SHIP_ARRAY(npcs, MAX_NPC_SHIPS);
    npcs[1].active = true;
    npcs[1].role = NPC_ROLE_HAULER;
    npcs[1].state = NPC_STATE_TRAVEL_TO_DEST;
    npcs[1].thrusting = true;
    npcs[1].ship->pos = v2(100.0f, 0.0f);
    npcs[1].target_asteroid = 44;
    npc_set_towed_fragment_index(&npcs[1], 12);

    uint8_t a[NPC_STATUS_MSG_HEADER +
              MAX_NPC_SHIPS * NPC_STATUS_RECORD_SIZE];
    uint8_t b[NPC_STATUS_MSG_HEADER +
              MAX_NPC_SHIPS * NPC_STATUS_RECORD_SIZE];
    int alen = serialize_npc_status_for_player(a, npcs, player_pos);

    npcs[1].thrusting = false;
    int blen = serialize_npc_status_for_player(b, npcs, player_pos);

    ASSERT_EQ_INT(alen, blen);
    uint64_t ahash = net_world_npc_status_semantic_hash(a, alen);
    uint64_t bhash = net_world_npc_status_semantic_hash(b, blen);
    ASSERT(ahash == bhash);

    npcs[1].state = NPC_STATE_UNLOADING;
    blen = serialize_npc_status_for_player(b, npcs, player_pos);
    bhash = net_world_npc_status_semantic_hash(b, blen);
    ASSERT(ahash != bhash);

    npcs[1].state = NPC_STATE_TRAVEL_TO_DEST;
    npcs[1].target_asteroid = 45;
    blen = serialize_npc_status_for_player(b, npcs, player_pos);
    bhash = net_world_npc_status_semantic_hash(b, blen);
    ASSERT(ahash != bhash);

    npcs[1].target_asteroid = 44;
    npc_set_towed_fragment_index(&npcs[1], 13);
    blen = serialize_npc_status_for_player(b, npcs, player_pos);
    bhash = net_world_npc_status_semantic_hash(b, blen);
    ASSERT(ahash != bhash);
}

TEST(test_world_npc_status8_semantic_hash_ignores_thrust_only) {
    vec2 player_pos = v2(0.0f, 0.0f);
    NPC_SHIP_ARRAY(npcs, MAX_NPC_SHIPS);
    npcs[1].active = true;
    npcs[1].role = NPC_ROLE_MINER;
    npcs[1].state = NPC_STATE_TRAVEL_TO_DEST;
    npcs[1].ship->pos = v2(20.0f, 0.0f);
    npcs[1].target_asteroid = 44;
    npc_set_towed_fragment_index(&npcs[1], 13);

    uint8_t a[NPC_STATUS8_MSG_HEADER +
              MAX_NPC_SHIPS * NPC_STATUS8_RECORD_SIZE];
    uint8_t b[NPC_STATUS8_MSG_HEADER +
              MAX_NPC_SHIPS * NPC_STATUS8_RECORD_SIZE];
    int alen = serialize_npc_status8_for_player(a, npcs, player_pos);
    npcs[1].thrusting = true;
    int blen = serialize_npc_status8_for_player(b, npcs, player_pos);

    ASSERT_EQ_INT(alen, NPC_STATUS8_MSG_HEADER + NPC_STATUS8_RECORD_SIZE);
    ASSERT_EQ_INT(blen, NPC_STATUS8_MSG_HEADER + NPC_STATUS8_RECORD_SIZE);
    uint64_t ahash = net_world_npc_status_semantic_hash(a, alen);
    uint64_t bhash = net_world_npc_status_semantic_hash(b, blen);
    ASSERT(ahash == bhash);

    npcs[1].target_asteroid = 45;
    blen = serialize_npc_status8_for_player(b, npcs, player_pos);
    bhash = net_world_npc_status_semantic_hash(b, blen);
    ASSERT(ahash != bhash);
}

TEST(test_npc_motion_stream_uses_relevance_filter) {
    vec2 player_pos = v2(0.0f, 0.0f);

    NPC_SHIP_ARRAY(npcs, MAX_NPC_SHIPS);
    npcs[1].active = true;
    npcs[1].ship->pos = v2(120.0f, -80.0f);
    npcs[1].ship->vel = v2(4.0f, 5.0f);
    npcs[1].ship->angle = 0.75f;
    npcs[1].thrusting = true;
    npcs[2].active = true;
    npcs[2].ship->pos = v2(4000.0f, 0.0f);

    uint8_t buf[NPC_MOTION_MSG_HEADER +
                MAX_NPC_SHIPS * NPC_MOTION_RECORD_SIZE];
    int len = serialize_npc_motion_for_player(buf, npcs, player_pos);

    ASSERT_EQ_INT(buf[0], NET_MSG_WORLD_NPC_MOTION);
    ASSERT_EQ_INT(buf[1], 1);
    ASSERT_EQ_INT(len, NPC_MOTION_MSG_HEADER + NPC_MOTION_RECORD_SIZE);
    const uint8_t *p = &buf[NPC_MOTION_MSG_HEADER];
    ASSERT_EQ_INT(p[0], 1);
    ASSERT(p[1] & 1);
    ASSERT(p[1] & (1 << 6));
    ASSERT_EQ_FLOAT(read_f32_le(&p[2]), 120.0f, 0.01f);
    ASSERT_EQ_FLOAT(read_f32_le(&p[6]), -80.0f, 0.01f);
    ASSERT_EQ_FLOAT(read_f32_le(&p[10]), 4.0f, 0.01f);
    ASSERT_EQ_FLOAT(read_f32_le(&p[14]), 5.0f, 0.01f);
    ASSERT_EQ_FLOAT(read_f32_le(&p[18]), 0.75f, 0.01f);
}

TEST(test_npc_motion_q_stream_quantizes_pose) {
    vec2 player_pos = v2(0.0f, 0.0f);

    NPC_SHIP_ARRAY(npcs, MAX_NPC_SHIPS);
    npcs[1].active = true;
    npcs[1].ship->pos = v2(120.0f, -80.0f);
    npcs[1].ship->vel = v2(4.0f, 5.0f);
    npcs[1].ship->angle = 0.75f;
    npcs[1].thrusting = true;
    npcs[2].active = true;
    npcs[2].ship->pos = v2(4000.0f, 0.0f);

    uint8_t buf[NPC_MOTION_Q_MSG_HEADER +
                MAX_NPC_SHIPS * NPC_MOTION_Q_RECORD_SIZE];
    int len = serialize_npc_motion_q_for_player(buf, npcs, player_pos);

    ASSERT_EQ_INT(buf[0], NET_MSG_WORLD_NPC_MOTION_Q);
    ASSERT_EQ_INT(buf[1], 1);
    ASSERT_EQ_INT(len, NPC_MOTION_Q_MSG_HEADER + NPC_MOTION_Q_RECORD_SIZE);
    const uint8_t *p = &buf[NPC_MOTION_Q_MSG_HEADER];
    ASSERT_EQ_INT(p[0], 1);
    ASSERT(p[1] & 1);
    ASSERT(p[1] & (1 << 6));
    ASSERT_EQ_FLOAT((float)(int16_t)read_u16_le(&p[2]) *
                    NPC_MOTION_Q_POS_SCALE, 120.0f, NPC_MOTION_Q_POS_SCALE);
    ASSERT_EQ_FLOAT((float)(int16_t)read_u16_le(&p[4]) *
                    NPC_MOTION_Q_POS_SCALE, -80.0f, NPC_MOTION_Q_POS_SCALE);
    ASSERT_EQ_FLOAT((float)(int16_t)read_u16_le(&p[6]) *
                    NPC_MOTION_Q_VEL_SCALE, 4.0f, NPC_MOTION_Q_VEL_SCALE);
    ASSERT_EQ_FLOAT((float)(int16_t)read_u16_le(&p[8]) *
                    NPC_MOTION_Q_VEL_SCALE, 5.0f, NPC_MOTION_Q_VEL_SCALE);
    ASSERT_EQ_FLOAT((float)read_u16_le(&p[10]) *
                    NPC_MOTION_Q_ANGLE_SCALE, 0.75f,
                    NPC_MOTION_Q_ANGLE_SCALE);
}

TEST(test_npc_motion8_q_stream_uses_byte_velocity_and_angle) {
    vec2 player_pos = v2(0.0f, 0.0f);

    NPC_SHIP_ARRAY(npcs, MAX_NPC_SHIPS);
    npcs[1].active = true;
    npcs[1].ship->pos = v2(120.0f, -80.0f);
    npcs[1].ship->vel = v2(4.0f, -6.0f);
    npcs[1].ship->angle = 0.75f;
    npcs[1].thrusting = true;
    npcs[2].active = true;
    npcs[2].ship->pos = v2(4000.0f, 0.0f);

    uint8_t buf[NPC_MOTION8_Q_MSG_HEADER +
                MAX_NPC_SHIPS * NPC_MOTION8_Q_RECORD_SIZE];
    int len = serialize_npc_motion8_q_for_player(buf, npcs, player_pos);

    ASSERT_EQ_INT(buf[0], NET_MSG_WORLD_NPC_MOTION8_Q);
    ASSERT_EQ_INT(buf[1], 1);
    ASSERT_EQ_INT(len,
                  NPC_MOTION8_Q_MSG_HEADER + NPC_MOTION8_Q_RECORD_SIZE);
    const uint8_t *p = &buf[NPC_MOTION8_Q_MSG_HEADER];
    ASSERT_EQ_INT(p[0], 1);
    ASSERT(p[1] & 1);
    ASSERT(p[1] & (1 << 6));
    ASSERT_EQ_FLOAT((float)(int16_t)read_u16_le(&p[2]) *
                    NPC_MOTION_Q_POS_SCALE, 120.0f, NPC_MOTION_Q_POS_SCALE);
    ASSERT_EQ_FLOAT((float)(int16_t)read_u16_le(&p[4]) *
                    NPC_MOTION_Q_POS_SCALE, -80.0f, NPC_MOTION_Q_POS_SCALE);
    ASSERT_EQ_FLOAT((float)(int8_t)p[6] * NPC_MOTION8_Q_VEL_SCALE,
                    4.0f, NPC_MOTION8_Q_VEL_SCALE);
    ASSERT_EQ_FLOAT((float)(int8_t)p[7] * NPC_MOTION8_Q_VEL_SCALE,
                    -6.0f, NPC_MOTION8_Q_VEL_SCALE);
    ASSERT_EQ_FLOAT((float)p[8] * NPC_MOTION8_Q_ANGLE_SCALE,
                    0.75f, NPC_MOTION8_Q_ANGLE_SCALE);
}

TEST(test_npc_motion_pos_q_uses_position_only_when_baseline_matches) {
    SERVER_PLAYER_DECL(sp);

    npc_motion_note_sent(&sp,
                         2,
                         1,
                         v2(0.0f, 0.0f),
                         v2(1.0f, 0.0f),
                         0.5f,
                         100u);

    ASSERT(npc_motion_should_send(&sp,
                                  2,
                                  1,
                                  v2(40.0f, -12.0f),
                                  v2(1.0f, 0.0f),
                                  0.5f,
                                  120u));
    ASSERT(npc_motion_pos_q_eligible(&sp,
                                     2,
                                     1,
                                     v2(1.0f, 0.0f),
                                     0.5f,
                                     120u));

    uint8_t rec[NPC_POS_Q_RECORD_SIZE];
    serialize_one_npc_pos_q(rec, 2, v2(40.0f, -12.0f));
    ASSERT_EQ_INT(rec[0], 2);
    ASSERT_EQ_INT((int)(int16_t)read_u16_le(&rec[1]), 10);
    ASSERT_EQ_INT((int)(int16_t)read_u16_le(&rec[3]), -3);

    ASSERT(!npc_motion_pos_q_eligible(&sp,
                                      2,
                                      (uint8_t)(1 | (1 << 6)),
                                      v2(1.0f, 0.0f),
                                      0.5f,
                                      120u));
    ASSERT(!npc_motion_pos_q_eligible(&sp,
                                      2,
                                      1,
                                      v2(12.0f, 0.0f),
                                      0.5f,
                                      120u));
    ASSERT(!npc_motion_pos_q_eligible(&sp,
                                      2,
                                      1,
                                      v2(1.0f, 0.0f),
                                      2.0f,
                                      120u));
    ASSERT(!npc_motion_pos_q_eligible(&sp,
                                      2,
                                      1,
                                      v2(1.0f, 0.0f),
                                      0.5f,
                                      100u + NPC_MOTION_HEARTBEAT_TICKS));
}

TEST(test_npc_motion_pose_q_uses_pose_when_angle_changes_only) {
    SERVER_PLAYER_DECL(sp);

    npc_motion_note_sent(&sp,
                         2,
                         1,
                         v2(0.0f, 0.0f),
                         v2(1.0f, 0.0f),
                         0.5f,
                         100u);

    ASSERT(npc_motion_should_send(&sp,
                                  2,
                                  1,
                                  v2(4.0f, -8.0f),
                                  v2(1.0f, 0.0f),
                                  2.5f,
                                  120u));
    ASSERT(!npc_motion_pos_q_eligible(&sp,
                                      2,
                                      1,
                                      v2(1.0f, 0.0f),
                                      2.5f,
                                      120u));
    ASSERT(npc_motion_pose_q_eligible(&sp,
                                      2,
                                      1,
                                      v2(1.0f, 0.0f),
                                      120u));

    uint8_t rec[NPC_POSE_Q_RECORD_SIZE];
    serialize_one_npc_pose_q(rec, 2, v2(4.0f, -8.0f), 2.5f);
    ASSERT_EQ_INT(rec[0], 2);
    ASSERT_EQ_INT((int)(int16_t)read_u16_le(&rec[1]), 1);
    ASSERT_EQ_INT((int)(int16_t)read_u16_le(&rec[3]), -2);
    ASSERT_EQ_INT((int)read_u16_le(&rec[5]),
                  (int)npc_motion_q_encode_angle(2.5f));

    ASSERT(!npc_motion_pose_q_eligible(&sp,
                                       2,
                                       (uint8_t)(1 | (1 << 6)),
                                       v2(1.0f, 0.0f),
                                       120u));
    ASSERT(!npc_motion_pose_q_eligible(&sp,
                                       2,
                                       1,
                                       v2(12.0f, 0.0f),
                                       120u));
    ASSERT(!npc_motion_pose_q_eligible(&sp,
                                       2,
                                       1,
                                       v2(1.0f, 0.0f),
                                       100u + NPC_MOTION_HEARTBEAT_TICKS));
}

TEST(test_npc_motion_linear_q_uses_position_velocity_when_angle_matches) {
    SERVER_PLAYER_DECL(sp);

    npc_motion_note_sent(&sp,
                         2,
                         1,
                         v2(0.0f, 0.0f),
                         v2(1.0f, 0.0f),
                         0.5f,
                         100u);

    ASSERT(npc_motion_should_send(&sp,
                                  2,
                                  1,
                                  v2(20.0f, -12.0f),
                                  v2(12.0f, -4.0f),
                                  0.5f,
                                  120u));
    ASSERT(!npc_motion_pos_q_eligible(&sp,
                                      2,
                                      1,
                                      v2(12.0f, -4.0f),
                                      0.5f,
                                      120u));
    ASSERT(!npc_motion_pose_q_eligible(&sp,
                                       2,
                                       1,
                                       v2(12.0f, -4.0f),
                                       120u));
    ASSERT(npc_motion_linear_q_eligible(&sp,
                                        2,
                                        1,
                                        0.5f,
                                        120u));

    uint8_t rec[NPC_LINEAR_Q_RECORD_SIZE];
    serialize_one_npc_linear_q(rec,
                               2,
                               v2(20.0f, -12.0f),
                               v2(12.0f, -4.0f));
    ASSERT_EQ_INT(rec[0], 2);
    ASSERT_EQ_INT((int)(int16_t)read_u16_le(&rec[1]), 5);
    ASSERT_EQ_INT((int)(int16_t)read_u16_le(&rec[3]), -3);
    ASSERT_EQ_INT((int)(int16_t)read_u16_le(&rec[5]), 48);
    ASSERT_EQ_INT((int)(int16_t)read_u16_le(&rec[7]), -16);

    ASSERT(!npc_motion_linear_q_eligible(&sp,
                                         3,
                                         1,
                                         0.5f,
                                         120u));
    ASSERT(!npc_motion_linear_q_eligible(&sp,
                                         2,
                                         (uint8_t)(1 | (1 << 6)),
                                         0.5f,
                                         120u));
    ASSERT(!npc_motion_linear_q_eligible(&sp,
                                         2,
                                         1,
                                         2.0f,
                                         120u));
    ASSERT(!npc_motion_linear_q_eligible(&sp,
                                         2,
                                         1,
                                         0.5f,
                                         100u + NPC_MOTION_HEARTBEAT_TICKS));
}

TEST(test_npc_motion_prediction_gate_skips_predicted_pose) {
    SERVER_PLAYER_DECL(sp);

    ASSERT_EQ_INT((int)NPC_MOTION_NET_REPEAT_TICKS, 240);
    ASSERT_EQ_INT((int)NPC_STATUS_NET_REPEAT_TICKS, 240);
    ASSERT_EQ_INT((int)NPC_MOTION_HEARTBEAT_TICKS, 720);
    ASSERT_EQ_FLOAT((float)NPC_MOTION_VEL_ERROR_SQ, 64.0f, 0.001f);
    ASSERT_EQ_FLOAT((float)NPC_MOTION_ANGLE_ERROR, 1.25f, 0.001f);

    uint8_t thrust_flags = (uint8_t)(1u | (1u << 6));

    npc_motion_note_sent(&sp,
                         5,
                         thrust_flags,
                         v2(10.0f, -20.0f),
                         v2(2.0f, 0.0f),
                         0.25f,
                         100u);

    ASSERT(!npc_motion_should_send(&sp,
                                   5,
                                   thrust_flags,
                                   v2(10.6f, -20.0f),
                                   v2(2.0f, 0.0f),
                                   0.25f,
                                   136u));
    ASSERT(!npc_motion_should_send(&sp,
                                   5,
                                   thrust_flags,
                                   v2(12.0f, -20.0f),
                                   v2(2.0f, 0.0f),
                                   0.25f,
                                   220u));
    ASSERT(!npc_motion_should_send(&sp,
                                   5,
                                   thrust_flags,
                                   v2(14.0f, -20.0f),
                                   v2(9.5f, 0.0f),
                                   1.0f,
                                   100u + NPC_MOTION_NET_REPEAT_TICKS));
    ASSERT(npc_motion_should_send(&sp,
                                  5,
                                  thrust_flags,
                                  v2(12.0f, -20.0f),
                                  v2(2.0f, 0.0f),
                                  0.25f,
                                  100u + NPC_MOTION_HEARTBEAT_TICKS));
    ASSERT(npc_motion_should_send(&sp,
                                  5,
                                  1u,
                                  v2(14.0f, -20.0f),
                                  v2(2.0f, 0.0f),
                                  0.25f,
                                  100u + NPC_MOTION_NET_REPEAT_TICKS));
}

TEST(test_npc_motion_prediction_gate_sends_divergence) {
    SERVER_PLAYER_DECL(sp);

    uint8_t flags = (uint8_t)(1u | (1u << 6));

    npc_motion_note_sent(&sp,
                         5,
                         flags,
                         v2(10.0f, -20.0f),
                         v2(2.0f, 0.0f),
                         0.25f,
                         100u);

    ASSERT(npc_motion_should_send(&sp,
                                  5,
                                  flags,
                                  v2(50.0f, -20.0f),
                                  v2(2.0f, 0.0f),
                                  0.25f,
                                  136u));
    ASSERT(npc_motion_should_send(&sp,
                                  5,
                                  flags,
                                  v2(10.6f, -20.0f),
                                  v2(12.0f, 0.0f),
                                  0.25f,
                                  136u));
    ASSERT(npc_motion_should_send(&sp,
                                  5,
                                  flags,
                                  v2(10.6f, -20.0f),
                                  v2(2.0f, 0.0f),
                                  1.75f,
                                  136u));
}

TEST(test_npc_status_stream_serializes_visual_status) {
    vec2 player_pos = v2(0.0f, 0.0f);

    NPC_SHIP_ARRAY(npcs, MAX_NPC_SHIPS);
    npcs[1].active = true;
    npcs[1].role = NPC_ROLE_MINER;
    npcs[1].state = NPC_STATE_MINING;
    npcs[1].thrusting = true;
    npcs[1].ship->pos = v2(120.0f, -80.0f);
    npcs[1].target_asteroid = 123;
    npc_set_towed_fragment_index(&npcs[1], 77);
    npcs[2].active = true;
    npcs[2].ship->pos = v2(4000.0f, 0.0f);
    npcs[2].target_asteroid = 88;

    uint8_t buf[NPC_STATUS_MSG_HEADER +
                MAX_NPC_SHIPS * NPC_STATUS_RECORD_SIZE];
    int len = serialize_npc_status_for_player(buf, npcs, player_pos);

    ASSERT_EQ_INT(buf[0], NET_MSG_WORLD_NPC_STATUS);
    ASSERT_EQ_INT(buf[1], 1);
    ASSERT_EQ_INT(len, NPC_STATUS_MSG_HEADER + NPC_STATUS_RECORD_SIZE);
    const uint8_t *p = &buf[NPC_STATUS_MSG_HEADER];
    ASSERT_EQ_INT(p[0], 1);
    ASSERT(p[1] & 1);
    ASSERT_EQ_INT((p[1] >> 1) & 0x3, NPC_ROLE_MINER);
    ASSERT_EQ_INT((p[1] >> 3) & 0x7, NPC_STATE_MINING);
    ASSERT(p[1] & (1 << 6));
    ASSERT_EQ_INT(read_u16_le(&p[2]), 123);
    ASSERT_EQ_INT(read_u16_le(&p[4]), 77);
}

TEST(test_npc_status8_stream_serializes_low_refs_and_rejects_high_refs) {
    vec2 player_pos = v2(0.0f, 0.0f);

    NPC_SHIP_ARRAY(npcs, MAX_NPC_SHIPS);
    npcs[1].active = true;
    npcs[1].role = NPC_ROLE_MINER;
    npcs[1].state = NPC_STATE_MINING;
    npcs[1].thrusting = true;
    npcs[1].ship->pos = v2(120.0f, -80.0f);
    npcs[1].target_asteroid = 123;
    npc_set_towed_fragment_index(&npcs[1], 77);
    npcs[2].active = true;
    npcs[2].ship->pos = v2(4000.0f, 0.0f);
    npcs[2].target_asteroid = 88;

    uint8_t buf[NPC_STATUS8_MSG_HEADER +
                MAX_NPC_SHIPS * NPC_STATUS8_RECORD_SIZE];
    int len = serialize_npc_status8_for_player(buf, npcs, player_pos);

    ASSERT_EQ_INT(buf[0], NET_MSG_WORLD_NPC_STATUS8_Q);
    ASSERT_EQ_INT(buf[1], 1);
    ASSERT_EQ_INT(len, NPC_STATUS8_MSG_HEADER + NPC_STATUS8_RECORD_SIZE);
    const uint8_t *p = &buf[NPC_STATUS8_MSG_HEADER];
    ASSERT_EQ_INT(p[0], 1);
    ASSERT(p[1] & 1);
    ASSERT_EQ_INT((p[1] >> 1) & 0x3, NPC_ROLE_MINER);
    ASSERT_EQ_INT((p[1] >> 3) & 0x7, NPC_STATE_MINING);
    ASSERT(p[1] & (1 << 6));
    ASSERT_EQ_INT(p[2], 123);
    ASSERT_EQ_INT(p[3], 77);

    npcs[1].target_asteroid = 255;
    len = serialize_npc_status8_for_player(buf, npcs, player_pos);
    ASSERT_EQ_INT(len, 0);
}

TEST(test_relevance_filtered_world_snapshots) {
    vec2 player_pos = v2(0.0f, 0.0f);

    NPC_SHIP_ARRAY(npcs, MAX_NPC_SHIPS);
    npcs[1].active = true;
    npcs[1].role = NPC_ROLE_HAULER;
    npcs[1].ship->pos = v2(200.0f, 0.0f);
    npcs[2].active = true;
    npcs[2].role = NPC_ROLE_MINER;
    npcs[2].ship->pos = v2(4000.0f, 0.0f);

    uint8_t npc_buf[2 + MAX_NPC_SHIPS * NPC_RECORD_SIZE];
    int npc_len = serialize_npcs_for_player(npc_buf, npcs, player_pos);
    ASSERT_EQ_INT(npc_buf[0], NET_MSG_WORLD_NPCS);
    ASSERT_EQ_INT(npc_buf[1], 1);
    ASSERT_EQ_INT(npc_len, 2 + NPC_RECORD_SIZE);
    ASSERT_EQ_INT(npc_buf[2], 1);

    scaffold_t scaffolds[MAX_SCAFFOLDS];
    memset(scaffolds, 0, sizeof(scaffolds));
    scaffolds[3].active = true;
    scaffolds[3].state = SCAFFOLD_LOOSE;
    scaffolds[3].module_type = MODULE_DOCK;
    scaffolds[3].pos = v2(100.0f, 100.0f);
    scaffolds[4].active = true;
    scaffolds[4].state = SCAFFOLD_LOOSE;
    scaffolds[4].module_type = MODULE_FURNACE;
    scaffolds[4].pos = v2(-4000.0f, 0.0f);

    uint8_t sc_buf[2 + MAX_SCAFFOLDS * SCAFFOLD_RECORD_SIZE];
    int sc_len = serialize_scaffolds_for_player(sc_buf, scaffolds, player_pos);
    ASSERT_EQ_INT(sc_buf[0], NET_MSG_WORLD_SCAFFOLDS);
    ASSERT_EQ_INT(sc_buf[1], 1);
    ASSERT_EQ_INT(sc_len, 2 + SCAFFOLD_RECORD_SIZE);
    ASSERT_EQ_INT(sc_buf[2], 3);

    sc_len = serialize_scaffolds_for_player(sc_buf, scaffolds, v2(9000.0f, 0.0f));
    ASSERT_EQ_INT(sc_buf[0], NET_MSG_WORLD_SCAFFOLDS);
    ASSERT_EQ_INT(sc_buf[1], 0);
    ASSERT_EQ_INT(sc_len, 2);

    cargo_pod_t pods[MAX_CARGO_PODS];
    memset(pods, 0, sizeof(pods));
    pods[5].active = true;
    pods[5].kind = CARGO_POD_CARGO;
    pods[5].commodity = COMMODITY_FERRITE_INGOT;
    pods[5].pos = v2(10.0f, -10.0f);
    pods[6].active = true;
    pods[6].kind = CARGO_POD_CARGO;
    pods[6].commodity = COMMODITY_CUPRITE_INGOT;
    pods[6].pos = v2(0.0f, 5000.0f);

    uint8_t pod_buf[2 + MAX_CARGO_PODS * CARGO_POD_RECORD_SIZE];
    int pod_len = serialize_cargo_pods_for_player(pod_buf, pods, player_pos);
    ASSERT_EQ_INT(pod_buf[0], NET_MSG_WORLD_CARGO_PODS);
    ASSERT_EQ_INT(pod_buf[1], 1);
    ASSERT_EQ_INT(pod_len, 2 + CARGO_POD_RECORD_SIZE);
    ASSERT_EQ_INT(pod_buf[2], 5);
}

typedef struct {
    uint8_t type[16];
    int len[16];
    int slot[16];
    int count;
} packet_capture_t;

static void packet_capture_sink(void *user, const uint8_t *data, int len) {
    packet_capture_t *cap = (packet_capture_t *)user;
    if (!cap || !data || len <= 0 ||
        cap->count >= (int)(sizeof(cap->type) / sizeof(cap->type[0]))) return;
    cap->type[cap->count] = data[0];
    cap->len[cap->count] = len;
    cap->slot[cap->count] = -1;
    cap->count++;
}

static void player_packet_capture_sink(void *user, int player_slot,
                                       const uint8_t *data, int len) {
    packet_capture_t *cap = (packet_capture_t *)user;
    if (!cap || !data || len <= 0 ||
        cap->count >= (int)(sizeof(cap->type) / sizeof(cap->type[0]))) return;
    cap->type[cap->count] = data[0];
    cap->len[cap->count] = len;
    cap->slot[cap->count] = player_slot;
    cap->count++;
}

static bool packet_capture_has_type(const packet_capture_t *cap,
                                    uint8_t type) {
    if (!cap) return false;
    for (int i = 0; i < cap->count; i++) {
        if (cap->type[i] == type) return true;
    }
    return false;
}

typedef struct {
    uint8_t data[NET_INPUT_APPLIED_SIZE];
    int len;
    int count;
} input_applied_capture_t;

typedef struct {
    uint8_t data[NET_STATE_AUTH_SIZE];
    int len;
    int count;
} auth_state_capture_t;

static void input_applied_capture_sink(void *user,
                                       const uint8_t *data,
                                       int len) {
    input_applied_capture_t *cap = (input_applied_capture_t *)user;
    if (!cap || !data || len <= 0) return;
    if (len > (int)sizeof(cap->data)) len = (int)sizeof(cap->data);
    memcpy(cap->data, data, (size_t)len);
    cap->len = len;
    cap->count++;
}

static void auth_state_capture_sink(void *user,
                                    const uint8_t *data,
                                    int len) {
    auth_state_capture_t *cap = (auth_state_capture_t *)user;
    if (!cap || !data || len <= 0) return;
    if (len > (int)sizeof(cap->data)) len = (int)sizeof(cap->data);
    memcpy(cap->data, data, (size_t)len);
    cap->len = len;
    cap->count++;
}

TEST(test_input_applied_emitter_sends_only_on_sequence_change) {
    SERVER_PLAYER_DECL(sp);
    sp.last_input_seq = 0;
    sp.last_input_tick = 0;

    input_applied_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    ASSERT(!server_emit_input_applied_if_changed(
        &sp, 0, 10, input_applied_capture_sink, &cap));
    ASSERT_EQ_INT(cap.count, 0);

    sp.last_input_seq = 44;
    sp.last_input_tick = 1234;
    ASSERT(!server_emit_input_applied_if_changed(
        &sp, 44, 11, input_applied_capture_sink, &cap));
    ASSERT_EQ_INT(cap.count, 0);

    ASSERT(server_emit_input_applied_if_changed(
        &sp, 43, 12, input_applied_capture_sink, &cap));
    ASSERT_EQ_INT(cap.count, 1);
    ASSERT_EQ_INT(cap.len, NET_INPUT_APPLIED_SIZE);
    ASSERT_EQ_INT(cap.data[0], NET_MSG_INPUT_APPLIED);
    ASSERT_EQ_INT((int)read_u16_le(&cap.data[1]), 44);
    ASSERT_EQ_INT((int)read_u32_le(&cap.data[3]), 12);
    ASSERT_EQ_INT((int)read_u32_le(&cap.data[7]), 1234);
}

TEST(test_pending_input_ack_coalesces_to_latest_sequence) {
    SERVER_PLAYER_DECL(sp);
    server_pending_input_ack_t pending;
    server_pending_input_ack_reset(&pending);

    input_applied_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    sp.last_input_seq = 44;
    sp.last_input_tick = 1200;
    ASSERT(!server_pending_input_ack_note(&pending, &sp, 44, 10));
    ASSERT(!pending.pending);

    ASSERT(server_pending_input_ack_note(&pending, &sp, 43, 11));
    ASSERT(pending.pending);
    ASSERT_EQ_INT((int)pending.previous_input_seq, 43);
    ASSERT_EQ_INT((int)pending.server_tick, 11);

    sp.last_input_seq = 45;
    sp.last_input_tick = 1234;
    ASSERT(server_pending_input_ack_note(&pending, &sp, 44, 12));
    ASSERT_EQ_INT((int)pending.previous_input_seq, 43);
    ASSERT_EQ_INT((int)pending.server_tick, 12);

    ASSERT(server_emit_pending_input_applied(
        &pending, &sp, input_applied_capture_sink, &cap));
    ASSERT(!pending.pending);
    ASSERT_EQ_INT(cap.count, 1);
    ASSERT_EQ_INT(cap.len, NET_INPUT_APPLIED_SIZE);
    ASSERT_EQ_INT(cap.data[0], NET_MSG_INPUT_APPLIED);
    ASSERT_EQ_INT((int)read_u16_le(&cap.data[1]), 45);
    ASSERT_EQ_INT((int)read_u32_le(&cap.data[3]), 12);
    ASSERT_EQ_INT((int)read_u32_le(&cap.data[7]), 1234);
    ASSERT(!server_emit_pending_input_applied(
        &pending, &sp, input_applied_capture_sink, &cap));
    ASSERT_EQ_INT(cap.count, 1);
}

TEST(test_authoritative_player_state_emitter_sends_only_on_sequence_change) {
    SERVER_PLAYER_DECL(sp);

    packet_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    ASSERT(!server_emit_authoritative_player_state_if_changed(
        &sp, 1, 0, 10, packet_capture_sink, &cap));
    ASSERT_EQ_INT(cap.count, 0);

    sp.last_input_seq = 44;
    sp.last_input_tick = 1234;
    ASSERT(!server_emit_authoritative_player_state_if_changed(
        &sp, 1, 44, 11, packet_capture_sink, &cap));
    ASSERT_EQ_INT(cap.count, 0);

    ASSERT(server_emit_authoritative_player_state_if_changed(
        &sp, 1, 43, 12, packet_capture_sink, &cap));
    ASSERT_EQ_INT(cap.count, 1);
    ASSERT_EQ_INT(cap.type[0], NET_MSG_STATE);
    ASSERT_EQ_INT(cap.len[0], NET_STATE_AUTH_SIZE);
}

TEST(test_pending_input_ack_emits_single_authoritative_state) {
    SERVER_PLAYER_DECL(sp);
    server_pending_input_ack_t pending;
    server_pending_input_ack_reset(&pending);

    auth_state_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    sp.last_input_seq = 44;
    sp.last_input_tick = 1200;
    ASSERT(server_pending_input_ack_note(&pending, &sp, 43, 11));
    ASSERT(pending.pending);

    sp.last_input_seq = 45;
    sp.last_input_tick = 1234;
    sp.last_input_client_sent_ms = 0x01020304u;
    sp.last_input_server_recv_ms = 0xA0B0C0D0u;
    ASSERT(server_pending_input_ack_note(&pending, &sp, 44, 12));
    ASSERT(server_emit_pending_authoritative_player_state(
        &pending, &sp, 2, auth_state_capture_sink, &cap));
    ASSERT(!pending.pending);
    ASSERT_EQ_INT(cap.count, 1);
    ASSERT_EQ_INT(cap.len, NET_STATE_AUTH_SIZE);
    ASSERT_EQ_INT(cap.data[0], NET_MSG_STATE);
    ASSERT_EQ_INT(cap.data[1], 2);
    ASSERT_EQ_INT((int)read_u16_le(
                      &cap.data[NET_STATE_AUTH_INPUT_ACK_OFFSET]),
                  45);
    ASSERT_EQ_INT((int)read_u32_le(
                      &cap.data[NET_STATE_AUTH_SERVER_TICK_OFFSET]),
                  12);
    ASSERT_EQ_INT((int)read_u32_le(
                      &cap.data[NET_STATE_AUTH_INPUT_TICK_OFFSET]),
                  1234);
    ASSERT_EQ_INT((int)read_u32_le(
                      &cap.data[NET_STATE_AUTH_CLIENT_SENT_MS_OFFSET]),
                  (int)0x01020304u);
    ASSERT_EQ_INT((int)read_u32_le(
                      &cap.data[NET_STATE_AUTH_SERVER_RECV_MS_OFFSET]),
                  (int)0xA0B0C0D0u);
    ASSERT_EQ_INT((int)read_u32_le(
                      &cap.data[NET_STATE_AUTH_SERVER_SEND_MS_OFFSET]),
                  0);
    ASSERT(!server_emit_pending_authoritative_player_state(
        &pending, &sp, 2, auth_state_capture_sink, &cap));
    ASSERT_EQ_INT(cap.count, 1);
}

TEST(test_pending_input_ack_adaptive_prefers_tiny_clean_ack) {
    SERVER_PLAYER_DECL(sp);
    sp.id = 2;
    sp.ship->pos = v2(100.0f, -20.0f);
    sp.ship->vel = v2(12.0f, 0.0f);
    sp.ship->angle = 0.25f;
    sp.ship->tractor_level = 1;
    memset(sp.ship->towed_fragments, -1, sizeof(sp.ship->towed_fragments));
    server_player_note_authoritative_ack_state(&sp, 100);
    sp.last_input_seq = 45;
    sp.last_input_tick = 112;
    sp.ship->pos.x += 8.0f;

    server_pending_input_ack_t pending;
    server_pending_input_ack_reset(&pending);
    ASSERT(server_pending_input_ack_note(&pending, &sp, 44, 112));

    packet_capture_t cap;
    memset(&cap, 0, sizeof(cap));
    ASSERT(server_emit_pending_input_ack_adaptive(
        &pending, &sp, 2, false, packet_capture_sink, &cap));
    ASSERT(!pending.pending);
    ASSERT_EQ_INT(cap.count, 1);
    ASSERT_EQ_INT(cap.type[0], NET_MSG_INPUT_APPLIED);
    ASSERT_EQ_INT(cap.len[0], NET_INPUT_APPLIED_SIZE);
}

TEST(test_pending_input_ack_adaptive_promotes_first_or_forced_state) {
    SERVER_PLAYER_DECL(sp);
    sp.id = 2;
    sp.last_input_seq = 45;
    sp.last_input_tick = 112;
    sp.ship->pos = v2(100.0f, -20.0f);
    memset(sp.ship->towed_fragments, -1, sizeof(sp.ship->towed_fragments));

    server_pending_input_ack_t pending;
    server_pending_input_ack_reset(&pending);
    ASSERT(server_pending_input_ack_note(&pending, &sp, 44, 112));

    packet_capture_t cap;
    memset(&cap, 0, sizeof(cap));
    ASSERT(server_emit_pending_input_ack_adaptive(
        &pending, &sp, 2, false, packet_capture_sink, &cap));
    ASSERT_EQ_INT(cap.count, 1);
    ASSERT_EQ_INT(cap.type[0], NET_MSG_STATE);
    ASSERT_EQ_INT(cap.len[0], NET_STATE_AUTH_SIZE);
    ASSERT(sp.replication->input_ack_state_valid);

    sp.last_input_seq = 46;
    sp.last_input_tick = 120;
    server_pending_input_ack_reset(&pending);
    ASSERT(server_pending_input_ack_note(&pending, &sp, 45, 120));
    ASSERT(server_emit_pending_input_ack_adaptive(
        &pending, &sp, 2, true, packet_capture_sink, &cap));
    ASSERT_EQ_INT(cap.count, 2);
    ASSERT_EQ_INT(cap.type[1], NET_MSG_STATE);
    ASSERT_EQ_INT(cap.len[1], NET_STATE_AUTH_SIZE);
}

TEST(test_pending_input_ack_adaptive_promotes_drifted_state) {
    SERVER_PLAYER_DECL(sp);
    sp.id = 2;
    sp.ship->pos = v2(0.0f, 0.0f);
    memset(sp.ship->towed_fragments, -1, sizeof(sp.ship->towed_fragments));
    server_player_note_authoritative_ack_state(&sp, 100);
    sp.last_input_seq = 45;
    sp.last_input_tick = 112;
    sp.ship->pos.x = 96.0f;

    server_pending_input_ack_t pending;
    server_pending_input_ack_reset(&pending);
    ASSERT(server_pending_input_ack_note(&pending, &sp, 44, 112));

    packet_capture_t cap;
    memset(&cap, 0, sizeof(cap));
    ASSERT(server_emit_pending_input_ack_adaptive(
        &pending, &sp, 2, false, packet_capture_sink, &cap));
    ASSERT_EQ_INT(cap.count, 1);
    ASSERT_EQ_INT(cap.type[0], NET_MSG_STATE);
    ASSERT_EQ_INT(cap.len[0], NET_STATE_AUTH_SIZE);
    ASSERT_EQ_INT((int)sp.replication->input_ack_state_tick, 112);
}

TEST(test_pending_input_ack_adaptive_promotes_heartbeat_state) {
    ASSERT_EQ_INT((int)INPUT_ACK_STATE_HEARTBEAT_TICKS, 960);

    SERVER_PLAYER_DECL(sp);
    sp.id = 2;
    sp.ship->pos = v2(0.0f, 0.0f);
    memset(sp.ship->towed_fragments, -1, sizeof(sp.ship->towed_fragments));
    server_player_note_authoritative_ack_state(&sp, 100);

    packet_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    sp.last_input_seq = 45;
    sp.last_input_tick = 199;
    server_pending_input_ack_t pending;
    server_pending_input_ack_reset(&pending);
    ASSERT(server_pending_input_ack_note(
        &pending, &sp, 44,
        100u + INPUT_ACK_STATE_HEARTBEAT_TICKS - 1u));
    ASSERT(server_emit_pending_input_ack_adaptive(
        &pending, &sp, 2, false, packet_capture_sink, &cap));
    ASSERT_EQ_INT(cap.count, 1);
    ASSERT_EQ_INT(cap.type[0], NET_MSG_INPUT_APPLIED);

    sp.last_input_seq = 46;
    sp.last_input_tick = 200;
    server_pending_input_ack_reset(&pending);
    ASSERT(server_pending_input_ack_note(
        &pending, &sp, 45, 100u + INPUT_ACK_STATE_HEARTBEAT_TICKS));
    ASSERT(server_emit_pending_input_ack_adaptive(
        &pending, &sp, 2, false, packet_capture_sink, &cap));
    ASSERT_EQ_INT(cap.count, 2);
    ASSERT_EQ_INT(cap.type[1], NET_MSG_STATE);
    ASSERT_EQ_INT(cap.len[1], NET_STATE_AUTH_SIZE);
}

TEST(test_world_snapshot_emitter_sequence_shared) {
    WORLD_DECL;
    test_world_bind_ship_slots(&w);
    w.players[0].connected = true;
    w.players[0].id = 0;
    w.players[0].ship->pos = v2(0.0f, 0.0f);
    w.asteroids[2].active = true;
    w.asteroids[2].pos = v2(100.0f, 0.0f);
    w.asteroids[2].net_dirty = true;
    w.tick = 77;
    w.time = 12.5f;

    static server_world_snapshot_scratch_t scratch;
    packet_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    server_emit_world_snapshot_for_player(&w, 0, true,
                                          packet_capture_sink, &cap,
                                          &scratch);

    ASSERT_EQ_INT(cap.count, 5);
    ASSERT_EQ_INT(cap.type[0], NET_MSG_WORLD_ASTEROIDS8_Q);
    ASSERT_EQ_INT(cap.type[1], NET_MSG_WORLD_PLAYERS);
    ASSERT_EQ_INT(cap.type[2], NET_MSG_WORLD_NPCS);
    ASSERT_EQ_INT(cap.type[3], NET_MSG_WORLD_INTERACTIONS_Q);
    ASSERT_EQ_INT(cap.type[4], NET_MSG_WORLD_TIME);
    ASSERT_EQ_INT(cap.len[0],
                  ASTEROID8_Q_MSG_HEADER + ASTEROID8_Q_RECORD_SIZE);
    ASSERT_EQ_INT(cap.len[1], 2);
    ASSERT_EQ_INT(cap.len[2], 2);
    ASSERT_EQ_INT(cap.len[3], 2);
    ASSERT_EQ_INT(cap.len[4], 5);

    ASSERT(w.asteroids[2].net_dirty);
    server_clear_asteroid_net_dirty(&w);
    ASSERT(!w.asteroids[2].net_dirty);
}

TEST(test_world_snapshot_emits_compact_asteroid_motion_stream) {
    WORLD_DECL;
    test_world_bind_ship_slots(&w);
    w.players[0].connected = true;
    w.players[0].id = 0;
    w.players[0].ship->pos = v2(0.0f, 0.0f);
    w.players[0].replication->asteroid_sent[2] = true;
    w.players[0].replication->asteroid_motion_sent_tick[2] = 100u;
    w.players[0].replication->asteroid_motion_sent_pos[2] = v2(0.0f, 0.0f);
    w.players[0].replication->asteroid_motion_sent_vel[2] = v2(2.0f, 0.0f);
    w.asteroids[2].active = true;
    w.asteroids[2].pos = v2(50.0f, 60.0f);
    w.asteroids[2].vel = v2(2.0f, 0.0f);
    w.asteroids[2].radius = 12.0f;
    w.asteroids[2].hp = 20.0f;
    w.players[0].replication->asteroid_identity_sent_sig[2] =
        asteroid_identity_signature(&w.asteroids[2]);
    w.tick = 100u + ASTEROID_NET_MOVING_REPEAT_TICKS;
    w.time = 12.5f;

    static server_world_snapshot_scratch_t scratch;
    packet_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    server_emit_world_snapshot_for_player(&w, 0, false,
                                          packet_capture_sink, &cap,
                                          &scratch);

    ASSERT_EQ_INT(cap.count, 4);
    ASSERT_EQ_INT(cap.type[0], NET_MSG_WORLD_ASTEROID_POSD8_Q);
    ASSERT_EQ_INT(cap.len[0],
                  ASTEROID_POSD8_Q_MSG_HEADER + ASTEROID_POSD8_Q_RECORD_SIZE);
    ASSERT_EQ_INT((int)w.players[0].replication->asteroid_motion_sent_tick[2],
                  100 + ASTEROID_NET_MOVING_REPEAT_TICKS);
    ASSERT_EQ_INT(cap.type[1], NET_MSG_WORLD_NPCS);
    ASSERT_EQ_INT(cap.type[3], NET_MSG_WORLD_TIME);
}

TEST(test_world_snapshot_prioritizes_local_towed_asteroid_identity) {
    WORLD_DECL;
    test_world_bind_ship_slots(&w);
    w.players[0].connected = true;
    w.players[0].id = 0;
    w.players[0].ship->pos = v2(0.0f, 0.0f);
    w.players[0].ship->towed_fragments[0] = 2;
    w.players[0].ship->towed_count = 1;
    w.players[0].replication->asteroid_sent[2] = true;
    w.players[0].replication->asteroid_motion_sent_tick[2] = 100u;
    w.players[0].replication->asteroid_motion_sent_pos[2] = v2(40.0f, 0.0f);
    w.players[0].replication->asteroid_motion_sent_vel[2] = v2(2.0f, 0.0f);

    w.asteroids[2].active = true;
    w.asteroids[2].tier = ASTEROID_TIER_S;
    w.asteroids[2].last_towed_by = 0;
    w.asteroids[2].pos = v2(50.0f, 0.0f);
    w.asteroids[2].vel = v2(2.0f, 0.0f);
    w.asteroids[2].radius = 12.0f;
    w.asteroids[2].hp = 20.0f;
    w.tick = 105u;
    w.time = 12.5f;

    static server_world_snapshot_scratch_t scratch;
    packet_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    server_emit_world_snapshot_for_player(&w, 0, false,
                                          packet_capture_sink, &cap,
                                          &scratch);

    ASSERT_EQ_INT(cap.count, 4);
    ASSERT_EQ_INT(cap.type[0], NET_MSG_WORLD_ASTEROIDS8_Q);
    ASSERT_EQ_INT(cap.len[0],
                  ASTEROID8_Q_MSG_HEADER + ASTEROID8_Q_RECORD_SIZE);
    ASSERT(w.players[0].replication->asteroid_sent[2]);
    ASSERT_EQ_INT((int)w.players[0].replication->asteroid_motion_sent_tick[2],
                  (int)w.tick);

    memset(&cap, 0, sizeof(cap));
    w.tick += 1u;
    w.time += SIM_DT;
    w.asteroids[2].pos = v2(50.5f, 0.0f);
    server_emit_world_snapshot_for_player(&w, 0, false,
                                          packet_capture_sink, &cap,
                                          &scratch);

    ASSERT(!packet_capture_has_type(&cap, NET_MSG_WORLD_ASTEROIDS));
    ASSERT(!packet_capture_has_type(&cap, NET_MSG_WORLD_ASTEROIDS_Q));
    ASSERT(!packet_capture_has_type(&cap, NET_MSG_WORLD_ASTEROIDS8_Q));
    ASSERT(packet_capture_has_type(&cap, NET_MSG_WORLD_ASTEROID_MOTION) ||
           packet_capture_has_type(&cap, NET_MSG_WORLD_ASTEROID_MOTION_Q) ||
           packet_capture_has_type(&cap, NET_MSG_WORLD_ASTEROID_POS_Q) ||
           packet_capture_has_type(&cap, NET_MSG_WORLD_ASTEROID_POS8_Q) ||
           packet_capture_has_type(&cap, NET_MSG_WORLD_ASTEROID_POSD_Q) ||
           packet_capture_has_type(&cap, NET_MSG_WORLD_ASTEROID_POSD8_Q));
    ASSERT(w.players[0].replication->asteroid_sent[2]);
    ASSERT_EQ_INT((int)w.players[0].replication->asteroid_motion_sent_tick[2],
                  (int)w.tick);
}

TEST(test_world_snapshot_defers_asteroids_while_docked) {
    WORLD_DECL;
    test_world_bind_ship_slots(&w);
    w.players[0].connected = true;
    w.players[0].id = 0;
    w.players[0].docked = true;
    w.players[0].ship->pos = v2(0.0f, 0.0f);
    w.asteroids[2].active = true;
    w.asteroids[2].pos = v2(100.0f, 0.0f);
    w.asteroids[2].net_dirty = true;
    w.tick = 77;
    w.time = 12.5f;

    static server_world_snapshot_scratch_t scratch;
    packet_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    server_emit_world_snapshot_for_player(&w, 0, false,
                                          packet_capture_sink, &cap,
                                          &scratch);

    for (int i = 0; i < cap.count; i++) {
        ASSERT(cap.type[i] != NET_MSG_WORLD_ASTEROIDS);
        ASSERT(cap.type[i] != NET_MSG_WORLD_ASTEROIDS_Q);
        ASSERT(cap.type[i] != NET_MSG_WORLD_ASTEROIDS8_Q);
        ASSERT(cap.type[i] != NET_MSG_WORLD_ASTEROID_MOTION);
        ASSERT(cap.type[i] != NET_MSG_WORLD_ASTEROID_MOTION_Q);
        ASSERT(cap.type[i] != NET_MSG_WORLD_ASTEROID_POS_Q);
        ASSERT(cap.type[i] != NET_MSG_WORLD_ASTEROID_POS8_Q);
    }
    ASSERT(!w.players[0].replication->asteroid_sent[2]);

    memset(&cap, 0, sizeof(cap));
    w.players[0].docked = false;
    w.scaffolds[3].pos.x += 8.0f;
    server_emit_world_snapshot_for_player(&w, 0, false,
                                          packet_capture_sink, &cap,
                                          &scratch);

    ASSERT_EQ_INT(cap.type[0], NET_MSG_WORLD_ASTEROIDS8_Q);
    ASSERT_EQ_INT(cap.len[0],
                  ASTEROID8_Q_MSG_HEADER + ASTEROID8_Q_RECORD_SIZE);
    ASSERT(w.players[0].replication->asteroid_sent[2]);
}

TEST(test_world_snapshot_defers_live_drift_while_docked) {
    WORLD_DECL;
    test_world_bind_ship_slots(&w);
    w.players[0].connected = true;
    w.players[0].id = 0;
    w.players[0].docked = true;
    w.players[0].ship->pos = v2(0.0f, 0.0f);
    w.tick = 120;
    w.time = 12.5f;

    w.npc_ships[1].active = true;
    w.npc_ships[1].role = NPC_ROLE_MINER;
    w.npc_ships[1].state = NPC_STATE_MINING;
    w.npc_ships[1].ship->pos = v2(120.0f, -80.0f);
    w.npc_ships[1].ship->vel = v2(4.0f, -6.0f);
    w.npc_ships[1].ship->angle = 0.75f;
    w.npc_ships[1].thrusting = true;
    w.npc_ships[1].target_asteroid = 7;

    w.scaffolds[3].active = true;
    w.scaffolds[3].state = SCAFFOLD_LOOSE;
    w.scaffolds[3].module_type = MODULE_DOCK;
    w.scaffolds[3].pos = v2(100.0f, 100.0f);
    w.scaffolds[3].vel = v2(2.0f, -1.0f);

    w.cargo_pods[5].active = true;
    w.cargo_pods[5].kind = CARGO_POD_CARGO;
    w.cargo_pods[5].commodity = COMMODITY_FERRITE_INGOT;
    w.cargo_pods[5].pos = v2(10.0f, -20.0f);
    w.cargo_pods[5].vel = v2(1.25f, -2.5f);
    w.cargo_pods[5].rotation = 0.5f;

    w.interactions.count = 1;
    w.interactions.items[0] = (sim_interaction_t){
        .type = SIM_INTERACTION_TRACTOR_BEAM,
        .visual = SIM_INTERACTION_VISUAL_CARGO_POD_MODULE_TRACTOR,
        .commodity = COMMODITY_FERRITE_INGOT,
        .source = { .type = SIM_INTERACTION_ENTITY_STATION_MODULE,
                    .index = 1, .aux = 2 },
        .target = { .type = SIM_INTERACTION_ENTITY_CARGO_POD,
                    .index = 5, .aux = -1 },
        .source_pos = { 10.0f, 20.0f },
        .target_pos = { 100.0f, 200.0f },
        .range = 512.0f,
        .intensity = 0.75f,
    };

    static server_world_snapshot_scratch_t scratch;
    packet_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    server_emit_world_snapshot_for_player(&w, 0, false,
                                          packet_capture_sink, &cap,
                                          &scratch);

    ASSERT(packet_capture_has_type(&cap, NET_MSG_WORLD_NPCS));
    ASSERT(packet_capture_has_type(&cap, NET_MSG_WORLD_SCAFFOLDS));
    ASSERT(packet_capture_has_type(&cap, NET_MSG_WORLD_CARGO_PODS_Q));
    ASSERT(packet_capture_has_type(&cap, NET_MSG_WORLD_INTERACTIONS_Q));
    ASSERT(!packet_capture_has_type(&cap, NET_MSG_WORLD_NPC_MOTION8_Q));
    ASSERT(!packet_capture_has_type(&cap, NET_MSG_WORLD_NPC_STATUS8_Q));
    ASSERT(!packet_capture_has_type(&cap, NET_MSG_WORLD_SCAFFOLD_MOTION_Q));
    ASSERT(!packet_capture_has_type(&cap, NET_MSG_WORLD_CARGO_POD_MOTION_Q));
    ASSERT(!packet_capture_has_type(&cap, NET_MSG_WORLD_INTERACTION_DRIFT));

    memset(&cap, 0, sizeof(cap));
    w.players[0].docked = false;
    server_emit_world_snapshot_for_player(&w, 0, false,
                                          packet_capture_sink, &cap,
                                          &scratch);

    ASSERT(packet_capture_has_type(&cap, NET_MSG_WORLD_NPC_MOTION8_Q));
    ASSERT(packet_capture_has_type(&cap, NET_MSG_WORLD_NPC_STATUS8_Q));
    ASSERT(packet_capture_has_type(&cap, NET_MSG_WORLD_CARGO_POD_MOTION_Q));
    ASSERT(packet_capture_has_type(&cap, NET_MSG_WORLD_INTERACTION_DRIFT));
}

TEST(test_world_time_snapshot_reconciles_at_low_cadence) {
    ASSERT_EQ_INT((int)WORLD_TIME_REPEAT_TICKS, 240);

    WORLD_DECL;
    test_world_bind_ship_slots(&w);
    w.players[0].connected = true;
    w.players[0].id = 0;
    w.tick = 100u;
    w.time = 12.5f;

    static server_world_snapshot_scratch_t scratch;
    packet_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    server_emit_world_snapshot_for_player(&w, 0, false,
                                          packet_capture_sink, &cap,
                                          &scratch);
    ASSERT_EQ_INT(cap.count, 3);
    ASSERT_EQ_INT(cap.type[2], NET_MSG_WORLD_TIME);
    ASSERT(w.players[0].replication->world_time_sent);
    ASSERT_EQ_INT((int)w.players[0].replication->world_time_last_sent_tick, 100);

    memset(&cap, 0, sizeof(cap));
    w.tick = 100u + WORLD_TIME_REPEAT_TICKS - 1u;
    w.time = 13.4f;
    server_emit_world_snapshot_for_player(&w, 0, false,
                                          packet_capture_sink, &cap,
                                          &scratch);
    ASSERT_EQ_INT(cap.count, 2);
    for (int i = 0; i < cap.count; i++)
        ASSERT(cap.type[i] != NET_MSG_WORLD_TIME);
    ASSERT_EQ_INT((int)w.players[0].replication->world_time_last_sent_tick, 100);

    memset(&cap, 0, sizeof(cap));
    w.tick = 100u + WORLD_TIME_REPEAT_TICKS;
    w.time = 13.5f;
    server_emit_world_snapshot_for_player(&w, 0, false,
                                          packet_capture_sink, &cap,
                                          &scratch);
    ASSERT_EQ_INT(cap.count, 3);
    ASSERT_EQ_INT(cap.type[2], NET_MSG_WORLD_TIME);
    ASSERT_EQ_INT((int)w.players[0].replication->world_time_last_sent_tick,
                  100 + WORLD_TIME_REPEAT_TICKS);
}

TEST(test_private_snapshot_emitter_sequence_shared) {
    WORLD_DECL;
    test_world_bind_ship_slots(&w);
    w.players[0].connected = true;
    w.players[0].id = 0;
    w.players[0].ship->hull = 88.0f;
    server_player_note_authoritative_ack_state(&w.players[0], w.tick);
    ASSERT(ship_manifest_bootstrap(w.players[0].ship));

    static server_private_snapshot_scratch_t scratch;
    packet_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    server_emit_private_snapshot_for_player(&w, 0, true,
                                            packet_capture_sink, &cap,
                                            &scratch);

    ASSERT_EQ_INT(cap.count, 7);
    ASSERT_EQ_INT(cap.type[0], NET_MSG_PLAYER_SHIP);
    ASSERT_EQ_INT(cap.type[1], NET_MSG_HOLD_INGOTS);
    ASSERT_EQ_INT(cap.type[2], NET_MSG_PLAYER_MANIFEST);
    ASSERT_EQ_INT(cap.type[3], NET_MSG_INSPECT_SNAPSHOT);
    ASSERT_EQ_INT(cap.type[4], NET_MSG_PLAYER_KNOWN_CONTRACTS);
    ASSERT_EQ_INT(cap.type[5], NET_MSG_PLAYER_KNOWN_LEDGER);
    ASSERT_EQ_INT(cap.type[6], NET_MSG_DELIVERY_LEDGER);
    ASSERT(cap.len[0] > 16);
    ASSERT(cap.len[1] >= HOLD_INGOTS_HEADER);
    ASSERT(cap.len[2] >= PLAYER_MANIFEST_HEADER);
    ASSERT(cap.len[3] > 0);
    ASSERT_EQ_INT(cap.len[4], 5);
    ASSERT(cap.len[5] >= PLAYER_KNOWN_LEDGER_HEADER);
    ASSERT(cap.len[6] >= DELIVERY_LEDGER_HEADER);
}

TEST(test_private_snapshot_emits_local_authoritative_baseline) {
    WORLD_DECL;
    test_world_bind_ship_slots(&w);
    w.players[0].connected = true;
    w.players[0].session_ready = true;
    w.players[0].id = 0;
    w.players[0].ship->pos = v2(10.0f, 20.0f);
    w.players[0].ship->vel = v2(1.0f, 2.0f);
    w.players[0].ship->angle = 0.5f;
    w.players[0].last_input_seq = 77;
    w.players[0].last_input_tick = 1001u;
    w.tick = 1234u;
    ASSERT(ship_manifest_bootstrap(w.players[0].ship));

    static server_private_snapshot_scratch_t scratch;
    packet_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    server_emit_private_snapshot_for_player(&w, 0, true,
                                            packet_capture_sink, &cap,
                                            &scratch);

    ASSERT_EQ_INT(cap.count, 8);
    ASSERT_EQ_INT(cap.type[0], NET_MSG_STATE);
    ASSERT_EQ_INT(cap.len[0], NET_STATE_AUTH_SIZE);
    ASSERT_EQ_INT(cap.type[1], NET_MSG_PLAYER_SHIP);
    ASSERT(w.players[0].replication->input_ack_state_valid);
    ASSERT_EQ_INT((int)w.players[0].replication->input_ack_state_tick, 1234);
}

TEST(test_private_snapshot_emits_idle_authoritative_heartbeat) {
    WORLD_DECL;
    test_world_bind_ship_slots(&w);
    server_player_t *sp = &w.players[0];
    sp->connected = true;
    sp->session_ready = true;
    sp->id = 0;
    sp->last_input_seq = 77;
    sp->last_input_tick = 1001u;
    ASSERT(ship_manifest_bootstrap(sp->ship));
    server_player_note_authoritative_ack_state(sp, 100u);

    static server_private_snapshot_scratch_t scratch;
    packet_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    /* Private snapshots are not input ACKs. Even a large pose delta must
     * wait for the heartbeat so it cannot invent a replay baseline. */
    sp->ship->pos = v2(INPUT_ACK_STATE_POS_ERROR_SQ, 0.0f);
    w.tick = 100u + INPUT_ACK_STATE_HEARTBEAT_TICKS - 1u;
    server_emit_private_snapshot_for_player(
        &w, 0, true, packet_capture_sink, &cap, &scratch);
    ASSERT(!packet_capture_has_type(&cap, NET_MSG_STATE));
    ASSERT_EQ_INT((int)sp->replication->input_ack_state_tick, 100);

    memset(&cap, 0, sizeof(cap));
    w.tick = 100u + INPUT_ACK_STATE_HEARTBEAT_TICKS;
    server_emit_private_snapshot_for_player(
        &w, 0, false, packet_capture_sink, &cap, &scratch);
    ASSERT(!packet_capture_has_type(&cap, NET_MSG_STATE));
    ASSERT_EQ_INT((int)sp->replication->input_ack_state_tick, 100);

    memset(&cap, 0, sizeof(cap));
    server_emit_private_snapshot_for_player(
        &w, 0, true, packet_capture_sink, &cap, &scratch);
    ASSERT_EQ_INT(cap.type[0], NET_MSG_STATE);
    ASSERT_EQ_INT(cap.len[0], NET_STATE_AUTH_SIZE);
    ASSERT_EQ_INT((int)sp->replication->input_ack_state_tick,
                  100 + INPUT_ACK_STATE_HEARTBEAT_TICKS);
}

TEST(test_station_snapshot_emitter_sequence_shared) {
    WORLD_DECL;
    test_world_bind_ship_slots(&w);
    w.station_count = 1;
    w.stations[0].id = 1;
    snprintf(w.stations[0].name, sizeof(w.stations[0].name), "Test Station");
    w.stations[0].dock_radius = 120.0f;
    w.stations[0].signal_range = 1000.0f;
    ASSERT(station_manifest_bootstrap(&w.stations[0]));

    static server_station_snapshot_scratch_t scratch;
    packet_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    server_emit_station_snapshot(&w, true, packet_capture_sink, &cap,
                                 &scratch);

    ASSERT_EQ_INT(cap.count, 5);
    ASSERT_EQ_INT(cap.type[0], NET_MSG_STATION_IDENTITY);
    ASSERT_EQ_INT(cap.type[1], NET_MSG_STATION_DIAG);
    ASSERT_EQ_INT(cap.type[2], NET_MSG_STATION_INGOTS);
    ASSERT_EQ_INT(cap.type[3], NET_MSG_STATION_MANIFEST);
    ASSERT_EQ_INT(cap.type[4], NET_MSG_WORLD_STATIONS);
    ASSERT(cap.len[0] >= STATION_IDENTITY_SIZE);
    ASSERT_EQ_INT(cap.len[1], STATION_DIAG_SIZE);
    ASSERT(cap.len[2] >= STATION_INGOTS_HEADER);
    ASSERT(cap.len[3] >= STATION_MANIFEST_HEADER);
    ASSERT_EQ_INT(cap.len[4], 2 + STATION_RECORD_SIZE);
}

TEST(test_fracture_update_emitter_shared) {
    WORLD_DECL;
    test_world_bind_ship_slots(&w);
    w.stations[0].signal_range = 1000.0f;
    w.stations[0].signal_connected = true;
    w.players[0].connected = true;
    w.players[0].session_ready = true;
    w.players[0].ship->pos = v2(0.0f, 0.0f);
    w.players[1].connected = true;
    w.players[1].session_ready = true;
    w.players[1].ship->pos = v2(100000.0f, 0.0f);
    w.asteroids[3].active = true;
    w.asteroids[3].pos = v2(100.0f, 0.0f);
    w.asteroids[3].hp = 50.0f;
    w.fracture_claims[3].challenge_dirty = true;
    w.fracture_claims[3].fracture_id = 1234u;
    w.fracture_claims[3].deadline_ms = 5000u;
    w.fracture_claims[3].burst_cap = 17u;
    w.fracture_claims[3].resolved_dirty = true;
    memset(w.asteroids[3].fragment_pub, 0x11,
           sizeof(w.asteroids[3].fragment_pub));
    memset(w.fracture_claims[3].best_player_pub, 0x22,
           sizeof(w.fracture_claims[3].best_player_pub));
    w.fracture_claims[3].best_grade = MINING_GRADE_RARE;

    w.pending_resolves[0].active = true;
    w.pending_resolves[0].fracture_id = 4321u;
    memset(w.pending_resolves[0].fragment_pub, 0x33,
           sizeof(w.pending_resolves[0].fragment_pub));
    memset(w.pending_resolves[0].winner_pub, 0x44,
           sizeof(w.pending_resolves[0].winner_pub));
    w.pending_resolves[0].grade = MINING_GRADE_COMMON;

    packet_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    server_emit_fracture_updates(&w, -1, player_packet_capture_sink, &cap);

    ASSERT_EQ_INT(cap.count, 4);
    ASSERT_EQ_INT(cap.slot[0], 0);
    ASSERT_EQ_INT(cap.type[0], NET_MSG_FRACTURE_CHALLENGE);
    ASSERT_EQ_INT(cap.slot[1], 0);
    ASSERT_EQ_INT(cap.type[1], NET_MSG_FRACTURE_RESOLVED);
    ASSERT_EQ_INT(cap.slot[2], 0);
    ASSERT_EQ_INT(cap.type[2], NET_MSG_FRACTURE_RESOLVED);
    ASSERT_EQ_INT(cap.slot[3], 1);
    ASSERT_EQ_INT(cap.type[3], NET_MSG_FRACTURE_RESOLVED);
    ASSERT_EQ_INT(cap.len[0], FRACTURE_CHALLENGE_SIZE);
    ASSERT_EQ_INT(cap.len[1], FRACTURE_RESOLVED_SIZE);
    ASSERT_EQ_INT(cap.len[2], FRACTURE_RESOLVED_SIZE);
    ASSERT_EQ_INT(cap.len[3], FRACTURE_RESOLVED_SIZE);
    ASSERT(!w.fracture_claims[3].challenge_dirty);
    ASSERT(!w.fracture_claims[3].resolved_dirty);
    ASSERT_EQ_INT(w.pending_resolves[0].tx_count, 1);
    ASSERT(w.pending_resolves[0].active);

    memset(&cap, 0, sizeof(cap));
    server_emit_fracture_updates(&w, 0, player_packet_capture_sink, &cap);
    ASSERT_EQ_INT(cap.count, 0);
}

TEST(test_fracture_challenge_rebroadcast_suppresses_seen_players) {
    WORLD_DECL;
    test_world_bind_ship_slots(&w);
    w.stations[0].signal_range = 1000.0f;
    w.stations[0].signal_connected = true;
    w.players[0].connected = true;
    w.players[0].session_ready = true;
    w.players[0].ship->pos = v2(0.0f, 0.0f);
    w.players[1].connected = true;
    w.players[1].session_ready = true;
    w.players[1].ship->pos = v2(0.0f, 0.0f);
    w.asteroids[3].active = true;
    w.asteroids[3].pos = v2(100.0f, 0.0f);
    w.fracture_claims[3].challenge_dirty = true;
    w.fracture_claims[3].fracture_id = 9001u;
    w.fracture_claims[3].deadline_ms = 5000u;
    w.fracture_claims[3].burst_cap = 17u;

    packet_capture_t cap;
    memset(&cap, 0, sizeof(cap));
    server_emit_fracture_updates(&w, -1, player_packet_capture_sink, &cap);

    ASSERT_EQ_INT(cap.count, 2);
    ASSERT_EQ_INT(cap.slot[0], 0);
    ASSERT_EQ_INT(cap.type[0], NET_MSG_FRACTURE_CHALLENGE);
    ASSERT_EQ_INT(cap.slot[1], 1);
    ASSERT_EQ_INT(cap.type[1], NET_MSG_FRACTURE_CHALLENGE);
    ASSERT_EQ_INT((int)w.players[0].replication->fracture_challenge_sent_id[3], 9001);
    ASSERT_EQ_INT((int)w.players[1].replication->fracture_challenge_sent_id[3], 9001);

    w.fracture_claims[3].challenge_dirty = true;
    memset(&cap, 0, sizeof(cap));
    server_emit_fracture_updates(&w, -1, player_packet_capture_sink, &cap);
    ASSERT_EQ_INT(cap.count, 0);

    w.players[2].connected = true;
    w.players[2].session_ready = true;
    w.players[2].ship->pos = v2(0.0f, 0.0f);
    w.fracture_claims[3].challenge_dirty = true;
    server_emit_fracture_updates(&w, -1, player_packet_capture_sink, &cap);

    ASSERT_EQ_INT(cap.count, 1);
    ASSERT_EQ_INT(cap.slot[0], 2);
    ASSERT_EQ_INT(cap.type[0], NET_MSG_FRACTURE_CHALLENGE);
    ASSERT_EQ_INT((int)w.players[2].replication->fracture_challenge_sent_id[3], 9001);
}

TEST(test_fracture_resolve_retry_suppresses_seen_players) {
    WORLD_DECL;
    test_world_bind_ship_slots(&w);
    w.stations[0].signal_range = 1000.0f;
    w.stations[0].signal_connected = true;
    w.players[0].connected = true;
    w.players[0].session_ready = true;
    w.players[0].ship->pos = v2(0.0f, 0.0f);
    w.players[1].connected = true;
    w.players[1].session_ready = true;
    w.players[1].ship->pos = v2(0.0f, 0.0f);
    w.asteroids[3].active = true;
    w.asteroids[3].pos = v2(100.0f, 0.0f);
    w.fracture_claims[3].resolved_dirty = true;
    w.fracture_claims[3].fracture_id = 9002u;
    memset(w.asteroids[3].fragment_pub, 0x11,
           sizeof(w.asteroids[3].fragment_pub));
    memset(w.fracture_claims[3].best_player_pub, 0x22,
           sizeof(w.fracture_claims[3].best_player_pub));
    w.fracture_claims[3].best_grade = MINING_GRADE_RARE;

    w.pending_resolves[0].active = true;
    w.pending_resolves[0].fracture_id = 9002u;
    memset(w.pending_resolves[0].fragment_pub, 0x11,
           sizeof(w.pending_resolves[0].fragment_pub));
    memset(w.pending_resolves[0].winner_pub, 0x22,
           sizeof(w.pending_resolves[0].winner_pub));
    w.pending_resolves[0].grade = MINING_GRADE_RARE;

    packet_capture_t cap;
    memset(&cap, 0, sizeof(cap));
    server_emit_fracture_updates(&w, -1, player_packet_capture_sink, &cap);

    ASSERT_EQ_INT(cap.count, 2);
    ASSERT_EQ_INT(cap.slot[0], 0);
    ASSERT_EQ_INT(cap.type[0], NET_MSG_FRACTURE_RESOLVED);
    ASSERT_EQ_INT(cap.slot[1], 1);
    ASSERT_EQ_INT(cap.type[1], NET_MSG_FRACTURE_RESOLVED);
    ASSERT_EQ_INT(w.pending_resolves[0].tx_count, 1);
    ASSERT(w.pending_resolves[0].active);

    w.time = 0.2f;
    memset(&cap, 0, sizeof(cap));
    server_emit_fracture_updates(&w, -1, player_packet_capture_sink, &cap);
    ASSERT_EQ_INT(cap.count, 0);
    ASSERT_EQ_INT(w.pending_resolves[0].tx_count, 2);
    ASSERT(w.pending_resolves[0].active);

    w.players[2].connected = true;
    w.players[2].session_ready = true;
    w.players[2].ship->pos = v2(100000.0f, 0.0f);
    w.time = 0.4f;
    server_emit_fracture_updates(&w, -1, player_packet_capture_sink, &cap);

    ASSERT_EQ_INT(cap.count, 1);
    ASSERT_EQ_INT(cap.slot[0], 2);
    ASSERT_EQ_INT(cap.type[0], NET_MSG_FRACTURE_RESOLVED);
    ASSERT_EQ_INT(w.pending_resolves[0].tx_count, 3);
    ASSERT(!w.pending_resolves[0].active);
}

typedef struct {
    int outpost_placed;
    int player_state_change;
    int death;
    int contract_complete;
    int hail_response;
    int structure_dirty;
} sim_event_hook_capture_t;

static void sim_event_count_outpost(void *user, const sim_event_t *ev) {
    (void)ev;
    ((sim_event_hook_capture_t *)user)->outpost_placed++;
}

static void sim_event_count_player_state(void *user, const sim_event_t *ev) {
    (void)ev;
    ((sim_event_hook_capture_t *)user)->player_state_change++;
}

static void sim_event_count_death(void *user, const sim_event_t *ev) {
    (void)ev;
    ((sim_event_hook_capture_t *)user)->death++;
}

static void sim_event_count_contract(void *user, const sim_event_t *ev) {
    (void)ev;
    ((sim_event_hook_capture_t *)user)->contract_complete++;
}

static void sim_event_count_hail(void *user, const sim_event_t *ev) {
    (void)ev;
    ((sim_event_hook_capture_t *)user)->hail_response++;
}

static void sim_event_count_structure(void *user, const sim_event_t *ev) {
    (void)ev;
    ((sim_event_hook_capture_t *)user)->structure_dirty++;
}

TEST(test_sim_event_transport_hooks_cover_freshness_buckets) {
    const server_sim_event_hooks_t hooks = {
        .outpost_placed = sim_event_count_outpost,
        .player_state_change = sim_event_count_player_state,
        .death = sim_event_count_death,
        .contract_complete = sim_event_count_contract,
        .hail_response = sim_event_count_hail,
        .structure_dirty = sim_event_count_structure,
    };
    sim_event_hook_capture_t cap;
    sim_event_t ev;

    memset(&cap, 0, sizeof(cap));
    memset(&ev, 0, sizeof(ev));
    ev.type = SIM_EVENT_OUTPOST_PLACED;
    server_process_sim_event_transport(&ev, &hooks, &cap);
    ASSERT_EQ_INT(cap.outpost_placed, 1);
    ASSERT_EQ_INT(cap.structure_dirty, 1);

    memset(&cap, 0, sizeof(cap));
    sim_event_type_t player_events[] = {
        SIM_EVENT_SELL,
        SIM_EVENT_BUY,
        SIM_EVENT_REPAIR,
        SIM_EVENT_UPGRADE,
        SIM_EVENT_DOCK,
        SIM_EVENT_LAUNCH,
    };
    for (int i = 0; i < (int)(sizeof(player_events) / sizeof(player_events[0])); i++) {
        memset(&ev, 0, sizeof(ev));
        ev.type = player_events[i];
        server_process_sim_event_transport(&ev, &hooks, &cap);
    }
    ASSERT_EQ_INT(cap.player_state_change, 6);

    memset(&cap, 0, sizeof(cap));
    memset(&ev, 0, sizeof(ev));
    ev.type = SIM_EVENT_DEATH;
    server_process_sim_event_transport(&ev, &hooks, &cap);
    ASSERT_EQ_INT(cap.death, 1);

    memset(&ev, 0, sizeof(ev));
    ev.type = SIM_EVENT_CONTRACT_COMPLETE;
    server_process_sim_event_transport(&ev, &hooks, &cap);
    ASSERT_EQ_INT(cap.contract_complete, 1);

    memset(&ev, 0, sizeof(ev));
    ev.type = SIM_EVENT_HAIL_RESPONSE;
    server_process_sim_event_transport(&ev, &hooks, &cap);
    ASSERT_EQ_INT(cap.hail_response, 1);

    sim_event_type_t structure_events[] = {
        SIM_EVENT_OUTPOST_ACTIVATED,
        SIM_EVENT_MODULE_ACTIVATED,
        SIM_EVENT_SCAFFOLD_READY,
    };
    for (int i = 0; i < (int)(sizeof(structure_events) / sizeof(structure_events[0])); i++) {
        memset(&ev, 0, sizeof(ev));
        ev.type = structure_events[i];
        server_process_sim_event_transport(&ev, &hooks, &cap);
    }
    ASSERT_EQ_INT(cap.structure_dirty, 3);

    memset(&cap, 0, sizeof(cap));
    memset(&ev, 0, sizeof(ev));
    ev.type = SIM_EVENT_FRACTURE;
    ASSERT_EQ_INT((int)server_sim_event_effects(&ev), 0);
    server_process_sim_event_transport(&ev, &hooks, &cap);
    ASSERT_EQ_INT(cap.outpost_placed + cap.player_state_change +
                  cap.death + cap.contract_complete +
                  cap.hail_response + cap.structure_dirty, 0);
}

TEST(test_pending_action_result_status_shared) {
    WORLD_DECL;
    world_reset(&w);
    server_player_t *sp = &w.players[0];
    sp->id = 0;
    sp->connected = true;
    sp->nearby_station = 0;
    sp->current_station = 0;
    sp->ship->hull = 50.0f;

    sim_events_t events;
    memset(&events, 0, sizeof(events));
    server_begin_pending_action_result(&w, sp, 11, 22, NET_ACTION_REPAIR);
    events.count = 1;
    events.events[0].player_id = 0;
    events.events[0].type = SIM_EVENT_REPAIR;
    ASSERT_EQ_INT(server_pending_action_result_status(&w, sp, &events),
                  NET_ACTION_RESULT_OK);

    memset(&events, 0, sizeof(events));
    server_begin_pending_action_result(&w, sp, 12, 23, NET_ACTION_DOCK);
    ASSERT_EQ_INT(server_pending_action_result_status(&w, sp, &events),
                  NET_ACTION_RESULT_NOOP);

    sp->docked = true;
    sp->current_station = 0;
    ASSERT_EQ_INT(server_pending_action_result_status(&w, sp, &events),
                  NET_ACTION_RESULT_OK);

    sp->docked = false;
    memset(&events, 0, sizeof(events));
    server_begin_pending_action_result(&w, sp, 13, 24, NET_ACTION_REPAIR);
    events.count = 1;
    events.events[0].player_id = 0;
    events.events[0].type = SIM_EVENT_ORDER_REJECTED;
    ASSERT_EQ_INT(server_pending_action_result_status(&w, sp, &events),
                  NET_ACTION_RESULT_REJECTED);
}

TEST(test_hail_response_serializes_reason_tail) {
    WORLD_DECL;
    world_reset(&w);

    sim_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = SIM_EVENT_HAIL_RESPONSE;
    ev.player_id = 0;
    ev.hail_response.station = 2;
    ev.hail_response.credits = 123.0f;
    ev.hail_response.contract_index = -1;
    ev.hail_response.decision_flags = 0x12345678u;
    ev.hail_response.decision_signal_quality = 0.75f;
    ev.hail_response.decision_candidate_count = 3u;
    ev.hail_response.decision_mode = HAIL_DECISION_MODE_SIGNAL_RANGE;
    ev.hail_response.decision_source_id = 0x1122334455667788ull;

    uint8_t buf[NET_HAIL_RESPONSE_REASON_SIZE] = {0};
    int len = serialize_hail_response_for_world(buf, &w, &ev);

    ASSERT_EQ_INT(len, NET_HAIL_RESPONSE_REASON_SIZE);
    ASSERT_EQ_INT(buf[0], NET_MSG_HAIL_RESPONSE);
    ASSERT_EQ_INT(buf[1], 2);
    ASSERT_EQ_FLOAT(read_f32_le(&buf[2]), 123.0f, 0.001f);
    ASSERT_EQ_INT(buf[6], 0xFF);
    ASSERT_EQ_INT((int)read_u32_le(&buf[7]), 0x12345678);
    ASSERT_EQ_FLOAT(read_f32_le(&buf[11]), 0.75f, 0.001f);
    ASSERT_EQ_INT(buf[15], 3);
    ASSERT_EQ_INT(buf[16], HAIL_DECISION_MODE_SIGNAL_RANGE);
    ASSERT(read_u64_le(&buf[17]) == 0x1122334455667788ull);
}

TEST(test_npc_role_default_hull_mapping_covers_tow) {
    ASSERT_EQ_INT(npc_default_hull_class_for_role(NPC_ROLE_MINER),
                  HULL_CLASS_NPC_MINER);
    ASSERT_EQ_INT(npc_default_hull_class_for_role(NPC_ROLE_HAULER),
                  HULL_CLASS_HAULER);
    ASSERT_EQ_INT(npc_default_hull_class_for_role(NPC_ROLE_TOW),
                  HULL_CLASS_DRONE_TRACTOR);
}

TEST(test_roundtrip_inspect_snapshot_npc_manifest_chain) {
    NPC_SHIP_DECL(npc);
    npc.active = true;
    npc.role = NPC_ROLE_HAULER;
    npc.state = NPC_STATE_TRAVEL_TO_DEST;
    npc.home_station = 0;
    npc.dest_station = 1;

    ship_t ship;
    memset(&ship, 0, sizeof(ship));
    ASSERT(ship_manifest_bootstrap(&ship));

    cargo_unit_t unit;
    memset(&unit, 0, sizeof(unit));
    uint8_t fragment_pub[32] = {0};
    fragment_pub[31] = 0x42;
    ASSERT(hash_ingot(COMMODITY_FERRITE_INGOT, MINING_GRADE_RARE,
                      fragment_pub, 7, &unit));
    unit.prefix_class = (uint8_t)INGOT_PREFIX_H;

    cargo_receipt_chain_t chain;
    memset(&chain, 0, sizeof(chain));
    chain.len = 2;
    memcpy(chain.links[0].cargo_pub, unit.pub, 32);
    memcpy(chain.links[1].cargo_pub, unit.pub, 32);
    memset(chain.links[0].authoring_station, 0xA1, 32);
    memset(chain.links[1].authoring_station, 0xB2, 32);
    chain.links[0].event_id = 7001;
    chain.links[1].event_id = 7002;
    ASSERT(ship_manifest_push_with_chain(&ship, &unit, NULL));
    ship_get_receipts(&ship)->chains[0] = chain;

    uint8_t buf[INSPECT_SNAPSHOT_MAX_SIZE];
    int len = serialize_inspect_snapshot_npc(buf, 3, &npc, &ship);

    ASSERT_EQ_INT(buf[0], NET_MSG_INSPECT_SNAPSHOT);
    ASSERT_EQ_INT(buf[1], INSPECT_TARGET_NPC);
    ASSERT_EQ_INT(buf[2], 3);
    ASSERT_EQ_INT(buf[3], 0xFF);
    ASSERT_EQ_INT(buf[4], NPC_ROLE_HAULER);
    ASSERT_EQ_INT(buf[5], NPC_STATE_TRAVEL_TO_DEST);
    ASSERT_EQ_INT(buf[6], 0);
    ASSERT_EQ_INT(buf[7], 1);
    ASSERT_EQ_INT(buf[8], 1);
    ASSERT_EQ_INT(read_u16_le(&buf[9]), 1);
    ASSERT_EQ_INT(len, INSPECT_SNAPSHOT_HEADER + INSPECT_SNAPSHOT_ROW);

    uint8_t *p = &buf[INSPECT_SNAPSHOT_HEADER];
    ASSERT_EQ_INT(p[0], COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(p[1], MINING_GRADE_RARE);
    ASSERT_EQ_INT(p[2], 2);
    ASSERT(p[3] & INSPECT_ROW_HAS_RECEIPT);
    ASSERT_EQ_INT(read_u16_le(&p[12]), 1);
    uint64_t event_id = 0;
    for (int i = 0; i < 8; i++) event_id |= ((uint64_t)p[4 + i]) << (8 * i);
    ASSERT_EQ_INT((int)event_id, 7002);
    ASSERT(memcmp(&p[14], unit.pub, 32) == 0);
    uint8_t expected_head[32];
    cargo_receipt_hash(&chain.links[1], expected_head);
    ASSERT(memcmp(&p[46], expected_head, 32) == 0);
    ASSERT(memcmp(&p[78], chain.links[0].authoring_station, 32) == 0);
    ASSERT(memcmp(&p[110], chain.links[1].authoring_station, 32) == 0);

    ship_cleanup(&ship);
}

TEST(test_inspect_snapshot_npc_expands_matching_receipt_chain) {
    NPC_SHIP_DECL(npc);
    npc.active = true;
    npc.role = NPC_ROLE_HAULER;
    npc.state = NPC_STATE_DOCKED;
    npc.home_station = 0;
    npc.dest_station = 1;

    ship_t ship;
    memset(&ship, 0, sizeof(ship));
    ASSERT(ship_manifest_bootstrap(&ship));

    cargo_unit_t unit;
    memset(&unit, 0, sizeof(unit));
    uint8_t fragment_pub[32] = {0};
    fragment_pub[31] = 0x67;
    ASSERT(hash_ingot(COMMODITY_FERRITE_INGOT, MINING_GRADE_RARE,
                      fragment_pub, 9, &unit));
    unit.prefix_class = (uint8_t)INGOT_PREFIX_H;

    cargo_receipt_chain_t chain;
    memset(&chain, 0, sizeof(chain));
    chain.len = 2;
    STATION_DECL(author_a);
    STATION_DECL(author_b);
    station_authority_init_seeded(&author_a, 0x7101u, 0);
    station_authority_init_seeded(&author_b, 0x7102u, 1);
    uint8_t recipient_a[32];
    uint8_t recipient_b[32];
    uint8_t origin_pin[32];
    memset(recipient_a, 0xC6, sizeof(recipient_a));
    memset(recipient_b, 0xD7, sizeof(recipient_b));
    memset(origin_pin, 0xA4, sizeof(origin_pin));
    ASSERT(cargo_receipt_issue(&author_a, 1, 7101, unit.pub,
                               recipient_a, origin_pin, &chain.links[0]));
    uint8_t first_hash[32];
    cargo_receipt_hash(&chain.links[0], first_hash);
    ASSERT(cargo_receipt_issue(&author_b, 2, 7102, unit.pub,
                               recipient_b, first_hash, &chain.links[1]));
    ASSERT(ship_manifest_push_with_chain(&ship, &unit, &chain));

    uint8_t expected_head[32];
    cargo_receipt_hash(&chain.links[1], expected_head);
    npc.job_diag_count = 1;
    npc.job_diag_kind[0] = (uint8_t)INSPECT_DIAG_JOB_HAUL;
    npc.job_diag_score[0] = 190;
    npc.job_diag_selected[0] = 255;
    npc.job_diag_source[0] = 0;
    npc.job_diag_dest[0] = 1;
    npc.job_diag_commodity[0] = (uint8_t)COMMODITY_FERRITE_INGOT;
    npc.job_diag_reason[0] = (uint8_t)INSPECT_JOB_REASON_RECEIPT_PROOF;
    npc.job_diag_proof_kind[0] = (uint8_t)INSPECT_JOB_PROOF_CHAIN_ANCHOR;
    memcpy(npc.job_diag_proof_hash[0], expected_head, 32);

    uint8_t buf[INSPECT_SNAPSHOT_MAX_SIZE];
    int len = serialize_inspect_snapshot_npc(buf, 3, &npc, &ship);

    ASSERT_EQ_INT(buf[0], NET_MSG_INSPECT_SNAPSHOT);
    ASSERT_EQ_INT(buf[1], INSPECT_TARGET_NPC);
    ASSERT_EQ_INT(buf[8], 4);
    ASSERT_EQ_INT(read_u16_le(&buf[9]), 1);
    ASSERT_EQ_INT(len, INSPECT_SNAPSHOT_HEADER + 4 * INSPECT_SNAPSHOT_ROW);

    uint8_t *job = &buf[INSPECT_SNAPSHOT_HEADER];
    ASSERT_EQ_INT(job[0], INSPECT_DIAG_JOB_HAUL);
    ASSERT(job[3] & INSPECT_ROW_DIAGNOSTIC);

    uint8_t *receipt = &buf[INSPECT_SNAPSHOT_HEADER + INSPECT_SNAPSHOT_ROW];
    ASSERT_EQ_INT(receipt[0], COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(receipt[1], MINING_GRADE_RARE);
    ASSERT_EQ_INT(receipt[2], 2);
    ASSERT(receipt[3] & INSPECT_ROW_HAS_RECEIPT);
    ASSERT(!(receipt[3] & INSPECT_ROW_DIAGNOSTIC));
    ASSERT_EQ_INT(read_u16_le(&receipt[12]), 1);
    ASSERT(memcmp(&receipt[14], unit.pub, 32) == 0);
    ASSERT(memcmp(&receipt[46], expected_head, 32) == 0);
    ASSERT(memcmp(&receipt[78], chain.links[0].authoring_station, 32) == 0);
    ASSERT(memcmp(&receipt[110], chain.links[1].authoring_station, 32) == 0);

    uint8_t *link0 = receipt + INSPECT_SNAPSHOT_ROW;
    ASSERT_EQ_INT(link0[0], INSPECT_DIAG_RECEIPT_LINK);
    ASSERT_EQ_INT(link0[1], 1);
    ASSERT_EQ_INT(link0[2], 2);
    ASSERT(link0[3] & INSPECT_ROW_DIAGNOSTIC);
    ASSERT(link0[3] & INSPECT_ROW_HAS_RECEIPT);
    ASSERT_EQ_INT((int)read_u64_le(&link0[4]), 7101);
    ASSERT_EQ_INT(read_u16_le(&link0[12]), 1);
    ASSERT(memcmp(&link0[14], unit.pub, 32) == 0);
    uint8_t link0_hash[32];
    cargo_receipt_hash(&chain.links[0], link0_hash);
    ASSERT(memcmp(&link0[46], link0_hash, 32) == 0);
    ASSERT(memcmp(&link0[78], chain.links[0].authoring_station, 32) == 0);
    ASSERT(memcmp(&link0[110], chain.links[0].recipient_pubkey, 32) == 0);

    uint8_t *link1 = link0 + INSPECT_SNAPSHOT_ROW;
    ASSERT_EQ_INT(link1[0], INSPECT_DIAG_RECEIPT_LINK);
    ASSERT_EQ_INT(link1[1], 2);
    ASSERT_EQ_INT(link1[2], 2);
    ASSERT(link1[3] & INSPECT_ROW_DIAGNOSTIC);
    ASSERT(link1[3] & INSPECT_ROW_HAS_RECEIPT);
    ASSERT_EQ_INT((int)read_u64_le(&link1[4]), 7102);
    ASSERT_EQ_INT(read_u16_le(&link1[12]), 2);
    ASSERT(memcmp(&link1[14], unit.pub, 32) == 0);
    ASSERT(memcmp(&link1[46], expected_head, 32) == 0);
    ASSERT(memcmp(&link1[78], chain.links[1].authoring_station, 32) == 0);
    ASSERT(memcmp(&link1[110], chain.links[1].recipient_pubkey, 32) == 0);

    ship_cleanup(&ship);
}

TEST(test_inspect_snapshot_npc_retrieves_matching_station_receipt_chain) {
    NPC_SHIP_DECL(npc);
    npc.active = true;
    npc.role = NPC_ROLE_HAULER;
    npc.state = NPC_STATE_DOCKED;
    npc.home_station = 0;
    npc.dest_station = 1;

    ship_t ship;
    memset(&ship, 0, sizeof(ship));
    ASSERT(ship_manifest_bootstrap(&ship));

    WORLD_DECL;
    world_reset(&w);

    cargo_unit_t unit;
    memset(&unit, 0, sizeof(unit));
    uint8_t fragment_pub[32] = {0};
    fragment_pub[31] = 0x91;
    ASSERT(hash_ingot(COMMODITY_CUPRITE_INGOT, MINING_GRADE_COMMON,
                      fragment_pub, 14, &unit));
    unit.prefix_class = (uint8_t)INGOT_PREFIX_K;

    cargo_receipt_chain_t chain;
    memset(&chain, 0, sizeof(chain));
    uint8_t recipient[32];
    uint8_t origin_pin[32];
    for (int i = 0; i < 32; i++) {
        recipient[i] = (uint8_t)(0x53 + i);
        origin_pin[i] = (uint8_t)(0x91 + i);
    }
    ASSERT(cargo_receipt_issue(&w.stations[1], 1, 7201, unit.pub,
                               recipient, origin_pin, &chain.links[0]));
    chain.len = 1;
    ASSERT(cargo_receipt_chain_verify(chain.links, chain.len, unit.pub) ==
           CARGO_RECEIPT_OK);
    ASSERT(station_manifest_push_with_chain(&w.stations[1], &unit, &chain));

    uint8_t expected_head[32];
    cargo_receipt_hash(&chain.links[0], expected_head);
    npc.job_diag_count = 1;
    npc.job_diag_kind[0] = (uint8_t)INSPECT_DIAG_JOB_HAUL;
    npc.job_diag_score[0] = 190;
    npc.job_diag_selected[0] = 255;
    npc.job_diag_source[0] = 0;
    npc.job_diag_dest[0] = 1;
    npc.job_diag_commodity[0] = (uint8_t)COMMODITY_CUPRITE_INGOT;
    npc.job_diag_reason[0] = (uint8_t)INSPECT_JOB_REASON_RECEIPT_PROOF;
    npc.job_diag_proof_kind[0] = (uint8_t)INSPECT_JOB_PROOF_CHAIN_ANCHOR;
    memcpy(npc.job_diag_proof_hash[0], expected_head, 32);

    uint8_t buf[INSPECT_SNAPSHOT_MAX_SIZE];
    int len = serialize_inspect_snapshot_npc_with_station_receipts(
        buf, 3, &npc, &ship, w.stations, MAX_STATIONS);

    ASSERT_EQ_INT(buf[0], NET_MSG_INSPECT_SNAPSHOT);
    ASSERT_EQ_INT(buf[1], INSPECT_TARGET_NPC);
    ASSERT_EQ_INT(buf[8], 3);
    ASSERT_EQ_INT(read_u16_le(&buf[9]), 0);
    ASSERT_EQ_INT(len, INSPECT_SNAPSHOT_HEADER + 3 * INSPECT_SNAPSHOT_ROW);

    uint8_t *job = &buf[INSPECT_SNAPSHOT_HEADER];
    ASSERT_EQ_INT(job[0], INSPECT_DIAG_JOB_HAUL);
    ASSERT(job[3] & INSPECT_ROW_DIAGNOSTIC);

    uint8_t *receipt = &buf[INSPECT_SNAPSHOT_HEADER + INSPECT_SNAPSHOT_ROW];
    ASSERT_EQ_INT(receipt[0], COMMODITY_CUPRITE_INGOT);
    ASSERT_EQ_INT(receipt[1], MINING_GRADE_COMMON);
    ASSERT_EQ_INT(receipt[2], 1);
    ASSERT(receipt[3] & INSPECT_ROW_HAS_RECEIPT);
    ASSERT(receipt[3] & INSPECT_ROW_STATION_RECEIPT);
    ASSERT(!(receipt[3] & INSPECT_ROW_DIAGNOSTIC));
    ASSERT(memcmp(&receipt[14], unit.pub, 32) == 0);
    ASSERT(memcmp(&receipt[46], expected_head, 32) == 0);

    uint8_t *link0 = receipt + INSPECT_SNAPSHOT_ROW;
    ASSERT_EQ_INT(link0[0], INSPECT_DIAG_RECEIPT_LINK);
    ASSERT_EQ_INT(link0[1], 1);
    ASSERT_EQ_INT(link0[2], 1);
    ASSERT(link0[3] & INSPECT_ROW_DIAGNOSTIC);
    ASSERT(link0[3] & INSPECT_ROW_HAS_RECEIPT);
    ASSERT_EQ_INT((int)read_u64_le(&link0[4]), 7201);

    ship_cleanup(&ship);
}

TEST(test_roundtrip_inspect_snapshot_player_manifest_chain) {
    server_player_t player;
    ship_t player_ship = {0};
    memset(&player, 0, sizeof(player));
    player.ship = &player_ship;
    player.connected = true;
    player.current_station = 2;
    player.nearby_station = 1;
    player.ship->hull_class = HULL_CLASS_HAULER;
    player.ship->hull = 149.6f;
    ASSERT(ship_manifest_bootstrap(player.ship));

    cargo_unit_t unit;
    memset(&unit, 0, sizeof(unit));
    uint8_t fragment_pub[32] = {0};
    fragment_pub[0] = 0x9A;
    ASSERT(hash_ingot(COMMODITY_CUPRITE_INGOT, MINING_GRADE_RATI,
                      fragment_pub, 11, &unit));
    unit.prefix_class = (uint8_t)INGOT_PREFIX_M;

    cargo_receipt_chain_t chain;
    memset(&chain, 0, sizeof(chain));
    chain.len = 1;
    memcpy(chain.links[0].cargo_pub, unit.pub, 32);
    memset(chain.links[0].authoring_station, 0xC3, 32);
    chain.links[0].event_id = 8001;
    ASSERT(ship_manifest_push_with_chain(player.ship, &unit, NULL));
    ship_get_receipts(player.ship)->chains[0] = chain;

    uint8_t buf[INSPECT_SNAPSHOT_MAX_SIZE];
    int len = serialize_inspect_snapshot_player(buf, 5, &player);

    ASSERT_EQ_INT(buf[0], NET_MSG_INSPECT_SNAPSHOT);
    ASSERT_EQ_INT(buf[1], INSPECT_TARGET_PLAYER);
    ASSERT_EQ_INT(buf[2], 5);
    ASSERT_EQ_INT(buf[3], 0xFF);
    ASSERT_EQ_INT(buf[4], HULL_CLASS_HAULER);
    ASSERT_EQ_INT(buf[5], 150);
    ASSERT_EQ_INT(buf[6], 2);
    ASSERT_EQ_INT(buf[7], 1);
    ASSERT_EQ_INT(buf[8], 1);
    ASSERT_EQ_INT(read_u16_le(&buf[9]), 1);
    ASSERT_EQ_INT(len, INSPECT_SNAPSHOT_HEADER + INSPECT_SNAPSHOT_ROW);

    uint8_t *p = &buf[INSPECT_SNAPSHOT_HEADER];
    ASSERT_EQ_INT(p[0], COMMODITY_CUPRITE_INGOT);
    ASSERT_EQ_INT(p[1], MINING_GRADE_RATI);
    ASSERT_EQ_INT(p[2], 1);
    ASSERT(p[3] & INSPECT_ROW_HAS_RECEIPT);
    ASSERT_EQ_INT(read_u16_le(&p[12]), 1);
    ASSERT(memcmp(&p[14], unit.pub, 32) == 0);

    ship_cleanup(player.ship);
}

TEST(test_inspect_snapshot_npc_includes_market_memory_diagnostics) {
    npc_ship_t npc;
    ship_t npc_ship = {0};
    memset(&npc, 0, sizeof(npc));
    npc.ship = &npc_ship;
    npc.active = true;
    npc.role = NPC_ROLE_HAULER;
    npc.state = NPC_STATE_DOCKED;
    npc.home_station = 0;
    npc.dest_station = 1;
    npc.ship->knowledge.capacity = SHIP_KNOWN_ITEM_CAP;
    npc.ship->knowledge.count = 1;

    market_memory_t memory;
    memset(&memory, 0, sizeof(memory));
    memory.active = true;
    memory.memory_kind = (uint8_t)MARKET_MEMORY_DELIVERY_RECEIPT;
    memory.station_a = 3;
    memory.station_b = 1;
    memory.commodity = (uint8_t)COMMODITY_FERRITE_INGOT;
    memory.action = (uint8_t)CONTRACT_DELIVERY;
    memory.confidence = 210;
    memory.salience = 180;
    memory.quantity_hint = 2;
    memory.value_hint = 77;

    knowledge_item_t *item = &npc.ship->knowledge.items[0];
    memset(item, 0, sizeof(*item));
    item->kind = (uint8_t)KNOW_MARKET;
    item->payload_kind = (uint8_t)KNOW_PAYLOAD_MARKET_MEMORY;
    item->confidence = memory.confidence;
    item->salience = memory.salience;
    for (int b = 0; b < 32; b++) {
        item->subject_hash[b] = (uint8_t)(0x10 + b);
        item->chain_anchor[b] = (uint8_t)(0x40 + b);
        item->source_hash[b] = (uint8_t)(0x70 + b);
        item->witness_hash[b] = (uint8_t)(0xA0 + b);
    }
    memcpy(item->payload, &memory, sizeof(memory));

    ship_t ship;
    memset(&ship, 0, sizeof(ship));
    ASSERT(ship_manifest_bootstrap(&ship));

    uint8_t buf[INSPECT_SNAPSHOT_MAX_SIZE];
    int len = serialize_inspect_snapshot_npc(buf, 3, &npc, &ship);

    ASSERT_EQ_INT(buf[0], NET_MSG_INSPECT_SNAPSHOT);
    ASSERT_EQ_INT(buf[1], INSPECT_TARGET_NPC);
    ASSERT_EQ_INT(buf[8], 1);
    ASSERT_EQ_INT(read_u16_le(&buf[9]), 0);
    ASSERT_EQ_INT(len, INSPECT_SNAPSHOT_HEADER + INSPECT_SNAPSHOT_ROW);

    uint8_t *row = &buf[INSPECT_SNAPSHOT_HEADER];
    ASSERT_EQ_INT(row[0], INSPECT_DIAG_DELIVERY_RECEIPT);
    ASSERT_EQ_INT(row[1], 210);
    ASSERT_EQ_INT(row[2], 180);
    ASSERT(row[3] & INSPECT_ROW_DIAGNOSTIC);
    ASSERT(!(row[3] & INSPECT_ROW_GROUPED));
    ASSERT_EQ_INT(read_u16_le(&row[12]), 77);
    ASSERT_EQ_INT(row[4], 3);
    ASSERT_EQ_INT(row[5], 1);
    ASSERT_EQ_INT(row[6], CONTRACT_DELIVERY);
    ASSERT_EQ_INT(row[7], COMMODITY_FERRITE_INGOT);
    for (int b = 0; b < 32; b++) {
        ASSERT_EQ_INT(row[14 + b], (uint8_t)(0x10 + b));
        ASSERT_EQ_INT(row[46 + b], (uint8_t)(0x40 + b));
        ASSERT_EQ_INT(row[78 + b], (uint8_t)(0x70 + b));
        ASSERT_EQ_INT(row[110 + b], (uint8_t)(0xA0 + b));
    }

    ship_cleanup(&ship);
}

TEST(test_inspect_snapshot_npc_expands_matching_job_source_memory) {
    NPC_SHIP_DECL(npc);
    npc.active = true;
    npc.role = NPC_ROLE_HAULER;
    npc.state = NPC_STATE_DOCKED;
    npc.home_station = 0;
    npc.dest_station = 1;
    npc.job_diag_count = 1;
    npc.job_diag_kind[0] = (uint8_t)INSPECT_DIAG_JOB_HAUL;
    npc.job_diag_score[0] = 180;
    npc.job_diag_selected[0] = 255;
    npc.job_diag_source[0] = 0;
    npc.job_diag_dest[0] = 1;
    npc.job_diag_commodity[0] = (uint8_t)COMMODITY_FERRITE_INGOT;
    npc.job_diag_reason[0] = (uint8_t)INSPECT_JOB_REASON_ROUTE_MEMORY;
    npc.job_diag_memory_kind[0] = (uint8_t)MARKET_MEMORY_ROUTE_SUCCESS;
    npc.job_diag_proof_kind[0] = (uint8_t)INSPECT_JOB_PROOF_CHAIN_ANCHOR;
    for (int b = 0; b < 32; b++)
        npc.job_diag_proof_hash[0][b] = (uint8_t)(0xC0 + b);

    npc.ship->knowledge.capacity = SHIP_KNOWN_ITEM_CAP;
    npc.ship->knowledge.count = 2;
    market_memory_t first;
    memset(&first, 0, sizeof(first));
    first.active = true;
    first.memory_kind = (uint8_t)MARKET_MEMORY_DEMAND;
    first.station_a = 2;
    first.station_b = 0xffu;
    first.commodity = (uint8_t)COMMODITY_CUPRITE_INGOT;
    first.action = (uint8_t)CONTRACT_TRACTOR;
    first.confidence = 120;
    first.salience = 90;
    first.value_hint = 11;
    knowledge_item_t *item = &npc.ship->knowledge.items[0];
    memset(item, 0, sizeof(*item));
    item->kind = (uint8_t)KNOW_MARKET;
    item->payload_kind = (uint8_t)KNOW_PAYLOAD_MARKET_MEMORY;
    item->confidence = first.confidence;
    item->salience = first.salience;
    memcpy(item->payload, &first, sizeof(first));

    market_memory_t route;
    memset(&route, 0, sizeof(route));
    route.active = true;
    route.memory_kind = (uint8_t)MARKET_MEMORY_ROUTE_SUCCESS;
    route.station_a = 1;
    route.station_b = 0;
    route.commodity = (uint8_t)COMMODITY_FERRITE_INGOT;
    route.action = (uint8_t)CONTRACT_TRACTOR;
    route.confidence = 230;
    route.salience = 210;
    route.quantity_hint = 3;
    route.value_hint = 88;
    item = &npc.ship->knowledge.items[1];
    memset(item, 0, sizeof(*item));
    item->kind = (uint8_t)KNOW_MARKET;
    item->payload_kind = (uint8_t)KNOW_PAYLOAD_MARKET_MEMORY;
    item->confidence = route.confidence;
    item->salience = route.salience;
    for (int b = 0; b < 32; b++) {
        item->subject_hash[b] = (uint8_t)(0x20 + b);
        item->chain_anchor[b] = (uint8_t)(0xC0 + b);
        item->source_hash[b] = (uint8_t)(0x60 + b);
        item->witness_hash[b] = (uint8_t)(0x90 + b);
    }
    memcpy(item->payload, &route, sizeof(route));

    ship_t ship;
    memset(&ship, 0, sizeof(ship));
    ASSERT(ship_manifest_bootstrap(&ship));

    uint8_t buf[INSPECT_SNAPSHOT_MAX_SIZE];
    int len = serialize_inspect_snapshot_npc(buf, 3, &npc, &ship);

    ASSERT_EQ_INT(buf[8], 3);
    ASSERT_EQ_INT(len, INSPECT_SNAPSHOT_HEADER + 3 * INSPECT_SNAPSHOT_ROW);

    uint8_t *job = &buf[INSPECT_SNAPSHOT_HEADER];
    ASSERT_EQ_INT(job[0], INSPECT_DIAG_JOB_HAUL);

    uint8_t *source = &buf[INSPECT_SNAPSHOT_HEADER + INSPECT_SNAPSHOT_ROW];
    ASSERT_EQ_INT(source[0], INSPECT_DIAG_ROUTE_SUCCESS);
    ASSERT_EQ_INT(source[1], 230);
    ASSERT_EQ_INT(source[2], 210);
    ASSERT_EQ_INT(source[4], 1);
    ASSERT_EQ_INT(source[5], 0);
    ASSERT_EQ_INT(source[6], CONTRACT_TRACTOR);
    ASSERT_EQ_INT(source[7], COMMODITY_FERRITE_INGOT);
    for (int b = 0; b < 32; b++) {
        ASSERT_EQ_INT(source[14 + b], (uint8_t)(0x20 + b));
        ASSERT_EQ_INT(source[46 + b], (uint8_t)(0xC0 + b));
        ASSERT_EQ_INT(source[78 + b], (uint8_t)(0x60 + b));
        ASSERT_EQ_INT(source[110 + b], (uint8_t)(0x90 + b));
    }

    uint8_t *general = &buf[INSPECT_SNAPSHOT_HEADER + 2 * INSPECT_SNAPSHOT_ROW];
    ASSERT_EQ_INT(general[0], INSPECT_DIAG_MARKET_DEMAND);

    ship_cleanup(&ship);
}

TEST(test_inspect_snapshot_npc_includes_job_offer_diagnostics) {
    NPC_SHIP_DECL(npc);
    npc.active = true;
    npc.role = NPC_ROLE_HAULER;
    npc.state = NPC_STATE_DOCKED;
    npc.home_station = 0;
    npc.dest_station = 1;
    npc.job_diag_count = 2;
    npc.job_diag_kind[0] = (uint8_t)INSPECT_DIAG_JOB_HAUL;
    npc.job_diag_score[0] = 212;
    npc.job_diag_selected[0] = 255;
    npc.job_diag_source[0] = 0;
    npc.job_diag_dest[0] = 1;
    npc.job_diag_commodity[0] = (uint8_t)COMMODITY_FERRITE_INGOT;
    npc.job_diag_hint[0] = 25;
    npc.job_diag_factor_value[0] = 201;
    npc.job_diag_factor_demand[0] = 202;
    npc.job_diag_factor_supply[0] = 203;
    npc.job_diag_factor_route[0] = 204;
    npc.job_diag_factor_freshness[0] = 205;
    npc.job_diag_factor_capability[0] = 206;
    npc.job_diag_factor_proof[0] = 207;
    npc.job_diag_factor_hologram[0] = 208;
    npc.job_diag_reason[0] = (uint8_t)INSPECT_JOB_REASON_REMOTE_SUPPLY;
    npc.job_diag_memory_kind[0] = (uint8_t)MARKET_MEMORY_SUPPLY;
    npc.job_diag_memory_hops[0] = 3;
    npc.job_diag_memory_age[0] = 12;
    npc.job_diag_memory_station[0] = 1;
    npc.job_diag_proof_kind[0] = (uint8_t)INSPECT_JOB_PROOF_CHAIN_ANCHOR;
    npc.job_diag_proof_prefix[0][0] = 0xA1;
    npc.job_diag_proof_prefix[0][1] = 0xB2;
    npc.job_diag_proof_prefix[0][2] = 0xC3;
    npc.job_diag_proof_prefix[0][3] = 0xD4;
    for (int b = 0; b < 32; b++)
        npc.job_diag_proof_hash[0][b] = (uint8_t)(0x80 + b);
    npc.job_diag_kind[1] = (uint8_t)INSPECT_DIAG_JOB_MINE;
    npc.job_diag_score[1] = 118;
    npc.job_diag_selected[1] = 96;
    npc.job_diag_source[1] = 0;
    npc.job_diag_dest[1] = 0;
    npc.job_diag_commodity[1] = (uint8_t)COMMODITY_FERRITE_ORE;
    npc.job_diag_hint[1] = 6;

    ship_t ship;
    memset(&ship, 0, sizeof(ship));
    ASSERT(ship_manifest_bootstrap(&ship));

    uint8_t buf[INSPECT_SNAPSHOT_MAX_SIZE];
    int len = serialize_inspect_snapshot_npc(buf, 3, &npc, &ship);

    ASSERT_EQ_INT(buf[0], NET_MSG_INSPECT_SNAPSHOT);
    ASSERT_EQ_INT(buf[1], INSPECT_TARGET_NPC);
    ASSERT_EQ_INT(buf[8], 2);
    ASSERT_EQ_INT(read_u16_le(&buf[9]), 0);
    ASSERT_EQ_INT(len, INSPECT_SNAPSHOT_HEADER + 2 * INSPECT_SNAPSHOT_ROW);

    uint8_t *haul = &buf[INSPECT_SNAPSHOT_HEADER];
    ASSERT_EQ_INT(haul[0], INSPECT_DIAG_JOB_HAUL);
    ASSERT_EQ_INT(haul[1], 212);
    ASSERT_EQ_INT(haul[2], 255);
    ASSERT(haul[3] & INSPECT_ROW_DIAGNOSTIC);
    ASSERT_EQ_INT(read_u16_le(&haul[12]), 25);
    ASSERT_EQ_INT(haul[4], 0);
    ASSERT_EQ_INT(haul[5], 1);
    ASSERT_EQ_INT(haul[6], INSPECT_DIAG_JOB_HAUL);
    ASSERT_EQ_INT(haul[7], COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(haul[14 + INSPECT_JOB_FACTOR_VALUE], 201);
    ASSERT_EQ_INT(haul[14 + INSPECT_JOB_FACTOR_DEMAND], 202);
    ASSERT_EQ_INT(haul[14 + INSPECT_JOB_FACTOR_SUPPLY], 203);
    ASSERT_EQ_INT(haul[14 + INSPECT_JOB_FACTOR_ROUTE], 204);
    ASSERT_EQ_INT(haul[14 + INSPECT_JOB_FACTOR_FRESHNESS], 205);
    ASSERT_EQ_INT(haul[14 + INSPECT_JOB_FACTOR_CAPABILITY], 206);
    ASSERT_EQ_INT(haul[14 + INSPECT_JOB_FACTOR_PROOF], 207);
    ASSERT_EQ_INT(haul[14 + INSPECT_JOB_FACTOR_HOLOGRAM], 208);
    ASSERT_EQ_INT(haul[14 + INSPECT_JOB_META_REASON],
                  INSPECT_JOB_REASON_REMOTE_SUPPLY);
    ASSERT_EQ_INT(haul[14 + INSPECT_JOB_META_MEMORY_KIND],
                  MARKET_MEMORY_SUPPLY);
    ASSERT_EQ_INT(haul[14 + INSPECT_JOB_META_HOPS], 3);
    ASSERT_EQ_INT(haul[14 + INSPECT_JOB_META_AGE], 12);
    ASSERT_EQ_INT(haul[14 + INSPECT_JOB_META_SOURCE_STATION], 1);
    ASSERT_EQ_INT(haul[14 + INSPECT_JOB_META_PROOF_KIND],
                  INSPECT_JOB_PROOF_CHAIN_ANCHOR);
    ASSERT_EQ_INT(haul[14 + INSPECT_JOB_META_PROOF0], 0xA1);
    ASSERT_EQ_INT(haul[14 + INSPECT_JOB_META_PROOF1], 0xB2);
    ASSERT_EQ_INT(haul[14 + INSPECT_JOB_META_PROOF2], 0xC3);
    ASSERT_EQ_INT(haul[14 + INSPECT_JOB_META_PROOF3], 0xD4);
    for (int b = 0; b < 32; b++)
        ASSERT_EQ_INT(haul[46 + b], (uint8_t)(0x80 + b));

    uint8_t *mine = &buf[INSPECT_SNAPSHOT_HEADER + INSPECT_SNAPSHOT_ROW];
    ASSERT_EQ_INT(mine[0], INSPECT_DIAG_JOB_MINE);
    ASSERT_EQ_INT(mine[1], 118);
    ASSERT_EQ_INT(mine[2], 96);
    ASSERT(mine[3] & INSPECT_ROW_DIAGNOSTIC);
    ASSERT_EQ_INT(read_u16_le(&mine[12]), 6);
    ASSERT_EQ_INT(mine[4], 0);
    ASSERT_EQ_INT(mine[5], 0);
    ASSERT_EQ_INT(mine[6], INSPECT_DIAG_JOB_MINE);
    ASSERT_EQ_INT(mine[7], COMMODITY_FERRITE_ORE);

    ship_cleanup(&ship);
}

TEST(test_inspect_snapshot_npc_includes_hnn_trace_diagnostics) {
    NPC_SHIP_DECL(npc);
    npc.active = true;
    npc.role = NPC_ROLE_MINER;
    npc.state = NPC_STATE_TRAVEL_TO_ASTEROID;
    npc.home_station = 0;
    npc.dest_station = 0;
    npc.brain_mode = SERVER_BRAIN_MODE_HOLOGRAPHIC;
    npc.hnn_mem.experience_count = (int)HNN_TRACE_CAPACITY;
    npc.hnn_mem.last_retrieval_similarity = 0.0f;
    npc.hnn_mem.last_margin = 0.02f;

    hnn_memory_contract_t contract = hnn_memory_contract(&npc.hnn_mem);

    ship_t ship;
    memset(&ship, 0, sizeof(ship));
    ASSERT(ship_manifest_bootstrap(&ship));

    uint8_t buf[INSPECT_SNAPSHOT_MAX_SIZE];
    int len = serialize_inspect_snapshot_npc(buf, 3, &npc, &ship);

    ASSERT_EQ_INT(buf[0], NET_MSG_INSPECT_SNAPSHOT);
    ASSERT_EQ_INT(buf[1], INSPECT_TARGET_NPC);
    ASSERT_EQ_INT(buf[8], 1);
    ASSERT_EQ_INT(read_u16_le(&buf[9]), 0);
    ASSERT_EQ_INT(len, INSPECT_SNAPSHOT_HEADER + INSPECT_SNAPSHOT_ROW);

    uint8_t *row = &buf[INSPECT_SNAPSHOT_HEADER];
    ASSERT_EQ_INT(row[0], INSPECT_DIAG_HNN_TRACE);
    ASSERT(row[3] & INSPECT_ROW_DIAGNOSTIC);
    ASSERT_EQ_INT(read_u16_le(&row[12]), HNN_TRACE_CAPACITY);
    ASSERT(read_u64_le(&row[4]) == contract.action_vocabulary_hash);
    ASSERT_EQ_INT(row[1], 255);
    ASSERT_EQ_INT(row[1], row[14 + INSPECT_HNN_TRACE_LOAD]);
    ASSERT_EQ_INT(row[2], row[14 + INSPECT_HNN_TRACE_FIDELITY]);
    ASSERT_EQ_INT(row[14 + INSPECT_HNN_TRACE_MARGIN],
                  inspect_snapshot_compact_signed_unit(contract.last_margin));
    ASSERT(row[14 + INSPECT_HNN_TRACE_SNR] > 0);
    ASSERT(row[14 + INSPECT_HNN_TRACE_FLAGS] &
           INSPECT_HNN_TRACE_WARN_NOISY);
    ASSERT(row[14 + INSPECT_HNN_TRACE_FLAGS] &
           INSPECT_HNN_TRACE_WARN_LOW_MARGIN);
    ASSERT_EQ_INT(row[14 + INSPECT_HNN_TRACE_KEYGEN_VERSION],
                  HNN_KEYGEN_VERSION);
    ASSERT_EQ_INT(row[14 + INSPECT_HNN_TRACE_ENCODER_VERSION],
                  HNN_PILOT_ENCODER_VERSION);
    ASSERT_EQ_INT(row[14 + INSPECT_HNN_TRACE_FORMAT_VERSION],
                  HNN_TRACE_FORMAT_VERSION);
    ASSERT_EQ_INT((uint16_t)row[14 + INSPECT_HNN_TRACE_CAPACITY_LO] |
                  ((uint16_t)row[14 + INSPECT_HNN_TRACE_CAPACITY_HI] << 8),
                  HNN_TRACE_CAPACITY);
    ASSERT_EQ_INT((uint16_t)row[14 + INSPECT_HNN_TRACE_DIM_LO] |
                  ((uint16_t)row[14 + INSPECT_HNN_TRACE_DIM_HI] << 8),
                  HNN_DIM);
    ASSERT(read_u64_le(&row[46]) == contract.seed);
    ASSERT(read_u64_le(&row[54]) == contract.action_vocabulary_hash);
    ASSERT_EQ_INT((int)read_u32_le(&row[62]), HNN_DIM);

    ship_cleanup(&ship);
}

TEST(test_inspect_snapshot_groups_anonymous_ingots_by_grade) {
    NPC_SHIP_DECL(npc);
    npc.active = true;
    npc.role = NPC_ROLE_HAULER;
    npc.state = NPC_STATE_TRAVEL_TO_DEST;
    npc.home_station = 0;
    npc.dest_station = 1;

    ship_t ship;
    memset(&ship, 0, sizeof(ship));
    ASSERT(ship_manifest_bootstrap(&ship));

    uint8_t fragment_pub[32] = {0};
    for (int i = 0; i < 3; i++) {
        cargo_unit_t u;
        memset(&u, 0, sizeof(u));
        fragment_pub[31] = (uint8_t)(0x10 + i);
        ASSERT(hash_ingot(COMMODITY_FERRITE_INGOT, MINING_GRADE_COMMON,
                          fragment_pub, (uint16_t)i, &u));
        u.prefix_class = (uint8_t)INGOT_PREFIX_ANONYMOUS;
        ASSERT(ship_manifest_push_with_chain(&ship, &u, NULL));
    }

    cargo_unit_t named;
    memset(&named, 0, sizeof(named));
    fragment_pub[31] = 0x40;
    ASSERT(hash_ingot(COMMODITY_FERRITE_INGOT, MINING_GRADE_COMMON,
                      fragment_pub, 9, &named));
    named.prefix_class = (uint8_t)INGOT_PREFIX_H;
    ASSERT(ship_manifest_push_with_chain(&ship, &named, NULL));

    for (int i = 0; i < 2; i++) {
        cargo_unit_t u;
        memset(&u, 0, sizeof(u));
        fragment_pub[31] = (uint8_t)(0x70 + i);
        ASSERT(hash_ingot(COMMODITY_FERRITE_INGOT, MINING_GRADE_RARE,
                          fragment_pub, (uint16_t)i, &u));
        u.prefix_class = (uint8_t)INGOT_PREFIX_ANONYMOUS;
        ASSERT(ship_manifest_push_with_chain(&ship, &u, NULL));
    }

    uint8_t buf[INSPECT_SNAPSHOT_MAX_SIZE];
    int len = serialize_inspect_snapshot_npc(buf, 3, &npc, &ship);

    ASSERT_EQ_INT(buf[8], 3);
    ASSERT_EQ_INT(read_u16_le(&buf[9]), 6);
    ASSERT_EQ_INT(len, INSPECT_SNAPSHOT_HEADER + 3 * INSPECT_SNAPSHOT_ROW);

    uint8_t *bulk_common = &buf[INSPECT_SNAPSHOT_HEADER];
    ASSERT_EQ_INT(bulk_common[0], COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(bulk_common[1], MINING_GRADE_COMMON);
    ASSERT(bulk_common[3] & INSPECT_ROW_GROUPED);
    ASSERT(!(bulk_common[3] & INSPECT_ROW_HAS_RECEIPT));
    ASSERT_EQ_INT(read_u16_le(&bulk_common[12]), 3);

    uint8_t *named_common = &buf[INSPECT_SNAPSHOT_HEADER + INSPECT_SNAPSHOT_ROW];
    ASSERT_EQ_INT(named_common[0], COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(named_common[1], MINING_GRADE_COMMON);
    ASSERT(!(named_common[3] & INSPECT_ROW_GROUPED));
    ASSERT_EQ_INT(read_u16_le(&named_common[12]), 1);
    ASSERT(memcmp(&named_common[14], named.pub, 32) == 0);

    uint8_t *bulk_rare = &buf[INSPECT_SNAPSHOT_HEADER + 2 * INSPECT_SNAPSHOT_ROW];
    ASSERT_EQ_INT(bulk_rare[0], COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(bulk_rare[1], MINING_GRADE_RARE);
    ASSERT(bulk_rare[3] & INSPECT_ROW_GROUPED);
    ASSERT_EQ_INT(read_u16_le(&bulk_rare[12]), 2);

    ship_cleanup(&ship);
}

TEST(test_inspect_snapshot_groups_finished_goods_by_grade) {
    NPC_SHIP_DECL(npc);
    npc.active = true;
    npc.role = NPC_ROLE_HAULER;
    npc.state = NPC_STATE_TRAVEL_TO_DEST;
    npc.home_station = 0;
    npc.dest_station = 1;

    ship_t ship;
    memset(&ship, 0, sizeof(ship));
    ASSERT(ship_manifest_bootstrap(&ship));

    const struct {
        cargo_kind_t kind;
        commodity_t commodity;
        int count;
    } buckets[] = {
        { CARGO_KIND_FRAME,   COMMODITY_FRAME,          4 },
        { CARGO_KIND_LASER,   COMMODITY_LASER_MODULE,   2 },
        { CARGO_KIND_TRACTOR, COMMODITY_TRACTOR_MODULE, 3 },
    };
    uint8_t pub_seed = 0x30;
    for (int b = 0; b < 3; b++) {
        for (int i = 0; i < buckets[b].count; i++) {
            cargo_unit_t u;
            memset(&u, 0, sizeof(u));
            u.kind = (uint8_t)buckets[b].kind;
            u.commodity = (uint8_t)buckets[b].commodity;
            u.grade = (uint8_t)MINING_GRADE_FINE;
            u.prefix_class = (uint8_t)INGOT_PREFIX_ANONYMOUS;
            u.quantity = 1;
            u.pub[31] = pub_seed++;
            ASSERT(ship_manifest_push_with_chain(&ship, &u, NULL));
        }
    }

    uint8_t buf[INSPECT_SNAPSHOT_MAX_SIZE];
    int len = serialize_inspect_snapshot_npc(buf, 3, &npc, &ship);

    ASSERT_EQ_INT(buf[8], 3);
    ASSERT_EQ_INT(read_u16_le(&buf[9]), 9);
    ASSERT_EQ_INT(len, INSPECT_SNAPSHOT_HEADER + 3 * INSPECT_SNAPSHOT_ROW);

    uint8_t *frames = &buf[INSPECT_SNAPSHOT_HEADER];
    ASSERT_EQ_INT(frames[0], COMMODITY_FRAME);
    ASSERT_EQ_INT(frames[1], MINING_GRADE_FINE);
    ASSERT(frames[3] & INSPECT_ROW_GROUPED);
    ASSERT_EQ_INT(read_u16_le(&frames[12]), 4);

    uint8_t *lasers = &buf[INSPECT_SNAPSHOT_HEADER + INSPECT_SNAPSHOT_ROW];
    ASSERT_EQ_INT(lasers[0], COMMODITY_LASER_MODULE);
    ASSERT_EQ_INT(lasers[1], MINING_GRADE_FINE);
    ASSERT(lasers[3] & INSPECT_ROW_GROUPED);
    ASSERT_EQ_INT(read_u16_le(&lasers[12]), 2);

    uint8_t *tractors = &buf[INSPECT_SNAPSHOT_HEADER + 2 * INSPECT_SNAPSHOT_ROW];
    ASSERT_EQ_INT(tractors[0], COMMODITY_TRACTOR_MODULE);
    ASSERT_EQ_INT(tractors[1], MINING_GRADE_FINE);
    ASSERT(tractors[3] & INSPECT_ROW_GROUPED);
    ASSERT_EQ_INT(read_u16_le(&tractors[12]), 3);

    ship_cleanup(&ship);
}

TEST(test_inspect_snapshot_keeps_named_ingots_individual) {
    /* Hauler scan should group common anonymous bulk, but every named
     * / prefix-class ingot stays per-unit so the hash and provenance can
     * be inspected. */
    NPC_SHIP_DECL(npc);
    npc.active = true;
    npc.role = NPC_ROLE_HAULER;
    npc.state = NPC_STATE_TRAVEL_TO_DEST;
    npc.home_station = 0;
    npc.dest_station = 1;

    ship_t ship;
    memset(&ship, 0, sizeof(ship));
    ASSERT(ship_manifest_bootstrap(&ship));

    uint8_t fragment_pub[32] = {0};
    cargo_unit_t h_units[3];
    /* Three H-class units at (FERRITE, COMMON). */
    for (int i = 0; i < 3; i++) {
        cargo_unit_t u;
        memset(&u, 0, sizeof(u));
        fragment_pub[31] = (uint8_t)(0xA0 + i);
        ASSERT(hash_ingot(COMMODITY_FERRITE_INGOT, MINING_GRADE_COMMON,
                          fragment_pub, (uint16_t)i, &u));
        u.prefix_class = (uint8_t)INGOT_PREFIX_H;
        h_units[i] = u;
        ASSERT(ship_manifest_push_with_chain(&ship, &u, NULL));
    }

    /* One RATI singleton at the same bucket. */
    cargo_unit_t solo;
    memset(&solo, 0, sizeof(solo));
    fragment_pub[31] = 0xB0;
    ASSERT(hash_ingot(COMMODITY_FERRITE_INGOT, MINING_GRADE_COMMON,
                      fragment_pub, 50, &solo));
    solo.prefix_class = (uint8_t)INGOT_PREFIX_RATI;
    ASSERT(ship_manifest_push_with_chain(&ship, &solo, NULL));

    uint8_t buf[INSPECT_SNAPSHOT_MAX_SIZE];
    int len = serialize_inspect_snapshot_npc(buf, 3, &npc, &ship);

    ASSERT_EQ_INT(buf[8], 4);
    ASSERT_EQ_INT(read_u16_le(&buf[9]), 4);
    ASSERT_EQ_INT(len, INSPECT_SNAPSHOT_HEADER + 4 * INSPECT_SNAPSHOT_ROW);

    for (int i = 0; i < 3; i++) {
        uint8_t *row = &buf[INSPECT_SNAPSHOT_HEADER + i * INSPECT_SNAPSHOT_ROW];
        ASSERT_EQ_INT(row[0], COMMODITY_FERRITE_INGOT);
        ASSERT_EQ_INT(row[1], MINING_GRADE_COMMON);
        ASSERT(!(row[3] & INSPECT_ROW_GROUPED));
        ASSERT_EQ_INT(read_u16_le(&row[12]), 1);
        ASSERT(memcmp(&row[14], h_units[i].pub, 32) == 0);
    }

    /* Row 3: RATI singleton, ungrouped, full pub. */
    uint8_t *single = &buf[INSPECT_SNAPSHOT_HEADER + 3 * INSPECT_SNAPSHOT_ROW];
    ASSERT_EQ_INT(single[0], COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(single[1], MINING_GRADE_COMMON);
    ASSERT(!(single[3] & INSPECT_ROW_GROUPED));
    ASSERT_EQ_INT(read_u16_le(&single[12]), 1);
    ASSERT(memcmp(&single[14], solo.pub, 32) == 0);

    ship_cleanup(&ship);
}

TEST(test_roundtrip_stations) {
    station_t stations[MAX_STATIONS];
    memset(stations, 0, sizeof(stations));

    /* Mark station 0 as active so it gets serialized */
    stations[0].signal_range = 2200.0f;
    stations[0]._inventory_cache[0] = 45.5f;
    stations[0]._inventory_cache[1] = 12.3f;
    stations[0]._inventory_cache[2] = 78.9f;
    ASSERT(station_manifest_bootstrap(&stations[0]));
    ASSERT(station_finished_mint(&stations[0], COMMODITY_FERRITE_INGOT,
                                 20, NULL) == 20);
    ASSERT(station_finished_accumulate(&stations[0], COMMODITY_FRAME,
                                       15.5f, NULL) == 15);

    uint8_t buf[2 + MAX_STATIONS * STATION_RECORD_SIZE];
    int len = serialize_stations(buf, stations);

    ASSERT_EQ_INT(buf[0], NET_MSG_WORLD_STATIONS);
    ASSERT_EQ_INT(buf[1], 1); /* only 1 active station */
    ASSERT_EQ_INT(len, 2 + 1 * STATION_RECORD_SIZE);

    uint8_t *p = &buf[2];
    ASSERT_EQ_INT(p[0], 0);
    /* inventory starts at byte 1, each commodity is 4 bytes */
    ASSERT_EQ_FLOAT(read_f32_le(&p[1 + COMMODITY_FERRITE_ORE * 4]), 45.5f, 0.1f);
    ASSERT_EQ_FLOAT(read_f32_le(&p[1 + COMMODITY_CUPRITE_ORE * 4]), 12.3f, 0.1f);
    ASSERT_EQ_FLOAT(read_f32_le(&p[1 + COMMODITY_CRYSTAL_ORE * 4]), 78.9f, 0.1f);
    ASSERT_EQ_FLOAT(read_f32_le(&p[1 + COMMODITY_FERRITE_INGOT * 4]), 20.0f, 0.1f);
    ASSERT_EQ_FLOAT(read_f32_le(&p[1 + COMMODITY_FRAME * 4]), 15.5f, 0.1f);
}

TEST(test_payload_cache_suppresses_unchanged_world_stations_per_connection) {
    station_t stations[MAX_STATIONS];
    memset(stations, 0, sizeof(stations));
    stations[0].signal_range = 2200.0f;
    stations[0]._inventory_cache[COMMODITY_FERRITE_ORE] = 45.5f;
    ASSERT(station_manifest_bootstrap(&stations[0]));
    ASSERT(station_finished_mint(&stations[0], COMMODITY_FERRITE_INGOT,
                                 20, NULL) == 20);

    uint8_t buf[2 + MAX_STATIONS * STATION_RECORD_SIZE];
    int len = serialize_stations(buf, stations);
    ASSERT_EQ_INT(len, 2 + STATION_RECORD_SIZE);

    net_payload_cache_t cache;
    memset(&cache, 0, sizeof(cache));
    int conn_a = 1;
    int conn_b = 2;

    ASSERT(net_payload_cache_should_send(&cache, &conn_a, buf, (size_t)len));
    ASSERT(!net_payload_cache_should_send(&cache, &conn_a, buf, (size_t)len));

    ASSERT(net_payload_cache_should_send(&cache, &conn_b, buf, (size_t)len));
    ASSERT(!net_payload_cache_should_send(&cache, &conn_b, buf, (size_t)len));

    ASSERT(station_finished_mint(&stations[0], COMMODITY_FERRITE_INGOT,
                                 1, NULL) == 1);
    len = serialize_stations(buf, stations);
    ASSERT(net_payload_cache_should_send(&cache, &conn_b, buf, (size_t)len));
    ASSERT(!net_payload_cache_should_send(&cache, &conn_b, buf, (size_t)len));
}

TEST(test_world_stations_q_omits_zero_inventory_slots) {
    station_t stations[MAX_STATIONS];
    memset(stations, 0, sizeof(stations));
    stations[0].signal_range = 2200.0f;
    stations[0]._inventory_cache[COMMODITY_FERRITE_ORE] = 45.5f;
    ASSERT(station_manifest_bootstrap(&stations[0]));
    ASSERT(station_finished_accumulate(&stations[0], COMMODITY_FRAME,
                                       15.5f, NULL) == 15);
    stations[1].signal_range = 1800.0f;

    uint8_t full[2 + MAX_STATIONS * STATION_RECORD_SIZE];
    int full_len = serialize_stations(full, stations);
    uint8_t q[STATION_Q_MAX_SIZE] = {0};
    int q_len = serialize_stations_q_from_full(q, full, full_len);

    ASSERT_EQ_INT(q[0], NET_MSG_WORLD_STATIONS_Q);
    ASSERT_EQ_INT(q[1], 2);
    ASSERT(q_len < full_len);

    int off = STATION_Q_HEADER_SIZE;
    ASSERT_EQ_INT(q[off++], 0);
    uint16_t mask0 = read_u16_le(&q[off]);
    off += 2;
    ASSERT_EQ_INT(mask0,
                  (1u << COMMODITY_FERRITE_ORE) |
                  (1u << COMMODITY_FRAME));
    ASSERT_EQ_FLOAT(read_f32_le(&q[off]), 45.5f, 0.01f);
    off += 4;
    ASSERT_EQ_FLOAT(read_f32_le(&q[off]), 15.5f, 0.01f);
    off += 4;

    ASSERT_EQ_INT(q[off++], 1);
    ASSERT_EQ_INT(read_u16_le(&q[off]), 0);
    off += 2;
    ASSERT_EQ_INT(off, q_len);
}

TEST(test_station_identity_serializes_module_commodities) {
    station_t st;
    memset(&st, 0, sizeof(st));
    st.services = STATION_SERVICE_REPAIR;
    st.pos = v2(10.0f, -20.0f);
    st.radius = 60.0f;
    st.dock_radius = 110.0f;
    st.signal_range = 2000.0f;
    snprintf(st.name, sizeof(st.name), "Wire Test");
    st.module_count = 3;
    st.modules[0] = (station_module_t){
        .type = MODULE_HOPPER,
        .ring = 1,
        .slot = 0,
        .scaffold = false,
        .commodity = (uint8_t)COMMODITY_CUPRITE_ORE,
        .build_progress = 1.0f,
    };
    st.modules[1] = (station_module_t){
        .type = MODULE_HOPPER,
        .ring = 2,
        .slot = 1,
        .scaffold = false,
        .commodity = (uint8_t)COMMODITY_FRAME,
        .build_progress = 1.0f,
    };
    st.modules[2] = (station_module_t){
        .type = MODULE_FURNACE,
        .ring = 3,
        .slot = 2,
        .scaffold = false,
        .commodity = (uint8_t)COMMODITY_CRYSTAL_INGOT,
        .build_progress = 1.0f,
    };

    uint8_t buf[STATION_IDENTITY_SIZE] = {0};
    int len = serialize_station_identity(buf, 4, &st);

    ASSERT_EQ_INT(len, STATION_IDENTITY_SIZE);
    ASSERT_EQ_INT(STATION_MODULE_RECORD_SIZE, 9);

    int moff = 59 + COMMODITY_COUNT * 4 + 4;
    ASSERT_EQ_INT(buf[moff], 3);
    moff++;
    ASSERT_EQ_INT(buf[moff + 8], COMMODITY_CUPRITE_ORE);
    moff += STATION_MODULE_RECORD_SIZE;
    ASSERT_EQ_INT(buf[moff + 8], COMMODITY_FRAME);
    moff += STATION_MODULE_RECORD_SIZE;
    ASSERT_EQ_INT(buf[moff + 8], COMMODITY_CRYSTAL_INGOT);
}

TEST(test_station_identity_serializes_operator_text) {
    station_t st;
    memset(&st, 0, sizeof(st));
    st.signal_range = 1000.0f;
    snprintf(st.name, sizeof(st.name), "Voice Test");
    snprintf(st.hail_message, sizeof(st.hail_message), "station motd");
    snprintf(st.miner_chatter[3], sizeof(st.miner_chatter[3]), "miner line");
    snprintf(st.hauler_chatter[5], sizeof(st.hauler_chatter[5]), "hauler line");
    snprintf(st.rati_hail_message, sizeof(st.rati_hail_message), "rati line");
    snprintf(st.currency_name, sizeof(st.currency_name), "voice scrip");

    uint8_t buf[STATION_IDENTITY_SIZE] = {0};
    int len = serialize_station_identity(buf, 2, &st);
    ASSERT_EQ_INT(len, STATION_IDENTITY_SIZE);
    ASSERT_EQ_INT(STATION_IDENTITY_SIZE,
                  STATION_IDENTITY_HULL_SIZE + STATION_IDENTITY_FACTION_SIZE +
                      STATION_IDENTITY_POLICY_SIZE);

    int moff = 59 + COMMODITY_COUNT * 4 + 4
        + 1 + MAX_MODULES_PER_STATION * STATION_MODULE_RECORD_SIZE
        + 1 + MAX_ARMS * 4 + MAX_ARMS * 4 + MAX_ARMS * 4 + MAX_ARMS * 4
        + 1 + STATION_PLAN_RECORD_COUNT * STATION_PLAN_RECORD_SIZE
        + 1 + STATION_PENDING_SCAFFOLD_RECORD_COUNT * STATION_PENDING_SCAFFOLD_RECORD_SIZE
        + 1 + STATION_PENDING_SHIP_RECORD_COUNT * STATION_PENDING_SHIP_RECORD_SIZE;

    ASSERT(memcmp(&buf[moff], "station motd", strlen("station motd")) == 0);
    moff += STATION_IDENTITY_HAIL_MESSAGE_LEN;
    ASSERT(memcmp(&buf[moff + 3 * STATION_IDENTITY_CHATTER_LINE_LEN],
                  "miner line", strlen("miner line")) == 0);
    moff += STATION_IDENTITY_CHATTER_LINES * STATION_IDENTITY_CHATTER_LINE_LEN;
    ASSERT(memcmp(&buf[moff + 5 * STATION_IDENTITY_CHATTER_LINE_LEN],
                  "hauler line", strlen("hauler line")) == 0);
    moff += STATION_IDENTITY_CHATTER_LINES * STATION_IDENTITY_CHATTER_LINE_LEN;
    ASSERT(memcmp(&buf[moff], "rati line", strlen("rati line")) == 0);
    moff += STATION_IDENTITY_RATI_HAIL_LEN;
    ASSERT(memcmp(&buf[moff], "voice scrip", strlen("voice scrip")) == 0);
}

TEST(test_station_identity_q_compacts_sparse_text_and_lists) {
    station_t st;
    memset(&st, 0, sizeof(st));
    st.services = 0x12345678u;
    st.signal_range = 1000.0f;
    st.pos = v2(10.0f, -20.0f);
    st.radius = 120.0f;
    st.dock_radius = 90.0f;
    snprintf(st.name, sizeof(st.name), "Compact Station");
    st.base_price[COMMODITY_FERRITE_ORE] = 11.0f;
    st.scaffold_progress = 0.5f;
    st.module_count = 2;
    st.modules[0] = (station_module_t){
        .type = MODULE_DOCK,
        .ring = 1,
        .slot = 2,
        .scaffold = false,
        .commodity = (uint8_t)COMMODITY_COUNT,
        .build_progress = 1.0f,
    };
    st.modules[1] = (station_module_t){
        .type = MODULE_FURNACE,
        .ring = 3,
        .slot = 4,
        .scaffold = true,
        .commodity = (uint8_t)COMMODITY_CUPRITE_ORE,
        .build_progress = 0.25f,
    };
    st.arm_count = 2;
    st.arm_speed[0] = 0.10f;
    st.ring_offset[0] = 0.20f;
    st.arm_rotation[0] = 0.30f;
    st.arm_omega[0] = 0.40f;
    st.arm_speed[1] = 0.50f;
    st.ring_offset[1] = 0.60f;
    st.arm_rotation[1] = 0.70f;
    st.arm_omega[1] = 0.80f;
    st.placement_plan_count = 1;
    st.placement_plans[0].type = MODULE_SIGNAL_RELAY;
    st.placement_plans[0].ring = 2;
    st.placement_plans[0].slot = 5;
    st.placement_plans[0].owner = 3;
    st.pending_scaffold_count = 1;
    st.pending_scaffolds[0].type = MODULE_DOCK;
    st.pending_scaffolds[0].owner = -1;
    st.pending_ship_build_count = 1;
    st.pending_ship_builds[0].hull_class = HULL_CLASS_HAULER;
    st.pending_ship_builds[0].owner = 2;
    st.pending_ship_builds[0].build_progress = 0.75f;
    snprintf(st.hail_message, sizeof(st.hail_message), "hello");
    snprintf(st.miner_chatter[0], sizeof(st.miner_chatter[0]), "mine");
    snprintf(st.hauler_chatter[0], sizeof(st.hauler_chatter[0]), "haul");
    snprintf(st.rati_hail_message, sizeof(st.rati_hail_message), "rati");
    snprintf(st.currency_name, sizeof(st.currency_name), "scrip");
    st.station_pubkey[0] = 0xAB;
    st.stored_hull_count[HULL_CLASS_HAULER] = 4;
    station_faction_seed_station(&st, 1);
    st.policy_card_count = 1;
    st.policy_card_ids[0] = (uint8_t)STATION_POLICY_CARD_BLACK_MARKET;

    uint8_t full[STATION_IDENTITY_SIZE] = {0};
    int full_len = serialize_station_identity(full, 7, &st);
    ASSERT_EQ_INT(full_len, STATION_IDENTITY_SIZE);
    uint8_t compact[STATION_IDENTITY_Q_MAX_SIZE] = {0};
    int len = serialize_station_identity_q_from_full(compact, full, full_len);
    ASSERT(len > 0);
    ASSERT(len < STATION_IDENTITY_SIZE / 2);
    ASSERT_EQ_INT(compact[0], NET_MSG_STATION_IDENTITY_Q);
    ASSERT_EQ_INT(compact[1], 7);
    ASSERT_EQ_INT(compact[2], 0);
    ASSERT_EQ_INT((int)read_u32_le(&compact[3]), 0x12345678);
    ASSERT_EQ_FLOAT(read_f32_le(&compact[7]), 10.0f, 0.001f);
    ASSERT_EQ_FLOAT(read_f32_le(&compact[11]), -20.0f, 0.001f);

    int off = STATION_IDENTITY_Q_HEADER_SIZE + 24;
    ASSERT_EQ_INT(compact[off], (int)strlen("Compact Station"));
    ASSERT(memcmp(&compact[off + 1], "Compact Station",
                  strlen("Compact Station")) == 0);
    off += 1 + (int)strlen("Compact Station");
    ASSERT_EQ_FLOAT(read_f32_le(&compact[off + COMMODITY_FERRITE_ORE * 4]),
                    11.0f, 0.001f);
    off += COMMODITY_COUNT * 4 + 4;
    ASSERT_EQ_INT(compact[off++], 2);
    ASSERT_EQ_INT(compact[off], MODULE_DOCK);
    off += STATION_MODULE_RECORD_SIZE;
    ASSERT_EQ_INT(compact[off], MODULE_FURNACE);
    ASSERT_EQ_INT(compact[off + 8], COMMODITY_CUPRITE_ORE);
    off += STATION_MODULE_RECORD_SIZE;
    ASSERT_EQ_INT(compact[off++], 2);
    ASSERT_EQ_FLOAT(read_f32_le(&compact[off]), 0.10f, 0.001f);
    ASSERT_EQ_FLOAT(read_f32_le(&compact[off + 12]), 0.40f, 0.001f);
    off += 2 * 16;
    ASSERT_EQ_INT(compact[off++], 1);
    ASSERT_EQ_INT(compact[off], MODULE_SIGNAL_RELAY);
    off += STATION_PLAN_RECORD_SIZE;
    ASSERT_EQ_INT(compact[off++], 1);
    ASSERT_EQ_INT(compact[off], MODULE_DOCK);
    ASSERT_EQ_INT(compact[off + 1], 0xFF);
    off += STATION_PENDING_SCAFFOLD_RECORD_SIZE;
    ASSERT_EQ_INT(compact[off++], 1);
    ASSERT_EQ_INT(compact[off], HULL_CLASS_HAULER);
    ASSERT_EQ_FLOAT(read_f32_le(&compact[off + 2]), 0.75f, 0.001f);
    off += STATION_PENDING_SHIP_RECORD_SIZE;
    ASSERT_EQ_INT(compact[off], (int)strlen("hello"));
    off += 1 + (int)strlen("hello");
    ASSERT_EQ_INT(compact[off], (int)strlen("mine"));
    off += 1 + (int)strlen("mine");
    for (int i = 1; i < STATION_IDENTITY_CHATTER_LINES; i++)
        ASSERT_EQ_INT(compact[off++], 0);
    ASSERT_EQ_INT(compact[off], (int)strlen("haul"));
    off += 1 + (int)strlen("haul");
    for (int i = 1; i < STATION_IDENTITY_CHATTER_LINES; i++)
        ASSERT_EQ_INT(compact[off++], 0);
    ASSERT_EQ_INT(compact[off], (int)strlen("rati"));
    off += 1 + (int)strlen("rati");
    ASSERT_EQ_INT(compact[off], (int)strlen("scrip"));
    off += 1 + (int)strlen("scrip");
    ASSERT_EQ_INT(compact[off], 0xAB);
    off += STATION_IDENTITY_PUBKEY_LEN;
    ASSERT_EQ_INT(compact[off + HULL_CLASS_HAULER], 4);
    off += HULL_CLASS_COUNT + STATION_IDENTITY_FACTION_SIZE;
    ASSERT_EQ_INT(compact[off++], 1);
    ASSERT_EQ_INT(compact[off++], STATION_POLICY_CARD_BLACK_MARKET);
    ASSERT_EQ_INT(off, len);
}

TEST(test_station_identity_serializes_pending_ship_builds) {
    station_t st;
    memset(&st, 0, sizeof(st));
    st.pending_ship_build_count = 2;
    st.pending_ship_builds[0].hull_class = HULL_CLASS_HAULER;
    st.pending_ship_builds[0].owner = 3;
    st.pending_ship_builds[0].build_progress = 0.25f;
    st.pending_ship_builds[1].hull_class = HULL_CLASS_DRONE_TRACTOR;
    st.pending_ship_builds[1].owner = -1;
    st.pending_ship_builds[1].build_progress = 0.0f;
    st.stored_hull_count[HULL_CLASS_MINER] = 3;
    st.stored_hull_count[HULL_CLASS_DRONE_TRACTOR] = 2;

    uint8_t buf[STATION_IDENTITY_SIZE] = {0};
    int len = serialize_station_identity(buf, 2, &st);
    ASSERT_EQ_INT(len, STATION_IDENTITY_SIZE);

    int moff = 59 + COMMODITY_COUNT * 4 + 4
        + 1 + MAX_MODULES_PER_STATION * STATION_MODULE_RECORD_SIZE
        + 1 + MAX_ARMS * 4 + MAX_ARMS * 4 + MAX_ARMS * 4 + MAX_ARMS * 4
        + 1 + STATION_PLAN_RECORD_COUNT * STATION_PLAN_RECORD_SIZE
        + 1 + STATION_PENDING_SCAFFOLD_RECORD_COUNT * STATION_PENDING_SCAFFOLD_RECORD_SIZE;

    ASSERT_EQ_INT(buf[moff], 2);
    moff++;
    ASSERT_EQ_INT(buf[moff + 0], HULL_CLASS_HAULER);
    ASSERT_EQ_INT(buf[moff + 1], 3);
    ASSERT_EQ_FLOAT(read_f32_le(&buf[moff + 2]), 0.25f, 0.001f);
    moff += STATION_PENDING_SHIP_RECORD_SIZE;
    ASSERT_EQ_INT(buf[moff + 0], HULL_CLASS_DRONE_TRACTOR);
    ASSERT_EQ_INT(buf[moff + 1], 0xFF);
    ASSERT_EQ_FLOAT(read_f32_le(&buf[moff + 2]), 0.0f, 0.001f);
    moff = STATION_IDENTITY_V1_SIZE;
    ASSERT_EQ_INT(buf[moff + HULL_CLASS_MINER], 3);
    ASSERT_EQ_INT(buf[moff + HULL_CLASS_DRONE_TRACTOR], 2);
}

TEST(test_station_identity_serializes_faction_trailer) {
    station_t st;
    memset(&st, 0, sizeof(st));
    station_faction_seed_station(&st, 2);
    st.faction_relations[STATION_FACTION_BLACKGLASS_SYNDICATE] = -91;
    st.policy_card_count = 2;
    st.policy_card_ids[0] = (uint8_t)STATION_POLICY_CARD_PROVENANCE_SCREENING;
    st.policy_card_ids[1] = (uint8_t)STATION_POLICY_CARD_BLACK_MARKET;

    uint8_t buf[STATION_IDENTITY_SIZE] = {0};
    int len = serialize_station_identity(buf, 2, &st);

    ASSERT_EQ_INT(len, STATION_IDENTITY_SIZE);
    int moff = STATION_IDENTITY_HULL_SIZE;
    ASSERT_EQ_INT(buf[moff++], STATION_FACTION_HELIOS_CONSORTIUM);
    ASSERT_EQ_INT(buf[moff++], STATION_FACTION_HELIOS_CONSORTIUM);
    ASSERT_EQ_INT(buf[moff++], STATION_IDEOLOGY_EXPANSIONIST);
    ASSERT_EQ_INT((int)(int8_t)buf[moff + STATION_FACTION_BLACKGLASS_SYNDICATE],
                  -91);
    moff = STATION_IDENTITY_FACTION_TRAILER_SIZE;
    ASSERT_EQ_INT(buf[moff++], 2);
    ASSERT_EQ_INT(buf[moff++], STATION_POLICY_CARD_PROVENANCE_SCREENING);
    ASSERT_EQ_INT(buf[moff++], STATION_POLICY_CARD_BLACK_MARKET);
}

TEST(test_station_identity_semantic_hash_ignores_ring_drift) {
    station_t st;
    memset(&st, 0, sizeof(st));
    st.signal_range = 1000.0f;
    st.dock_radius = 120.0f;
    snprintf(st.name, sizeof(st.name), "Semantic Identity");
    st.arm_count = 3;
    for (int a = 0; a < MAX_ARMS; a++) {
        st.arm_speed[a] = 0.05f * (float)(a + 1);
        st.ring_offset[a] = 0.25f * (float)a;
        st.arm_rotation[a] = 0.5f * (float)a;
        st.arm_omega[a] = 0.01f * (float)(a + 1);
    }

    uint8_t base[STATION_IDENTITY_SIZE] = {0};
    int base_len = serialize_station_identity(base, 2, &st);
    ASSERT_EQ_INT(base_len, STATION_IDENTITY_SIZE);
    uint64_t base_hash = net_station_identity_semantic_hash(base, base_len);

    for (int a = 0; a < MAX_ARMS; a++) {
        st.arm_rotation[a] += 3.0f + (float)a;
        st.arm_omega[a] -= 0.75f + 0.1f * (float)a;
    }
    uint8_t drift[STATION_IDENTITY_SIZE] = {0};
    int drift_len = serialize_station_identity(drift, 2, &st);
    ASSERT_EQ_INT(drift_len, STATION_IDENTITY_SIZE);
    uint64_t drift_hash = net_station_identity_semantic_hash(drift, drift_len);
    ASSERT(base_hash == drift_hash);

    st.arm_speed[1] += 0.2f;
    uint8_t changed[STATION_IDENTITY_SIZE] = {0};
    int changed_len = serialize_station_identity(changed, 2, &st);
    ASSERT_EQ_INT(changed_len, STATION_IDENTITY_SIZE);
    uint64_t changed_hash =
        net_station_identity_semantic_hash(changed, changed_len);
    ASSERT(base_hash != changed_hash);
}

TEST(test_payload_cache_suppresses_unchanged_station_identity_per_connection) {
    station_t st;
    memset(&st, 0, sizeof(st));
    st.signal_range = 1000.0f;
    st.dock_radius = 120.0f;
    snprintf(st.name, sizeof(st.name), "Cache Test");

    uint8_t buf[STATION_IDENTITY_SIZE] = {0};
    int len = serialize_station_identity(buf, 2, &st);
    ASSERT_EQ_INT(len, STATION_IDENTITY_SIZE);

    net_payload_cache_t cache;
    memset(&cache, 0, sizeof(cache));
    int conn_a = 1;
    int conn_b = 2;

    ASSERT(net_payload_cache_should_send(&cache, &conn_a, buf, (size_t)len));
    ASSERT(!net_payload_cache_should_send(&cache, &conn_a, buf, (size_t)len));

    ASSERT(net_payload_cache_should_send(&cache, &conn_b, buf, (size_t)len));
    ASSERT(!net_payload_cache_should_send(&cache, &conn_b, buf, (size_t)len));

    st.signal_range = 1250.0f;
    len = serialize_station_identity(buf, 2, &st);
    ASSERT(net_payload_cache_should_send(&cache, &conn_b, buf, (size_t)len));
    ASSERT(!net_payload_cache_should_send(&cache, &conn_b, buf, (size_t)len));
}

TEST(test_deferable_snapshot_classification_preserves_ack_lane) {
    ASSERT(net_msg_is_deferable_snapshot(NET_MSG_WORLD_PLAYERS));
    ASSERT(net_msg_is_deferable_snapshot(NET_MSG_WORLD_PLAYER_MOTION));
    ASSERT(net_msg_is_deferable_snapshot(NET_MSG_WORLD_PLAYER_MOTION_Q));
    ASSERT(net_msg_is_deferable_snapshot(NET_MSG_WORLD_PLAYER_MOTIOND_Q));
    ASSERT(net_msg_is_deferable_snapshot(NET_MSG_WORLD_PLAYER_POSED_Q));
    ASSERT(net_msg_is_deferable_snapshot(NET_MSG_WORLD_PLAYER_MOTIONM_Q));
    ASSERT(net_msg_is_deferable_snapshot(NET_MSG_WORLD_PLAYER_DOCK_Q));
    ASSERT(net_msg_is_deferable_snapshot(NET_MSG_WORLD_NPCS));
    ASSERT(net_msg_is_deferable_snapshot(NET_MSG_WORLD_NPC_MOTION));
    ASSERT(net_msg_is_deferable_snapshot(NET_MSG_WORLD_NPC_MOTION_Q));
    ASSERT(net_msg_is_deferable_snapshot(NET_MSG_WORLD_NPC_MOTION8_Q));
    ASSERT(net_msg_is_deferable_snapshot(NET_MSG_WORLD_NPC_POS_Q));
    ASSERT(net_msg_is_deferable_snapshot(NET_MSG_WORLD_NPC_POSE_Q));
    ASSERT(net_msg_is_deferable_snapshot(NET_MSG_WORLD_NPC_LINEAR_Q));
    ASSERT(net_msg_is_deferable_snapshot(NET_MSG_WORLD_NPC_STATUS));
    ASSERT(net_msg_is_deferable_snapshot(NET_MSG_WORLD_NPC_STATUS8_Q));
    ASSERT(net_msg_is_deferable_snapshot(NET_MSG_WORLD_TIME));
    ASSERT(net_msg_is_deferable_snapshot(NET_MSG_WORLD_SCAFFOLDS));
    ASSERT(net_msg_is_deferable_snapshot(NET_MSG_WORLD_SCAFFOLD_MOTION_Q));
    ASSERT(net_msg_is_deferable_snapshot(NET_MSG_WORLD_CARGO_PODS));
    ASSERT(net_msg_is_deferable_snapshot(NET_MSG_WORLD_CARGO_PODS_Q));
    ASSERT(net_msg_is_deferable_snapshot(NET_MSG_WORLD_CARGO_POD_MOTION));
    ASSERT(net_msg_is_deferable_snapshot(NET_MSG_WORLD_CARGO_POD_MOTION_Q));
    ASSERT(net_msg_is_deferable_snapshot(NET_MSG_WORLD_CARGO_POD_LINEAR_Q));
    ASSERT(net_msg_is_deferable_snapshot(NET_MSG_WORLD_INTERACTIONS));
    ASSERT(net_msg_is_deferable_snapshot(NET_MSG_WORLD_INTERACTIONS_Q));
    ASSERT(net_msg_is_deferable_snapshot(NET_MSG_WORLD_INTERACTION_DRIFT));
    ASSERT(net_msg_is_deferable_snapshot(NET_MSG_WORLD_ASTEROID_MOTION));
    ASSERT(net_msg_is_deferable_snapshot(NET_MSG_WORLD_ASTEROID_MOTION_Q));
    ASSERT(net_msg_is_deferable_snapshot(NET_MSG_WORLD_ASTEROID_POS_Q));
    ASSERT(net_msg_is_deferable_snapshot(NET_MSG_WORLD_ASTEROID_POS8_Q));
    ASSERT(net_msg_is_deferable_snapshot(NET_MSG_WORLD_ASTEROID_POSD_Q));
    ASSERT(net_msg_is_deferable_snapshot(NET_MSG_WORLD_ASTEROID_POSD8_Q));
    ASSERT(net_msg_is_deferable_snapshot(NET_MSG_WORLD_ASTEROID_STATE_Q));
    ASSERT(net_msg_is_deferable_snapshot(NET_MSG_WORLD_ASTEROIDS));
    ASSERT(net_msg_is_deferable_snapshot(NET_MSG_WORLD_ASTEROIDS_Q));
    ASSERT(net_msg_is_deferable_snapshot(NET_MSG_WORLD_ASTEROIDS8_Q));

    ASSERT(!net_msg_is_deferable_snapshot(NET_MSG_LATENCY_PONG));
    ASSERT(!net_msg_is_deferable_snapshot(NET_MSG_INPUT_APPLIED));
    ASSERT(!net_msg_is_deferable_snapshot(NET_MSG_STATE));
    ASSERT(!net_msg_is_deferable_snapshot(NET_MSG_ACTION_ACK));
    ASSERT(!net_msg_is_deferable_snapshot(NET_MSG_ACTION_RESULT));
    ASSERT(!net_msg_is_deferable_snapshot(NET_MSG_WORLD_ASTEROID_REMOVE));
    ASSERT(!net_msg_is_deferable_snapshot(NET_MSG_WORLD_CARGO_POD_REMOVE));
    ASSERT(!net_msg_is_deferable_snapshot(NET_MSG_WORLD_SCAFFOLD_REMOVE));
}

TEST(test_deferable_snapshot_backpressure_reserves_control_lane) {
    const size_t reserve = 8192u;

    ASSERT(!net_deferable_snapshot_would_backpressure(
        NET_MSG_LATENCY_PONG, reserve, 1u, reserve));
    ASSERT(!net_deferable_snapshot_would_backpressure(
        NET_MSG_WORLD_NPCS, 0u, reserve, reserve));
    ASSERT(net_deferable_snapshot_would_backpressure(
        NET_MSG_WORLD_NPCS, 0u, reserve + 1u, reserve));
    ASSERT(net_deferable_snapshot_would_backpressure(
        NET_MSG_WORLD_NPCS, reserve - 2u, 3u, reserve));
    ASSERT(!net_deferable_snapshot_would_backpressure(
        NET_MSG_WORLD_NPCS, reserve - 2u, 2u, reserve));
    ASSERT(net_deferable_snapshot_would_backpressure(
        NET_MSG_WORLD_PLAYER_MOTION_Q, reserve - 8u, 12u, reserve));
    ASSERT(net_deferable_snapshot_would_backpressure(
        NET_MSG_WORLD_PLAYER_MOTIOND_Q, reserve - 8u, 12u, reserve));
    ASSERT(net_deferable_snapshot_would_backpressure(
        NET_MSG_WORLD_PLAYER_POSED_Q, reserve - 8u, 12u, reserve));
    ASSERT(net_deferable_snapshot_would_backpressure(
        NET_MSG_WORLD_PLAYER_MOTIONM_Q, reserve - 8u, 12u, reserve));
    ASSERT(net_deferable_snapshot_would_backpressure(
        NET_MSG_WORLD_PLAYER_DOCK_Q, reserve - 8u, 12u, reserve));
    ASSERT(net_deferable_snapshot_would_backpressure(
        NET_MSG_WORLD_ASTEROID_MOTION_Q, reserve - 8u, 12u, reserve));
    ASSERT(net_deferable_snapshot_would_backpressure(
        NET_MSG_WORLD_ASTEROID_POS_Q, reserve - 4u, 8u, reserve));
    ASSERT(net_deferable_snapshot_would_backpressure(
        NET_MSG_WORLD_ASTEROID_POS8_Q, reserve - 4u, 8u, reserve));
    ASSERT(net_deferable_snapshot_would_backpressure(
        NET_MSG_WORLD_ASTEROID_POSD_Q, reserve - 4u, 8u, reserve));
    ASSERT(net_deferable_snapshot_would_backpressure(
        NET_MSG_WORLD_ASTEROID_POSD8_Q, reserve - 4u, 8u, reserve));
    ASSERT(net_deferable_snapshot_would_backpressure(
        NET_MSG_WORLD_ASTEROIDS, reserve - 8u, 12u, reserve));
    ASSERT(net_deferable_snapshot_would_backpressure(
        NET_MSG_WORLD_ASTEROIDS_Q, reserve - 8u, 12u, reserve));
    ASSERT(net_deferable_snapshot_would_backpressure(
        NET_MSG_WORLD_ASTEROIDS8_Q, reserve - 8u, 12u, reserve));
    ASSERT(!net_deferable_snapshot_would_backpressure(
        NET_MSG_WORLD_ASTEROID_REMOVE, reserve - 8u, 12u, reserve));
}

TEST(test_bug92_station_record_size_matches_buffer) {
    /* Bug 92: station broadcast buffer must match serialized record size.
     * STATION_RECORD_SIZE is validated at compile time via _Static_assert,
     * but verify at runtime that serialize_stations writes exactly the
     * expected number of bytes. */
    station_t stations[MAX_STATIONS];
    memset(stations, 0, sizeof(stations));
    /* Empty stations should produce 0 records */
    uint8_t buf[2 + MAX_STATIONS * STATION_RECORD_SIZE];
    int len = serialize_stations(buf, stations);
    ASSERT_EQ_INT(len, 2); /* header only, no records */
    /* With active stations */
    for (int i = 0; i < 3; i++) stations[i].signal_range = 1000.0f;
    len = serialize_stations(buf, stations);
    ASSERT_EQ_INT(len, 2 + 3 * STATION_RECORD_SIZE);
    ASSERT((size_t)len <= sizeof(buf));
}

TEST(test_player_known_contract_mask_uses_compact_contract_ordinals) {
    contract_t contracts[MAX_CONTRACTS];
    memset(contracts, 0, sizeof(contracts));

    contracts[3] = (contract_t){
        .active = true,
        .action = CONTRACT_TRACTOR,
        .station_index = 1,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity_needed = 5.0f,
        .base_price = 10.0f,
        .target_index = -1,
    };
    contracts[7] = (contract_t){
        .active = true,
        .action = CONTRACT_TRACTOR,
        .station_index = 2,
        .commodity = COMMODITY_CUPRITE_INGOT,
        .proof_flags = (uint8_t)(CONTRACT_PROOF_REQUIRE_PROOF |
                                 CONTRACT_PROOF_REQUIRE_RECIPE |
                                 CONTRACT_PROOF_FORBID_ORIGIN),
        .required_recipe_id = (uint16_t)RECIPE_SMELT,
        .forbidden_origin_mask = 1ULL << 0,
        .quantity_needed = 8.0f,
        .base_price = 20.0f,
        .target_index = -1,
    };
    for (int i = 0; i < 32; i++)
        contracts[7].target_pub[i] = (uint8_t)(0x40u + (uint8_t)i);

    uint8_t cbuf[2 + MAX_CONTRACTS * CONTRACT_RECORD_SIZE];
    int clen = serialize_contracts(cbuf, contracts);
    ASSERT_EQ_INT(clen, 2 + 2 * CONTRACT_RECORD_SIZE);
    ASSERT_EQ_INT(cbuf[1], 2);
    ASSERT_EQ_INT(cbuf[2 + CONTRACT_RECORD_SIZE + 1], 2);
    ASSERT_EQ_INT(cbuf[2 + CONTRACT_RECORD_SIZE + 2], COMMODITY_CUPRITE_INGOT);
    ASSERT_EQ_INT(cbuf[2 + CONTRACT_RECORD_SIZE + 4],
                  CONTRACT_PROOF_REQUIRE_PROOF |
                  CONTRACT_PROOF_REQUIRE_RECIPE |
                  CONTRACT_PROOF_FORBID_ORIGIN);
    ASSERT_EQ_INT(read_u16_le(&cbuf[2 + CONTRACT_RECORD_SIZE + 6]), RECIPE_SMELT);
    ASSERT_EQ_INT((int)read_u64_le(&cbuf[2 + CONTRACT_RECORD_SIZE + 64]), 1);
    ASSERT(memcmp(&cbuf[2 + CONTRACT_RECORD_SIZE + 72],
                  contracts[7].target_pub, 32) == 0);

    ship_t ship;
    memset(&ship, 0, sizeof(ship));
    test_clear_knowledge(&ship.knowledge, SHIP_KNOWN_ITEM_CAP);
    contract_summary_t known = {
        .active = true,
        .action = (uint8_t)CONTRACT_TRACTOR,
        .station_index = 2,
        .commodity = (uint8_t)COMMODITY_CUPRITE_INGOT,
        .proof_flags = (uint8_t)(CONTRACT_PROOF_REQUIRE_PROOF |
                                 CONTRACT_PROOF_REQUIRE_RECIPE |
                                 CONTRACT_PROOF_FORBID_ORIGIN),
        .required_recipe_id = (uint16_t)RECIPE_SMELT,
        .forbidden_origin_mask = 1ULL << 0,
        .quantity_needed = 8.0f,
        .base_price = 20.0f,
    };
    memcpy(known.target_pub, contracts[7].target_pub, 32);
    ASSERT(test_add_known_contract(&ship.knowledge, &known));

    uint8_t kbuf[5];
    int klen = serialize_player_known_contracts(kbuf, contracts, &ship);
    ASSERT_EQ_INT(klen, 5);
    ASSERT_EQ_INT(kbuf[0], NET_MSG_PLAYER_KNOWN_CONTRACTS);
    uint32_t mask = read_u32_le(&kbuf[1]);
    ASSERT_EQ_INT((int)mask, 1 << 1);
    ASSERT_EQ_INT(contract_compact_index_for_slot(contracts, 3), 0);
    ASSERT_EQ_INT(contract_compact_index_for_slot(contracts, 7), 1);
}

TEST(test_player_known_ledger_serializes_station_balances) {
    WORLD_DECL;
    world_reset(&w);
    server_player_t *sp = &w.players[0];
    player_init_ship(sp, &w);
    sp->connected = true;
    sp->session_ready = true;
    memcpy(sp->session_token, "LEDGER01", 8);

    ledger_earn(&w.stations[0], sp->session_token, 123.0f);
    ledger_earn(&w.stations[2], sp->session_token, 45.0f);

    uint8_t buf[PLAYER_KNOWN_LEDGER_HEADER +
                PLAYER_KNOWN_LEDGER_MAX_RECORDS *
                PLAYER_KNOWN_LEDGER_RECORD_SIZE];
    int len = serialize_player_known_ledger(buf, &w, sp);
    ASSERT_EQ_INT(buf[0], NET_MSG_PLAYER_KNOWN_LEDGER);
    ASSERT_EQ_INT(buf[1], 2);
    ASSERT_EQ_INT(len, PLAYER_KNOWN_LEDGER_HEADER +
                       2 * PLAYER_KNOWN_LEDGER_RECORD_SIZE);
    ASSERT_EQ_INT(buf[2], 0);
    ASSERT_EQ_FLOAT(read_f32_le(&buf[3]), 123.0f, 0.001f);
    ASSERT_EQ_INT(buf[7], 2);
    ASSERT_EQ_FLOAT(read_f32_le(&buf[8]), 45.0f, 0.001f);

    memset(sp->pubkey, 0x21, sizeof(sp->pubkey));
    sp->pubkey_set = true;
    sp->pubkey_proof_ok = true;
    ledger_earn_by_pubkey(&w.stations[1], sp->pubkey, 77.0f);
    len = serialize_player_known_ledger(buf, &w, sp);
    ASSERT_EQ_INT(len, PLAYER_KNOWN_LEDGER_HEADER +
                       PLAYER_KNOWN_LEDGER_RECORD_SIZE);
    ASSERT_EQ_INT(buf[1], 1);
    ASSERT_EQ_INT(buf[2], 1);
    ASSERT_EQ_FLOAT(read_f32_le(&buf[3]), 77.0f, 0.001f);
}

TEST(test_delivery_contract_action_serializes) {
    contract_t contracts[MAX_CONTRACTS];
    memset(contracts, 0, sizeof(contracts));
    contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_DELIVERY,
        .station_index = 2,
        .target_index = 0,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity_needed = 3.0f,
        .base_price = 42.0f,
    };

    uint8_t buf[2 + MAX_CONTRACTS * CONTRACT_RECORD_SIZE];
    int len = serialize_contracts(buf, contracts);
    ASSERT_EQ_INT(len, 2 + CONTRACT_RECORD_SIZE);
    ASSERT_EQ_INT(buf[0], NET_MSG_CONTRACTS);
    ASSERT_EQ_INT(buf[1], 1);
    ASSERT_EQ_INT(buf[2], CONTRACT_DELIVERY);
    ASSERT_EQ_INT(buf[3], 2);
    ASSERT_EQ_INT((int)read_u32_le(&buf[2 + 28]), 0);
    ASSERT_EQ_INT(CONTRACT_RECORD_SIZE, 104);
}

TEST(test_contracts_q_omits_zero_optional_tails) {
    contract_t contracts[MAX_CONTRACTS];
    memset(contracts, 0, sizeof(contracts));
    contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_TRACTOR,
        .station_index = 1,
        .target_index = -1,
        .commodity = COMMODITY_FERRITE_ORE,
        .quantity_needed = 3.0f,
        .base_price = 42.0f,
    };
    contracts[1] = (contract_t){
        .active = true,
        .action = CONTRACT_DELIVERY,
        .station_index = 2,
        .target_index = 7,
        .commodity = COMMODITY_CUPRITE_INGOT,
        .proof_flags = CONTRACT_PROOF_FORBID_ORIGIN,
        .quantity_needed = 5.0f,
        .base_price = 80.0f,
        .forbidden_origin_mask = 0x0102030405060708ull,
    };
    contracts[1].required_parent[0] = 0xa5u;
    contracts[1].target_pub[0] = 0xb6u;

    uint8_t full[2 + MAX_CONTRACTS * CONTRACT_RECORD_SIZE];
    int full_len = serialize_contracts(full, contracts);
    uint8_t q[CONTRACT_Q_MAX_SIZE] = {0};
    int q_len = serialize_contracts_q_from_full(q, full, full_len);

    ASSERT_EQ_INT(q[0], NET_MSG_CONTRACTS_Q);
    ASSERT_EQ_INT(q[1], 2);
    ASSERT_EQ_INT(q_len, 2 + (1 + CONTRACT_Q_BASE_SIZE) +
                         CONTRACT_Q_MAX_RECORD_SIZE);
    ASSERT(q_len < full_len);

    int off = CONTRACT_Q_HEADER_SIZE;
    ASSERT_EQ_INT(q[off], 0);
    ASSERT_EQ_INT(q[off + 1], CONTRACT_TRACTOR);
    ASSERT_EQ_INT(q[off + 2], 1);
    ASSERT_EQ_INT((int)read_u32_le(&q[off + 1 + 28]), -1);
    off += 1 + CONTRACT_Q_BASE_SIZE;

    ASSERT_EQ_INT(q[off], CONTRACT_Q_FLAG_PARENT |
                          CONTRACT_Q_FLAG_ORIGIN_MASK |
                          CONTRACT_Q_FLAG_TARGET_PUB);
    ASSERT_EQ_INT(q[off + 1], CONTRACT_DELIVERY);
    ASSERT_EQ_INT(q[off + 2], 2);
    ASSERT_EQ_INT((int)read_u32_le(&q[off + 1 + 28]), 7);
    off += 1 + CONTRACT_Q_BASE_SIZE;
    ASSERT_EQ_INT(q[off], 0xa5);
    off += 32;
    ASSERT(read_u64_le(&q[off]) == 0x0102030405060708ull);
    off += 8;
    ASSERT_EQ_INT(q[off], 0xb6);
    off += 32;
    ASSERT_EQ_INT(off, q_len);
}

TEST(test_contracts_semantic_hash_ignores_age_only) {
    contract_t contracts[MAX_CONTRACTS];
    memset(contracts, 0, sizeof(contracts));
    contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_TRACTOR,
        .station_index = 2,
        .target_index = -1,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity_needed = 3.0f,
        .base_price = 42.0f,
        .age = 1.0f,
    };

    uint8_t a[2 + MAX_CONTRACTS * CONTRACT_RECORD_SIZE];
    uint8_t b[2 + MAX_CONTRACTS * CONTRACT_RECORD_SIZE];
    int alen = serialize_contracts(a, contracts);
    uint64_t ahash = net_contracts_semantic_hash(a, alen);

    contracts[0].age = 6.0f;
    int blen = serialize_contracts(b, contracts);
    uint64_t bhash = net_contracts_semantic_hash(b, blen);
    ASSERT_EQ_INT(alen, blen);
    ASSERT(ahash == bhash);
    ASSERT_EQ_INT((int)CONTRACTS_AGE_REFRESH_MS, 30000);
    ASSERT(contracts_age_refresh_due(0ull, 10ull));
    ASSERT(!contracts_age_refresh_due(
        1000ull, 1000ull + CONTRACTS_AGE_REFRESH_MS - 1ull));
    ASSERT(contracts_age_refresh_due(
        1000ull, 1000ull + CONTRACTS_AGE_REFRESH_MS));

    contracts[0].quantity_needed = 4.0f;
    blen = serialize_contracts(b, contracts);
    bhash = net_contracts_semantic_hash(b, blen);
    ASSERT(ahash != bhash);

    contracts[0].quantity_needed = 3.0f;
    contracts[0].base_price = 45.0f;
    blen = serialize_contracts(b, contracts);
    bhash = net_contracts_semantic_hash(b, blen);
    ASSERT(ahash != bhash);
}

TEST(test_delivery_ledger_serializes_player_shipments) {
    WORLD_DECL;
    world_reset(&w);
    uint8_t bound_pub[32];
    uint8_t other_bound_pub[32];
    memset(bound_pub, 0xa1, sizeof(bound_pub));
    memset(other_bound_pub, 0xb2, sizeof(other_bound_pub));
    w.delivery_shipments[0] = (delivery_shipment_t){
        .active = true,
        .shipment_id = 77,
        .origin_station = 0,
        .destination_station = 2,
        .contract_index = 4,
        .debtor_player = 1,
        .commodity = (uint8_t)COMMODITY_FERRITE_INGOT,
        .quantity_total = 3,
        .quantity_bound = 3,
        .quantity_delivered = 1,
        .debt_principal = 60.0f,
        .destination_payout = 150.0f,
        .origin_completion_credit = 6.0f,
        .due_tick = 900,
        .status = DELIVERY_SHIPMENT_PICKED_UP,
    };
    memcpy(w.delivery_shipments[0].cargo_pub[0], bound_pub, 32);
    memcpy(w.delivery_shipments[0].cargo_pub[1], other_bound_pub, 32);
    w.delivery_shipments[1] = (delivery_shipment_t){
        .active = true,
        .shipment_id = 88,
        .debtor_player = 1,
        .status = DELIVERY_SHIPMENT_CLEARED,
    };
    w.delivery_shipments[2] = (delivery_shipment_t){
        .active = true,
        .shipment_id = 99,
        .debtor_player = 0,
        .status = DELIVERY_SHIPMENT_PICKED_UP,
    };

    w.players[1].ship->towed_pods[0] = 5;
    w.players[1].ship->towed_pod_count = 1;
    w.cargo_pods[5] = (cargo_pod_t){
        .active = true,
        .kind = CARGO_POD_CARGO,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity = 2,
        .shipment_id = 77,
    };
    cargo_pod_set_player_tractor(&w.cargo_pods[5], 1);

    uint8_t buf[DELIVERY_LEDGER_HEADER +
                DELIVERY_LEDGER_MAX_RECORDS * DELIVERY_LEDGER_RECORD_SIZE];
    int len = serialize_delivery_ledger(buf, &w, 1);
    ASSERT_EQ_INT(len, DELIVERY_LEDGER_HEADER + DELIVERY_LEDGER_RECORD_SIZE);
    ASSERT_EQ_INT(buf[0], NET_MSG_DELIVERY_LEDGER);
    ASSERT_EQ_INT(buf[1], 1);
    const uint8_t *p = &buf[DELIVERY_LEDGER_HEADER];
    ASSERT_EQ_INT(read_u16_le(&p[0]), 77);
    ASSERT_EQ_INT(p[2], DELIVERY_SHIPMENT_PICKED_UP);
    ASSERT_EQ_INT(p[3], 0);
    ASSERT_EQ_INT(p[4], 2);
    ASSERT_EQ_INT(p[5], 4);
    ASSERT_EQ_INT(p[6], COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(read_u16_le(&p[7]), 3);
    ASSERT_EQ_INT(read_u16_le(&p[9]), 1);
    ASSERT_EQ_INT(read_u16_le(&p[11]), 3);
    ASSERT_EQ_FLOAT(read_f32_le(&p[13]), 60.0f, 0.001f);
    ASSERT_EQ_FLOAT(read_f32_le(&p[17]), 150.0f, 0.001f);
    ASSERT_EQ_FLOAT(read_f32_le(&p[21]), 6.0f, 0.001f);
    ASSERT_EQ_INT((int)read_u32_le(&p[25]), 900);
    ASSERT_EQ_INT(read_u16_le(&p[29]), 2);
}

TEST(test_bug93_hint_mines_small_shard_with_minor_desync) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.asteroids, 0, sizeof(w.asteroids));
    memset(w.npc_ships, 0, sizeof(w.npc_ships));

    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    w.players[0].id = 0;
    w.players[0].docked = false;
    w.players[0].in_dock_range = false;
    w.players[0].nearby_station = -1;
    w.players[0].ship->pos = v2(0.0f, 0.0f);
    w.players[0].ship->vel = v2(0.0f, 0.0f);
    w.players[0].ship->angle = 0.0f;
    w.players[0].ship->mining_level = 0;
    w.players[0].input.mine = true;
    w.players[0].input.mining_target_hint = 0;

    /* Place an M-tier shard just outside the exact server ray, as would
     * happen when the client view is a few units behind a fast fracture child.
     * Exact fallback targeting should miss it; the explicit hint should still
     * be accepted and mine it. */
    w.asteroids[0].active = true;
    w.asteroids[0].fracture_child = true;
    w.asteroids[0].tier = ASTEROID_TIER_M;
    w.asteroids[0].commodity = COMMODITY_FERRITE_ORE;
    w.asteroids[0].pos = v2(80.0f, 26.0f);
    w.asteroids[0].vel = v2(0.0f, 0.0f);
    w.asteroids[0].radius = 20.0f;
    w.asteroids[0].hp = 40.0f;
    w.asteroids[0].max_hp = 40.0f;

    float hp_before = w.asteroids[0].hp;
    world_sim_step(&w, SIM_DT);

    ASSERT_EQ_INT(w.players[0].hover_asteroid, 0);
    ASSERT(w.asteroids[0].hp < hp_before);
}

TEST(test_roundtrip_player_ship) {
    SERVER_PLAYER_DECL(sp);
    sp.ship->hull = 85.5f;
    sp.docked = true;
    sp.current_station = 2;
    sp.ship->mining_level = 3;
    sp.ship->hold_level = 2;
    sp.ship->tractor_level = 1;
    sp.ship->cargo[COMMODITY_FERRITE_ORE] = 45.0f;
    sp.ship->cargo[COMMODITY_CUPRITE_ORE] = 12.5f;
    sp.ship->cargo[COMMODITY_CRYSTAL_ORE] = 8.0f;
    ASSERT(test_set_ship_finished_units(sp.ship, COMMODITY_FERRITE_INGOT,
                                        20, MINING_GRADE_COMMON));

    uint8_t buf[PLAYER_SHIP_SIZE];
    int len = serialize_player_ship_bal(buf, 3, &sp, 1234.0f);

    ASSERT(len <= PLAYER_SHIP_SIZE);
    ASSERT_EQ_INT(buf[0], NET_MSG_PLAYER_SHIP);
    ASSERT_EQ_INT(buf[1], 3);
    ASSERT_EQ_FLOAT(read_f32_le(&buf[2]), 85.5f, 0.1f);
    ASSERT_EQ_FLOAT(read_f32_le(&buf[6]), 1234.0f, 0.1f);
    ASSERT_EQ_INT(buf[10], 1);   /* docked */
    ASSERT_EQ_INT(buf[11], 2);   /* station */
    ASSERT_EQ_INT(buf[12], 3);   /* mining_level */
    ASSERT_EQ_INT(buf[13], 2);   /* hold_level */
    ASSERT_EQ_INT(buf[14], 1);   /* tractor_level */
    ASSERT_EQ_INT(buf[15], 0);   /* reserved (was has_scaffold_kit) */
    ASSERT_EQ_FLOAT(read_f32_le(&buf[16]), 45.0f, 0.1f);   /* ferrite ore */
    ASSERT_EQ_FLOAT(read_f32_le(&buf[16 + 3*4]), 20.0f, 0.1f); /* ferrite ingot */
}

TEST(test_named_ingot_record_serializes_grade) {
    station_t st;
    memset(&st, 0, sizeof(st));
    ASSERT(station_manifest_bootstrap(&st));

    cargo_unit_t unit = {0};
    unit.kind = (uint8_t)CARGO_KIND_INGOT;
    unit.commodity = (uint8_t)COMMODITY_FERRITE_INGOT;
    unit.grade = (uint8_t)MINING_GRADE_RARE;
    unit.prefix_class = (uint8_t)INGOT_PREFIX_M;
    unit.recipe_id = (uint16_t)RECIPE_SMELT;
    unit.origin_station = 7;
    unit.quantity = 1;
    unit.mined_block = 0x0102030405060708ull;
    for (int i = 0; i < 32; i++) unit.pub[i] = (uint8_t)(0xA0 + i);
    ASSERT(manifest_push(&st.manifest, &unit));

    uint8_t buf[STATION_INGOTS_HEADER + NAMED_INGOT_RECORD_SIZE];
    int len = serialize_station_ingots(buf, 3, &st);
    ASSERT_EQ_INT(len, STATION_INGOTS_HEADER + NAMED_INGOT_RECORD_SIZE);
    ASSERT_EQ_INT(buf[0], NET_MSG_STATION_INGOTS);
    ASSERT_EQ_INT(buf[1], 3);
    ASSERT_EQ_INT(buf[2], 1);

    const uint8_t *p = &buf[STATION_INGOTS_HEADER];
    ASSERT_EQ_INT(p[32], INGOT_PREFIX_M);
    ASSERT_EQ_INT(p[33], COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(p[34], MINING_GRADE_RARE);
    ASSERT_EQ_INT(p[44], 7);
    ASSERT_EQ_INT(p[36], 0x08);
    ASSERT_EQ_INT(p[43], 0x01);

    station_cleanup(&st);
}

TEST(test_parse_input_valid) {
    input_intent_t intent;
    memset(&intent, 0, sizeof(intent));

    uint8_t msg[4] = {
        NET_MSG_INPUT,
        NET_INPUT_THRUST | NET_INPUT_LEFT | NET_INPUT_FIRE | NET_INPUT_BOOST,
        NET_ACTION_SELL_CARGO,
        0xFF  /* no mining target */
    };

    parse_input(msg, 4, &intent);
    ASSERT_EQ_FLOAT(intent.thrust, 1.0f, 0.01f);
    ASSERT_EQ_FLOAT(intent.turn, 1.0f, 0.01f);
    ASSERT(intent.mine);
    ASSERT(intent.boost);
    ASSERT(intent.service_sell);
}

TEST(test_parse_input_reverse_flag) {
    input_intent_t intent;
    memset(&intent, 0, sizeof(intent));

    uint8_t msg[4] = {
        NET_MSG_INPUT,
        NET_INPUT_BRAKE,
        NET_ACTION_NONE,
        0xFF
    };

    parse_input(msg, 4, &intent);
    ASSERT_EQ_FLOAT(intent.thrust, -1.0f, 0.01f);
    ASSERT(!intent.reverse_thrust);

    msg[1] = NET_INPUT_BRAKE | NET_INPUT_REVERSE;
    parse_input(msg, 4, &intent);
    ASSERT_EQ_FLOAT(intent.thrust, -1.0f, 0.01f);
    ASSERT(intent.reverse_thrust);
}

TEST(test_parse_input_too_short) {
    input_intent_t intent;
    memset(&intent, 0, sizeof(intent));
    intent.thrust = 99.0f;  /* canary value */

    uint8_t msg[3] = { NET_MSG_INPUT, 0xFF, 0 };
    parse_input(msg, 3, &intent);

    /* Too short (< 4 bytes) — should not modify intent */
    ASSERT_EQ_FLOAT(intent.thrust, 99.0f, 0.01f);
}

TEST(test_parse_input_no_action) {
    input_intent_t intent;
    memset(&intent, 0, sizeof(intent));

    uint8_t msg[4] = { NET_MSG_INPUT, NET_INPUT_THRUST, NET_ACTION_NONE, 0xFF };
    parse_input(msg, 4, &intent);

    ASSERT_EQ_FLOAT(intent.thrust, 1.0f, 0.01f);
    ASSERT(!intent.service_sell);
    ASSERT(!intent.interact);
}

TEST(test_parse_input_v2_uint16_mining_target) {
    input_intent_t intent;
    memset(&intent, 0, sizeof(intent));

    uint8_t msg[12] = {
        NET_MSG_INPUT,
        NET_INPUT_FIRE,
        NET_ACTION_NONE,
        0x2C, /* legacy low byte for target 300 */
        MINING_GRADE_COUNT,
        0xFF, 0xFF, 0xFF,
        0x34, 0x12, /* input seq */
        0x2C, 0x01  /* mining target 300 */
    };

    parse_input(msg, sizeof(msg), &intent);
    ASSERT(intent.mine);
    ASSERT_EQ_INT(intent.mining_target_hint, 300);

    msg[10] = 0xFF;
    msg[11] = 0xFF;
    parse_input(msg, sizeof(msg), &intent);
    ASSERT_EQ_INT(intent.mining_target_hint, -1);
}

TEST(test_parse_input_v3_action_id) {
    input_intent_t intent;
    memset(&intent, 0, sizeof(intent));

    uint8_t msg[14] = {
        NET_MSG_INPUT,
        NET_INPUT_THRUST,
        NET_ACTION_LAUNCH,
        0xFF,
        MINING_GRADE_COUNT,
        0xFF, 0xFF, 0xFF,
        0x78, 0x56,
        0xFF, 0xFF,
        0x34, 0x12
    };

    parse_input(msg, sizeof(msg), &intent);
    ASSERT_EQ_FLOAT(intent.thrust, 1.0f, 0.01f);
    ASSERT(intent.launch);
    ASSERT_EQ_INT((int)input_action_id(msg, sizeof(msg)), 0x1234);
    ASSERT_EQ_INT((int)input_action_id(msg, 12), 0);
}

TEST(test_parse_input_v4_client_tick) {
    uint8_t msg[NET_INPUT_MSG_SIZE] = {
        NET_MSG_INPUT,
        NET_INPUT_THRUST,
        NET_ACTION_NONE,
        0xFF,
        MINING_GRADE_COUNT,
        0xFF, 0xFF, 0xFF,
        0x78, 0x56,
        0xFF, 0xFF,
        0x34, 0x12,
        0xEF, 0xCD, 0xAB, 0x89,
        0x04, 0x03, 0x02, 0x01
    };

    ASSERT_EQ_INT((int)input_client_tick(msg, sizeof(msg)), (int)0x89ABCDEFu);
    ASSERT_EQ_INT((int)input_client_tick(msg, 14), 0);
    ASSERT_EQ_INT((int)input_client_sent_ms(msg, sizeof(msg)),
                  (int)0x01020304u);
    ASSERT_EQ_INT((int)input_client_sent_ms(msg, NET_INPUT_LEGACY_SIZE), 0);
}

TEST(test_socket_player_requires_session_for_gameplay) {
    WORLD_DECL;
    world_reset(&w);
    server_player_t *sp = &w.players[0];
    player_init_ship(sp, &w);
    sp->connected = true;
    sp->id = 0;
    sp->connection->conn = (void *)(uintptr_t)1;
    sp->session_ready = false;
    sp->docked = false;
    sp->ship->hull = ship_max_hull(sp->ship);
    sp->ship->pos = v2(0.0f, 0.0f);
    sp->input.thrust = 1.0f;

    world_sim_step(&w, SIM_DT);
    ASSERT(!sp->actual_thrusting);
    ASSERT_EQ_FLOAT(sp->ship->pos.x, 0.0f, 0.001f);
    ASSERT_EQ_FLOAT(sp->ship->pos.y, 0.0f, 0.001f);

    uint8_t input_msg[10] = {
        NET_MSG_INPUT,
        NET_INPUT_THRUST,
        NET_ACTION_NONE,
        0xFF,
        MINING_GRADE_COUNT,
        0xFF, 0xFF, 0xFF,
        0x2A, 0x00
    };
    server_input_dispatch_result_t input_result;
    ASSERT(!server_dispatch_input_message(&w, 0, input_msg,
                                          sizeof(input_msg), 0,
                                          &input_result));
    ASSERT_EQ_INT(sp->movement_queue_count, 0);

    uint8_t players[2 + MAX_PLAYERS * PLAYER_RECORD_SIZE];
    int players_len = serialize_all_player_states(players, w.players, w.tick);
    ASSERT_EQ_INT(players_len, 2);
    ASSERT_EQ_INT(players[0], NET_MSG_WORLD_PLAYERS);
    ASSERT_EQ_INT(players[1], 0);

    sp->session_ready = true;
    ASSERT(server_dispatch_input_message(&w, 0, input_msg,
                                         sizeof(input_msg), 0,
                                         &input_result));
    ASSERT_EQ_INT(sp->movement_queue_count, 1);
    players_len = serialize_all_player_states(players, w.players, w.tick);
    ASSERT_EQ_INT(players_len, 2 + PLAYER_RECORD_SIZE);
    ASSERT_EQ_INT(players[1], 1);
    ASSERT_EQ_INT(players[2], 0);
}

TEST(test_ticked_movement_input_applies_on_sim_tick) {
    WORLD_DECL;
    test_world_bind_ship_slots(&w);
    world_reset(&w);
    server_player_t *sp = &w.players[0];
    sp->connected = true;
    sp->id = 0;
    player_init_ship(sp, &w);

    input_intent_t intent = {0};
    intent.turn = 1.0f;
    intent.thrust = 1.0f;
    intent.mine = true;
    intent.mining_target_hint = 7;
    server_player_queue_movement_input(sp, &intent, 77, 2);

    world_sim_step(&w, SIM_DT);
    ASSERT_EQ_INT((int)w.tick, 1);
    ASSERT_EQ_INT((int)sp->last_input_seq, 0);
    ASSERT_EQ_FLOAT(sp->input.turn, 0.0f, 0.01f);

    world_sim_step(&w, SIM_DT);
    ASSERT_EQ_INT((int)w.tick, 2);
    ASSERT_EQ_INT((int)sp->last_input_seq, 77);
    ASSERT_EQ_INT((int)sp->last_input_tick, 2);
    ASSERT_EQ_FLOAT(sp->input.turn, 1.0f, 0.01f);
    ASSERT_EQ_FLOAT(sp->input.thrust, 1.0f, 0.01f);
    ASSERT(sp->input.mine);
    ASSERT_EQ_INT(sp->input.mining_target_hint, 7);

    world_cleanup(&w);
}

TEST(test_input_applied_carries_input_transport_timestamps) {
    WORLD_DECL;
    test_world_bind_ship_slots(&w);
    world_reset(&w);
    server_player_t *sp = &w.players[0];
    sp->connected = true;
    sp->id = 0;
    sp->session_ready = true;
    player_init_ship(sp, &w);

    uint8_t input_msg[NET_INPUT_MSG_SIZE] = {
        NET_MSG_INPUT,
        NET_INPUT_THRUST,
        NET_ACTION_NONE,
        0xFF,
        MINING_GRADE_COUNT,
        0xFF, 0xFF, 0xFF,
        0x4D, 0x00,
        0xFF, 0xFF,
        0x00, 0x00,
        0x02, 0x00, 0x00, 0x00,
        0xDD, 0xCC, 0xBB, 0xAA
    };
    server_input_dispatch_result_t result;
    ASSERT(server_dispatch_input_message(&w, 0, input_msg,
                                         sizeof(input_msg), 0x11223344u,
                                         &result));
    ASSERT_EQ_INT(sp->movement_queue_count, 1);

    world_sim_step(&w, SIM_DT);
    ASSERT_EQ_INT((int)sp->last_input_seq, 0);
    world_sim_step(&w, SIM_DT);
    ASSERT_EQ_INT((int)sp->last_input_seq, 77);
    ASSERT_EQ_INT((int)sp->last_input_client_sent_ms, (int)0xAABBCCDDu);
    ASSERT_EQ_INT((int)sp->last_input_server_recv_ms, (int)0x11223344u);

    input_applied_capture_t cap;
    memset(&cap, 0, sizeof(cap));
    ASSERT(server_emit_input_applied_if_changed(
        sp, 0, w.tick, input_applied_capture_sink, &cap));
    ASSERT_EQ_INT(cap.len, NET_INPUT_APPLIED_SIZE);
    ASSERT_EQ_INT((int)read_u32_le(&cap.data[11]), (int)0xAABBCCDDu);
    ASSERT_EQ_INT((int)read_u32_le(&cap.data[15]), (int)0x11223344u);
    ASSERT_EQ_INT((int)read_u32_le(&cap.data[19]), 0);

    world_cleanup(&w);
}

TEST(test_latency_pong_can_arrive_before_authoritative_input_ack) {
    WORLD_DECL;
    test_world_bind_ship_slots(&w);
    world_reset(&w);
    server_player_t *sp = &w.players[0];
    sp->connected = true;
    sp->id = 0;
    player_init_ship(sp, &w);

    input_intent_t intent = {0};
    intent.thrust = 1.0f;
    server_player_queue_movement_input(sp, &intent, 42, 6);

    uint8_t pong[NET_LATENCY_PONG_SIZE];
    int pong_len = serialize_latency_pong(pong, 99u, 1000u, 1001u, 1001u,
                                          1234u);
    ASSERT_EQ_INT(pong_len, NET_LATENCY_PONG_SIZE);
    ASSERT_EQ_INT(pong[0], NET_MSG_LATENCY_PONG);
    ASSERT_EQ_INT((int)read_u32_le(&pong[1]), 99);
    ASSERT_EQ_INT((int)read_u32_le(&pong[17]), 1234);

    uint8_t players[2 + MAX_PLAYERS * PLAYER_RECORD_SIZE];
    int players_len = serialize_all_player_states(players, w.players, w.tick);
    ASSERT_EQ_INT(players_len, 2 + PLAYER_RECORD_SIZE);
    ASSERT_EQ_INT(players[0], NET_MSG_WORLD_PLAYERS);
    ASSERT_EQ_INT(players[1], 1);
    ASSERT_EQ_INT((int)read_u16_le(&players[2 + 67]), 0);
    ASSERT_EQ_INT((int)read_u32_le(&players[2 + 69]), 0);
    ASSERT_EQ_INT((int)read_u32_le(&players[2 + 73]), 0);

    for (int tick = 1; tick < 6; tick++) {
        world_sim_step(&w, SIM_DT);
        players_len = serialize_all_player_states(players, w.players, w.tick);
        ASSERT_EQ_INT(players_len, 2 + PLAYER_RECORD_SIZE);
        ASSERT_EQ_INT((int)w.tick, tick);
        ASSERT_EQ_INT((int)read_u16_le(&players[2 + 67]), 0);
    }

    world_sim_step(&w, SIM_DT);
    ASSERT_EQ_INT((int)w.tick, 6);
    players_len = serialize_all_player_states(players, w.players, w.tick);
    ASSERT_EQ_INT(players_len, 2 + PLAYER_RECORD_SIZE);
    ASSERT_EQ_INT((int)read_u16_le(&players[2 + 67]), 42);
    ASSERT_EQ_INT((int)read_u32_le(&players[2 + 69]), 6);
    ASSERT_EQ_INT((int)read_u32_le(&players[2 + 73]), 6);

    world_cleanup(&w);
}

TEST(test_action_ack_roundtrip) {
    uint8_t buf[NET_ACTION_ACK_SIZE];
    int len = serialize_action_ack(buf, 0x1234, 0x5678,
                                   NET_ACTION_ACK_DUPLICATE,
                                   NET_ACTION_DOCK);

    ASSERT_EQ_INT(len, NET_ACTION_ACK_SIZE);
    ASSERT_EQ_INT(buf[0], NET_MSG_ACTION_ACK);
    ASSERT_EQ_INT((int)read_u16_le(&buf[1]), 0x1234);
    ASSERT_EQ_INT((int)read_u16_le(&buf[3]), 0x5678);
    ASSERT_EQ_INT(buf[5], NET_ACTION_ACK_DUPLICATE);
    ASSERT_EQ_INT(buf[6], NET_ACTION_DOCK);
}

TEST(test_action_result_roundtrip) {
    uint8_t buf[NET_ACTION_RESULT_SIZE];
    int len = serialize_action_result(buf, 0x1234, 0x5678,
                                      NET_ACTION_RESULT_OK,
                                      NET_ACTION_LAUNCH,
                                      0xAABBCCDDu);

    ASSERT_EQ_INT(len, NET_ACTION_RESULT_SIZE);
    ASSERT_EQ_INT(buf[0], NET_MSG_ACTION_RESULT);
    ASSERT_EQ_INT((int)read_u16_le(&buf[1]), 0x1234);
    ASSERT_EQ_INT((int)read_u16_le(&buf[3]), 0x5678);
    ASSERT_EQ_INT(buf[5], NET_ACTION_RESULT_OK);
    ASSERT_EQ_INT(buf[6], NET_ACTION_LAUNCH);
    ASSERT_EQ_INT((int)read_u32_le(&buf[7]), (int)0xAABBCCDDu);
}

TEST(test_input_applied_roundtrip) {
    uint8_t buf[NET_INPUT_APPLIED_SIZE];
    int len = serialize_input_applied(buf, 0x1234, 0xAABBCCDDu,
                                      0x01020304u, 0x11121314u,
                                      0x21222324u, 0x31323334u);

    ASSERT_EQ_INT(len, NET_INPUT_APPLIED_SIZE);
    ASSERT_EQ_INT(buf[0], NET_MSG_INPUT_APPLIED);
    ASSERT_EQ_INT((int)read_u16_le(&buf[1]), 0x1234);
    ASSERT_EQ_INT((int)read_u32_le(&buf[3]), (int)0xAABBCCDDu);
    ASSERT_EQ_INT((int)read_u32_le(&buf[7]), (int)0x01020304u);
    ASSERT_EQ_INT((int)read_u32_le(&buf[11]), (int)0x11121314u);
    ASSERT_EQ_INT((int)read_u32_le(&buf[15]), (int)0x21222324u);
    ASSERT_EQ_INT((int)read_u32_le(&buf[19]), (int)0x31323334u);
}

TEST(test_cargo_receipt_bundle_roundtrip) {
    cargo_receipt_chain_t chain;
    memset(&chain, 0, sizeof(chain));
    chain.len = 1;
    chain.links[0].event_id = 0x1122334455667788ull;

    uint8_t buf[3 + CARGO_RECEIPT_CHAIN_MAX_LEN * CARGO_RECEIPT_SIZE];
    int len = serialize_cargo_receipt_bundle(buf, &chain);

    ASSERT_EQ_INT(len, 3 + CARGO_RECEIPT_SIZE);
    ASSERT_EQ_INT(buf[0], NET_MSG_CARGO_RECEIPT_BUNDLE);
    ASSERT_EQ_INT((int)read_u16_le(&buf[1]), 1);

    cargo_receipt_t unpacked;
    memset(&unpacked, 0, sizeof(unpacked));
    ASSERT(cargo_receipt_unpack(&buf[3], &unpacked));
    ASSERT(unpacked.event_id == chain.links[0].event_id);
}

TEST(test_latency_pong_roundtrip) {
    uint8_t buf[NET_LATENCY_PONG_SIZE];
    int len = serialize_latency_pong(buf, 0x11223344u, 0x55667788u,
                                     0x99AABBCDu, 0xDDEEFF00u,
                                     0x01020304u);

    ASSERT_EQ_INT(len, NET_LATENCY_PONG_SIZE);
    ASSERT_EQ_INT(buf[0], NET_MSG_LATENCY_PONG);
    ASSERT_EQ_INT((int)read_u32_le(&buf[1]), (int)0x11223344u);
    ASSERT_EQ_INT((int)read_u32_le(&buf[5]), (int)0x55667788u);
    ASSERT_EQ_INT((int)read_u32_le(&buf[9]), (int)0x99AABBCDu);
    ASSERT_EQ_INT((int)read_u32_le(&buf[13]), (int)0xDDEEFF00u);
    ASSERT_EQ_INT((int)read_u32_le(&buf[17]), (int)0x01020304u);
}

static const uint8_t *find_protocol_stream(const uint8_t *buf, uint8_t msg) {
    int count = buf[7];
    for (int i = 0; i < count; i++) {
        const uint8_t *p = &buf[PROTOCOL_INFO_HEADER_SIZE +
                                i * PROTOCOL_INFO_STREAM_RECORD_SIZE];
        if (p[0] == msg) return p;
    }
    return NULL;
}

TEST(test_protocol_info_serializes_stream_map) {
    uint8_t buf[PROTOCOL_INFO_SIZE];
    int len = serialize_protocol_info(buf, 8, 50, 100, 250, 300, 2000);

    ASSERT(len >= PROTOCOL_INFO_HEADER_SIZE);
    ASSERT(len <= PROTOCOL_INFO_SIZE);
    ASSERT_EQ_INT(buf[0], NET_MSG_PROTOCOL_INFO);
    ASSERT_EQ_INT((int)read_u16_le(&buf[1]), (int)SIGNAL_PROTOCOL_VERSION);
    ASSERT(read_u32_le(&buf[3]) & SIGNAL_PROTOCOL_CAP_PROTOCOL_INFO);
    ASSERT(read_u32_le(&buf[3]) & SIGNAL_PROTOCOL_CAP_STATION_DIAG);
    ASSERT(read_u32_le(&buf[3]) & SIGNAL_PROTOCOL_CAP_HANDOFF_TICKETS);
    ASSERT(read_u32_le(&buf[3]) & SIGNAL_PROTOCOL_CAP_DELIVERY_SHIPMENTS);
    ASSERT(read_u32_le(&buf[3]) & SIGNAL_PROTOCOL_CAP_INPUT_APPLIED_ACK);
    ASSERT(read_u32_le(&buf[3]) & SIGNAL_PROTOCOL_CAP_PLAYER_KNOWN_LEDGER);
    ASSERT(read_u32_le(&buf[3]) & SIGNAL_PROTOCOL_CAP_ASTEROID_MOTION);
    ASSERT(read_u32_le(&buf[3]) & SIGNAL_PROTOCOL_CAP_ASTEROID_MOTION_Q);
    ASSERT(read_u32_le(&buf[3]) & SIGNAL_PROTOCOL_CAP_ASTEROID_POS_Q);
    ASSERT(read_u32_le(&buf[3]) & SIGNAL_PROTOCOL_CAP_ASTEROID_POS8_Q);
    ASSERT(read_u32_le(&buf[3]) & SIGNAL_PROTOCOL_CAP_ASTEROID_POSD_Q);
    ASSERT(read_u32_le(&buf[3]) & SIGNAL_PROTOCOL_CAP_ASTEROID_STATE_Q);
    ASSERT(read_u32_le(&buf[3]) & SIGNAL_PROTOCOL_CAP_ASTEROID_REMOVE);
    ASSERT(read_u32_le(&buf[3]) & SIGNAL_PROTOCOL_CAP_PLAYER_MOTION);
    ASSERT(read_u32_le(&buf[3]) & SIGNAL_PROTOCOL_CAP_NPC_MOTION);
    ASSERT(read_u32_le(&buf[3]) & SIGNAL_PROTOCOL_CAP_NPC_MOTION_Q);
    ASSERT(read_u32_le(&buf[3]) & SIGNAL_PROTOCOL_CAP_NPC_POS_Q);
    ASSERT(read_u32_le(&buf[3]) & SIGNAL_PROTOCOL_CAP_NPC_POSE_Q);
    ASSERT(read_u32_le(&buf[3]) & SIGNAL_PROTOCOL_CAP_NPC_LINEAR_Q);
    ASSERT(read_u32_le(&buf[3]) & SIGNAL_PROTOCOL_CAP_NPC_STATUS);
    ASSERT(read_u32_le(&buf[3]) & SIGNAL_PROTOCOL_CAP_NPC_STATUS8_Q);
    ASSERT(read_u32_le(&buf[3]) & SIGNAL_PROTOCOL_CAP_CARGO_POD_MOTION);
    ASSERT(read_u32_le(&buf[3]) & SIGNAL_PROTOCOL_CAP_CARGO_POD_MOTION_Q);
    ASSERT(read_u32_le(&buf[3]) & SIGNAL_PROTOCOL_CAP_CARGO_POD_REMOVE);
    ASSERT(read_u32_le(&buf[3]) & SIGNAL_PROTOCOL_CAP_SCAFFOLD_REMOVE);
    ASSERT(read_u32_le(&buf[3]) & SIGNAL_PROTOCOL_CAP_SCAFFOLD_MOTION_Q);
    ASSERT(read_u32_le(&buf[3]) & SIGNAL_PROTOCOL_CAP_INTERACTION_DRIFT);
    ASSERT(read_u32_le(&buf[3]) & SIGNAL_PROTOCOL_CAP_LATENCY_PONG_TICK);
    ASSERT_EQ_INT(buf[7], (len - PROTOCOL_INFO_HEADER_SIZE) /
                          PROTOCOL_INFO_STREAM_RECORD_SIZE);
    ASSERT(buf[7] <= PROTOCOL_INFO_STREAM_CAPACITY);

    const uint8_t *diag = find_protocol_stream(buf, NET_MSG_STATION_DIAG);
    ASSERT(diag != NULL);
    ASSERT_EQ_INT(diag[1], PROTOCOL_STREAM_CLASS_LIVE);
    ASSERT(read_u16_le(&diag[2]) & PROTOCOL_STREAM_FLAG_DIRTY_ONLY);
    ASSERT(read_u16_le(&diag[2]) & PROTOCOL_STREAM_FLAG_FIXED_SIZE);
    ASSERT_EQ_INT(read_u16_le(&diag[4]), 3);
    ASSERT_EQ_INT(read_u16_le(&diag[6]), 1);
    ASSERT_EQ_INT(read_u16_le(&diag[8]), MAX_MODULES_PER_STATION);
    ASSERT_EQ_INT(read_u16_le(&diag[10]), 300);

    const uint8_t *latency_pong = find_protocol_stream(
        buf, NET_MSG_LATENCY_PONG);
    ASSERT(latency_pong != NULL);
    ASSERT_EQ_INT(latency_pong[1], PROTOCOL_STREAM_CLASS_LIVE);
    ASSERT(read_u16_le(&latency_pong[2]) &
           PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT);
    ASSERT(read_u16_le(&latency_pong[2]) &
           PROTOCOL_STREAM_FLAG_FIXED_SIZE);
    ASSERT_EQ_INT(read_u16_le(&latency_pong[4]), NET_LATENCY_PONG_SIZE);

    const uint8_t *identity = find_protocol_stream(buf, NET_MSG_STATION_IDENTITY);
    ASSERT(identity != NULL);
    ASSERT_EQ_INT(identity[1], PROTOCOL_STREAM_CLASS_STATIC);
    ASSERT_EQ_INT(read_u16_le(&identity[4]), STATION_IDENTITY_SIZE);
    ASSERT_EQ_INT(read_u16_le(&identity[10]), 2000);

    const uint8_t *identity_q = find_protocol_stream(
        buf, NET_MSG_STATION_IDENTITY_Q);
    ASSERT(identity_q != NULL);
    ASSERT_EQ_INT(identity_q[1], PROTOCOL_STREAM_CLASS_STATIC);
    ASSERT(read_u16_le(&identity_q[2]) &
           PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT);
    ASSERT(read_u16_le(&identity_q[2]) &
           PROTOCOL_STREAM_FLAG_DIRTY_ONLY);
    ASSERT(read_u16_le(&identity_q[2]) &
           PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER);
    ASSERT(!(read_u16_le(&identity_q[2]) &
             PROTOCOL_STREAM_FLAG_FIXED_SIZE));
    ASSERT_EQ_INT(read_u16_le(&identity_q[4]),
                  STATION_IDENTITY_Q_HEADER_SIZE);
    ASSERT_EQ_INT(read_u16_le(&identity_q[6]), 0);
    ASSERT_EQ_INT(read_u16_le(&identity_q[10]), 2000);

    const uint8_t *players = find_protocol_stream(buf, NET_MSG_WORLD_PLAYERS);
    ASSERT(players != NULL);
    ASSERT_EQ_INT(read_u16_le(&players[6]), PLAYER_RECORD_SIZE);
    ASSERT_EQ_INT(read_u16_le(&players[10]), 50);

    const uint8_t *player_motion = find_protocol_stream(
        buf, NET_MSG_WORLD_PLAYER_MOTION);
    ASSERT(player_motion != NULL);
    ASSERT_EQ_INT(player_motion[1], PROTOCOL_STREAM_CLASS_LIVE);
    ASSERT(read_u16_le(&player_motion[2]) &
           PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER);
    ASSERT_EQ_INT(read_u16_le(&player_motion[4]),
                  PLAYER_MOTION_MSG_HEADER);
    ASSERT_EQ_INT(read_u16_le(&player_motion[6]),
                  PLAYER_MOTION_RECORD_SIZE);
    ASSERT_EQ_INT(read_u16_le(&player_motion[8]), MAX_PLAYERS);
    ASSERT_EQ_INT(read_u16_le(&player_motion[10]), 50);

    const uint8_t *player_motion_q = find_protocol_stream(
        buf, NET_MSG_WORLD_PLAYER_MOTION_Q);
    ASSERT(player_motion_q != NULL);
    ASSERT_EQ_INT(player_motion_q[1], PROTOCOL_STREAM_CLASS_LIVE);
    ASSERT(read_u16_le(&player_motion_q[2]) &
           PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER);
    ASSERT_EQ_INT(read_u16_le(&player_motion_q[4]),
                  PLAYER_MOTION_Q_MSG_HEADER);
    ASSERT_EQ_INT(read_u16_le(&player_motion_q[6]),
                  PLAYER_MOTION_Q_RECORD_SIZE);
    ASSERT_EQ_INT(read_u16_le(&player_motion_q[8]), MAX_PLAYERS);
    ASSERT_EQ_INT(read_u16_le(&player_motion_q[10]), 200);

    const uint8_t *player_motion_delta_q = find_protocol_stream(
        buf, NET_MSG_WORLD_PLAYER_MOTIOND_Q);
    ASSERT(player_motion_delta_q != NULL);
    ASSERT_EQ_INT(player_motion_delta_q[1], PROTOCOL_STREAM_CLASS_LIVE);
    ASSERT(read_u16_le(&player_motion_delta_q[2]) &
           PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER);
    ASSERT_EQ_INT(read_u16_le(&player_motion_delta_q[4]),
                  PLAYER_MOTIOND_Q_MSG_HEADER);
    ASSERT_EQ_INT(read_u16_le(&player_motion_delta_q[6]),
                  PLAYER_MOTIOND_Q_RECORD_SIZE);
    ASSERT_EQ_INT(read_u16_le(&player_motion_delta_q[8]), MAX_PLAYERS);
    ASSERT_EQ_INT(read_u16_le(&player_motion_delta_q[10]), 200);

    const uint8_t *player_posed_q = find_protocol_stream(
        buf, NET_MSG_WORLD_PLAYER_POSED_Q);
    ASSERT(player_posed_q != NULL);
    ASSERT_EQ_INT(player_posed_q[1], PROTOCOL_STREAM_CLASS_LIVE);
    ASSERT(read_u16_le(&player_posed_q[2]) &
           PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER);
    ASSERT_EQ_INT(read_u16_le(&player_posed_q[4]),
                  PLAYER_POSED_Q_MSG_HEADER);
    ASSERT_EQ_INT(read_u16_le(&player_posed_q[6]),
                  PLAYER_POSED_Q_RECORD_SIZE);
    ASSERT_EQ_INT(read_u16_le(&player_posed_q[8]), MAX_PLAYERS);
    ASSERT_EQ_INT(read_u16_le(&player_posed_q[10]), 200);

    const uint8_t *player_mixed_q = find_protocol_stream(
        buf, NET_MSG_WORLD_PLAYER_MOTIONM_Q);
    ASSERT(player_mixed_q != NULL);
    ASSERT_EQ_INT(player_mixed_q[1], PROTOCOL_STREAM_CLASS_LIVE);
    ASSERT(read_u16_le(&player_mixed_q[2]) &
           PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER);
    ASSERT_EQ_INT(read_u16_le(&player_mixed_q[4]),
                  PLAYER_MOTIONM_Q_MSG_HEADER);
    ASSERT_EQ_INT(read_u16_le(&player_mixed_q[6]), 0);
    ASSERT_EQ_INT(read_u16_le(&player_mixed_q[8]), MAX_PLAYERS);
    ASSERT_EQ_INT(read_u16_le(&player_mixed_q[10]), 200);

    const uint8_t *player_dock = find_protocol_stream(
        buf, NET_MSG_WORLD_PLAYER_DOCK_Q);
    ASSERT(player_dock != NULL);
    ASSERT_EQ_INT(player_dock[1], PROTOCOL_STREAM_CLASS_LIVE);
    ASSERT(read_u16_le(&player_dock[2]) &
           PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER);
    ASSERT_EQ_INT(read_u16_le(&player_dock[4]), PLAYER_DOCK_MSG_HEADER);
    ASSERT_EQ_INT(read_u16_le(&player_dock[6]), PLAYER_DOCK_RECORD_SIZE);
    ASSERT_EQ_INT(read_u16_le(&player_dock[8]), MAX_PLAYERS);
    ASSERT_EQ_INT(read_u16_le(&player_dock[10]), 50);

    const uint8_t *asteroids = find_protocol_stream(buf, NET_MSG_WORLD_ASTEROIDS);
    ASSERT(asteroids != NULL);
    ASSERT_EQ_INT(asteroids[1], PROTOCOL_STREAM_CLASS_LIVE);
    ASSERT(read_u16_le(&asteroids[2]) & PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER);
    ASSERT(read_u16_le(&asteroids[2]) & PROTOCOL_STREAM_FLAG_DIRTY_ONLY);
    ASSERT_EQ_INT(read_u16_le(&asteroids[4]), ASTEROID_MSG_HEADER);
    ASSERT_EQ_INT(read_u16_le(&asteroids[6]), ASTEROID_RECORD_SIZE);
    ASSERT_EQ_INT(read_u16_le(&asteroids[8]), MAX_ASTEROIDS);
    ASSERT_EQ_INT(read_u16_le(&asteroids[10]), 100);

    const uint8_t *asteroids_q = find_protocol_stream(
        buf, NET_MSG_WORLD_ASTEROIDS_Q);
    ASSERT(asteroids_q != NULL);
    ASSERT_EQ_INT(asteroids_q[1], PROTOCOL_STREAM_CLASS_LIVE);
    ASSERT(read_u16_le(&asteroids_q[2]) &
           PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER);
    ASSERT(read_u16_le(&asteroids_q[2]) &
           PROTOCOL_STREAM_FLAG_DIRTY_ONLY);
    ASSERT_EQ_INT(read_u16_le(&asteroids_q[4]), ASTEROID_Q_MSG_HEADER);
    ASSERT_EQ_INT(read_u16_le(&asteroids_q[6]), ASTEROID_Q_RECORD_SIZE);
    ASSERT_EQ_INT(read_u16_le(&asteroids_q[8]), MAX_ASTEROIDS);
    ASSERT_EQ_INT(read_u16_le(&asteroids_q[10]), 100);

    const uint8_t *asteroids8_q = find_protocol_stream(
        buf, NET_MSG_WORLD_ASTEROIDS8_Q);
    ASSERT(asteroids8_q != NULL);
    ASSERT_EQ_INT(asteroids8_q[1], PROTOCOL_STREAM_CLASS_LIVE);
    ASSERT(read_u16_le(&asteroids8_q[2]) &
           PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER);
    ASSERT(read_u16_le(&asteroids8_q[2]) &
           PROTOCOL_STREAM_FLAG_DIRTY_ONLY);
    ASSERT_EQ_INT(read_u16_le(&asteroids8_q[4]), ASTEROID8_Q_MSG_HEADER);
    ASSERT_EQ_INT(read_u16_le(&asteroids8_q[6]), ASTEROID8_Q_RECORD_SIZE);
    ASSERT_EQ_INT(read_u16_le(&asteroids8_q[8]), 256);
    ASSERT_EQ_INT(read_u16_le(&asteroids8_q[10]), 100);

    const uint8_t *asteroid_motion = find_protocol_stream(
        buf, NET_MSG_WORLD_ASTEROID_MOTION);
    ASSERT(asteroid_motion != NULL);
    ASSERT_EQ_INT(asteroid_motion[1], PROTOCOL_STREAM_CLASS_LIVE);
    ASSERT(read_u16_le(&asteroid_motion[2]) &
           PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER);
    ASSERT_EQ_INT(read_u16_le(&asteroid_motion[4]),
                  ASTEROID_MOTION_MSG_HEADER);
    ASSERT_EQ_INT(read_u16_le(&asteroid_motion[6]),
                  ASTEROID_MOTION_RECORD_SIZE);
    ASSERT_EQ_INT(read_u16_le(&asteroid_motion[8]), MAX_ASTEROIDS);
    ASSERT_EQ_INT(read_u16_le(&asteroid_motion[10]), 100);

    const uint8_t *asteroid_motion_q = find_protocol_stream(
        buf, NET_MSG_WORLD_ASTEROID_MOTION_Q);
    ASSERT(asteroid_motion_q != NULL);
    ASSERT_EQ_INT(asteroid_motion_q[1], PROTOCOL_STREAM_CLASS_LIVE);
    ASSERT(read_u16_le(&asteroid_motion_q[2]) &
           PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER);
    ASSERT_EQ_INT(read_u16_le(&asteroid_motion_q[4]),
                  ASTEROID_MOTION_Q_MSG_HEADER);
    ASSERT_EQ_INT(read_u16_le(&asteroid_motion_q[6]),
                  ASTEROID_MOTION_Q_RECORD_SIZE);
    ASSERT_EQ_INT(read_u16_le(&asteroid_motion_q[8]), MAX_ASTEROIDS);
    ASSERT_EQ_INT(read_u16_le(&asteroid_motion_q[10]), 100);

    const uint8_t *asteroid_pos_q = find_protocol_stream(
        buf, NET_MSG_WORLD_ASTEROID_POS_Q);
    ASSERT(asteroid_pos_q != NULL);
    ASSERT_EQ_INT(asteroid_pos_q[1], PROTOCOL_STREAM_CLASS_LIVE);
    ASSERT(read_u16_le(&asteroid_pos_q[2]) &
           PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER);
    ASSERT_EQ_INT(read_u16_le(&asteroid_pos_q[4]),
                  ASTEROID_POS_Q_MSG_HEADER);
    ASSERT_EQ_INT(read_u16_le(&asteroid_pos_q[6]),
                  ASTEROID_POS_Q_RECORD_SIZE);
    ASSERT_EQ_INT(read_u16_le(&asteroid_pos_q[8]), MAX_ASTEROIDS);
    ASSERT_EQ_INT(read_u16_le(&asteroid_pos_q[10]), 100);

    const uint8_t *asteroid_pos8_q = find_protocol_stream(
        buf, NET_MSG_WORLD_ASTEROID_POS8_Q);
    ASSERT(asteroid_pos8_q != NULL);
    ASSERT_EQ_INT(asteroid_pos8_q[1], PROTOCOL_STREAM_CLASS_LIVE);
    ASSERT(read_u16_le(&asteroid_pos8_q[2]) &
           PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER);
    ASSERT_EQ_INT(read_u16_le(&asteroid_pos8_q[4]),
                  ASTEROID_POS8_Q_MSG_HEADER);
    ASSERT_EQ_INT(read_u16_le(&asteroid_pos8_q[6]),
                  ASTEROID_POS8_Q_RECORD_SIZE);
    ASSERT_EQ_INT(read_u16_le(&asteroid_pos8_q[8]), 256);
    ASSERT_EQ_INT(read_u16_le(&asteroid_pos8_q[10]), 100);

    const uint8_t *asteroid_posd_q = find_protocol_stream(
        buf, NET_MSG_WORLD_ASTEROID_POSD_Q);
    ASSERT(asteroid_posd_q != NULL);
    ASSERT_EQ_INT(asteroid_posd_q[1], PROTOCOL_STREAM_CLASS_LIVE);
    ASSERT(read_u16_le(&asteroid_posd_q[2]) &
           PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER);
    ASSERT_EQ_INT(read_u16_le(&asteroid_posd_q[4]),
                  ASTEROID_POSD_Q_MSG_HEADER);
    ASSERT_EQ_INT(read_u16_le(&asteroid_posd_q[6]),
                  ASTEROID_POSD_Q_RECORD_SIZE);
    ASSERT_EQ_INT(read_u16_le(&asteroid_posd_q[8]), MAX_ASTEROIDS);
    ASSERT_EQ_INT(read_u16_le(&asteroid_posd_q[10]), 100);

    const uint8_t *asteroid_posd8_q = find_protocol_stream(
        buf, NET_MSG_WORLD_ASTEROID_POSD8_Q);
    ASSERT(asteroid_posd8_q != NULL);
    ASSERT_EQ_INT(asteroid_posd8_q[1], PROTOCOL_STREAM_CLASS_LIVE);
    ASSERT(read_u16_le(&asteroid_posd8_q[2]) &
           PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER);
    ASSERT_EQ_INT(read_u16_le(&asteroid_posd8_q[4]),
                  ASTEROID_POSD8_Q_MSG_HEADER);
    ASSERT_EQ_INT(read_u16_le(&asteroid_posd8_q[6]),
                  ASTEROID_POSD8_Q_RECORD_SIZE);
    ASSERT_EQ_INT(read_u16_le(&asteroid_posd8_q[8]), 256);
    ASSERT_EQ_INT(read_u16_le(&asteroid_posd8_q[10]), 100);

    const uint8_t *asteroid_state_q = find_protocol_stream(
        buf, NET_MSG_WORLD_ASTEROID_STATE_Q);
    ASSERT(asteroid_state_q != NULL);
    ASSERT_EQ_INT(asteroid_state_q[1], PROTOCOL_STREAM_CLASS_LIVE);
    ASSERT(read_u16_le(&asteroid_state_q[2]) &
           PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER);
    ASSERT(read_u16_le(&asteroid_state_q[2]) &
           PROTOCOL_STREAM_FLAG_DIRTY_ONLY);
    ASSERT_EQ_INT(read_u16_le(&asteroid_state_q[4]),
                  ASTEROID_STATE_Q_MSG_HEADER);
    ASSERT_EQ_INT(read_u16_le(&asteroid_state_q[6]),
                  ASTEROID_STATE_Q_RECORD_SIZE);
    ASSERT_EQ_INT(read_u16_le(&asteroid_state_q[8]), MAX_ASTEROIDS);
    ASSERT_EQ_INT(read_u16_le(&asteroid_state_q[10]), 100);

    const uint8_t *asteroid_remove = find_protocol_stream(
        buf, NET_MSG_WORLD_ASTEROID_REMOVE);
    ASSERT(asteroid_remove != NULL);
    ASSERT_EQ_INT(asteroid_remove[1], PROTOCOL_STREAM_CLASS_LIVE);
    ASSERT(read_u16_le(&asteroid_remove[2]) &
           PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER);
    ASSERT(read_u16_le(&asteroid_remove[2]) &
           PROTOCOL_STREAM_FLAG_DIRTY_ONLY);
    ASSERT_EQ_INT(read_u16_le(&asteroid_remove[4]),
                  ASTEROID_REMOVE_MSG_HEADER);
    ASSERT_EQ_INT(read_u16_le(&asteroid_remove[6]),
                  ASTEROID_REMOVE_RECORD_SIZE);
    ASSERT_EQ_INT(read_u16_le(&asteroid_remove[8]), MAX_ASTEROIDS);
    ASSERT_EQ_INT(read_u16_le(&asteroid_remove[10]), 100);

    const uint8_t *npcs = find_protocol_stream(buf, NET_MSG_WORLD_NPCS);
    ASSERT(npcs != NULL);
    ASSERT_EQ_INT(npcs[1], PROTOCOL_STREAM_CLASS_LIVE);
    ASSERT(read_u16_le(&npcs[2]) & PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER);
    ASSERT_EQ_INT(read_u16_le(&npcs[4]), 2);
    ASSERT_EQ_INT(read_u16_le(&npcs[6]), NPC_RECORD_SIZE);
    ASSERT_EQ_INT(read_u16_le(&npcs[8]), MAX_NPC_SHIPS);
    ASSERT_EQ_INT(read_u16_le(&npcs[10]), 100);

    const uint8_t *npc_motion = find_protocol_stream(
        buf, NET_MSG_WORLD_NPC_MOTION);
    ASSERT(npc_motion != NULL);
    ASSERT_EQ_INT(npc_motion[1], PROTOCOL_STREAM_CLASS_LIVE);
    ASSERT(read_u16_le(&npc_motion[2]) &
           PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER);
    ASSERT_EQ_INT(read_u16_le(&npc_motion[4]), NPC_MOTION_MSG_HEADER);
    ASSERT_EQ_INT(read_u16_le(&npc_motion[6]), NPC_MOTION_RECORD_SIZE);
    ASSERT_EQ_INT(read_u16_le(&npc_motion[8]), MAX_NPC_SHIPS);
    ASSERT_EQ_INT(read_u16_le(&npc_motion[10]), 100);

    const uint8_t *npc_motion_q = find_protocol_stream(
        buf, NET_MSG_WORLD_NPC_MOTION_Q);
    ASSERT(npc_motion_q != NULL);
    ASSERT_EQ_INT(npc_motion_q[1], PROTOCOL_STREAM_CLASS_LIVE);
    ASSERT(read_u16_le(&npc_motion_q[2]) &
           PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER);
    ASSERT_EQ_INT(read_u16_le(&npc_motion_q[4]), NPC_MOTION_Q_MSG_HEADER);
    ASSERT_EQ_INT(read_u16_le(&npc_motion_q[6]), NPC_MOTION_Q_RECORD_SIZE);
    ASSERT_EQ_INT(read_u16_le(&npc_motion_q[8]), MAX_NPC_SHIPS);
    ASSERT_EQ_INT(read_u16_le(&npc_motion_q[10]), 100);

    const uint8_t *npc_motion8_q = find_protocol_stream(
        buf, NET_MSG_WORLD_NPC_MOTION8_Q);
    ASSERT(npc_motion8_q != NULL);
    ASSERT_EQ_INT(npc_motion8_q[1], PROTOCOL_STREAM_CLASS_LIVE);
    ASSERT(read_u16_le(&npc_motion8_q[2]) &
           PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER);
    ASSERT_EQ_INT(read_u16_le(&npc_motion8_q[4]),
                  NPC_MOTION8_Q_MSG_HEADER);
    ASSERT_EQ_INT(read_u16_le(&npc_motion8_q[6]),
                  NPC_MOTION8_Q_RECORD_SIZE);
    ASSERT_EQ_INT(read_u16_le(&npc_motion8_q[8]), MAX_NPC_SHIPS);
    ASSERT_EQ_INT(read_u16_le(&npc_motion8_q[10]), 100);

    const uint8_t *npc_pos_q = find_protocol_stream(
        buf, NET_MSG_WORLD_NPC_POS_Q);
    ASSERT(npc_pos_q != NULL);
    ASSERT_EQ_INT(npc_pos_q[1], PROTOCOL_STREAM_CLASS_LIVE);
    ASSERT(read_u16_le(&npc_pos_q[2]) &
           PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER);
    ASSERT_EQ_INT(read_u16_le(&npc_pos_q[4]), NPC_POS_Q_MSG_HEADER);
    ASSERT_EQ_INT(read_u16_le(&npc_pos_q[6]), NPC_POS_Q_RECORD_SIZE);
    ASSERT_EQ_INT(read_u16_le(&npc_pos_q[8]), MAX_NPC_SHIPS);
    ASSERT_EQ_INT(read_u16_le(&npc_pos_q[10]), 100);

    const uint8_t *npc_pose_q = find_protocol_stream(
        buf, NET_MSG_WORLD_NPC_POSE_Q);
    ASSERT(npc_pose_q != NULL);
    ASSERT_EQ_INT(npc_pose_q[1], PROTOCOL_STREAM_CLASS_LIVE);
    ASSERT(read_u16_le(&npc_pose_q[2]) &
           PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER);
    ASSERT_EQ_INT(read_u16_le(&npc_pose_q[4]), NPC_POSE_Q_MSG_HEADER);
    ASSERT_EQ_INT(read_u16_le(&npc_pose_q[6]), NPC_POSE_Q_RECORD_SIZE);
    ASSERT_EQ_INT(read_u16_le(&npc_pose_q[8]), MAX_NPC_SHIPS);
    ASSERT_EQ_INT(read_u16_le(&npc_pose_q[10]), 100);

    const uint8_t *npc_linear_q = find_protocol_stream(
        buf, NET_MSG_WORLD_NPC_LINEAR_Q);
    ASSERT(npc_linear_q != NULL);
    ASSERT_EQ_INT(npc_linear_q[1], PROTOCOL_STREAM_CLASS_LIVE);
    ASSERT(read_u16_le(&npc_linear_q[2]) &
           PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER);
    ASSERT_EQ_INT(read_u16_le(&npc_linear_q[4]), NPC_LINEAR_Q_MSG_HEADER);
    ASSERT_EQ_INT(read_u16_le(&npc_linear_q[6]), NPC_LINEAR_Q_RECORD_SIZE);
    ASSERT_EQ_INT(read_u16_le(&npc_linear_q[8]), MAX_NPC_SHIPS);
    ASSERT_EQ_INT(read_u16_le(&npc_linear_q[10]), 100);

    const uint8_t *npc_status = find_protocol_stream(
        buf, NET_MSG_WORLD_NPC_STATUS);
    ASSERT(npc_status != NULL);
    ASSERT_EQ_INT(npc_status[1], PROTOCOL_STREAM_CLASS_LIVE);
    ASSERT(read_u16_le(&npc_status[2]) &
           PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER);
    ASSERT_EQ_INT(read_u16_le(&npc_status[4]), NPC_STATUS_MSG_HEADER);
    ASSERT_EQ_INT(read_u16_le(&npc_status[6]), NPC_STATUS_RECORD_SIZE);
    ASSERT_EQ_INT(read_u16_le(&npc_status[8]), MAX_NPC_SHIPS);
    ASSERT_EQ_INT(read_u16_le(&npc_status[10]), 100);

    const uint8_t *npc_status8 = find_protocol_stream(
        buf, NET_MSG_WORLD_NPC_STATUS8_Q);
    ASSERT(npc_status8 != NULL);
    ASSERT_EQ_INT(npc_status8[1], PROTOCOL_STREAM_CLASS_LIVE);
    ASSERT(read_u16_le(&npc_status8[2]) &
           PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER);
    ASSERT_EQ_INT(read_u16_le(&npc_status8[4]), NPC_STATUS8_MSG_HEADER);
    ASSERT_EQ_INT(read_u16_le(&npc_status8[6]), NPC_STATUS8_RECORD_SIZE);
    ASSERT_EQ_INT(read_u16_le(&npc_status8[8]), MAX_NPC_SHIPS);
    ASSERT_EQ_INT(read_u16_le(&npc_status8[10]), 100);

    const uint8_t *scaffolds = find_protocol_stream(
        buf, NET_MSG_WORLD_SCAFFOLDS);
    ASSERT(scaffolds != NULL);
    ASSERT_EQ_INT(scaffolds[1], PROTOCOL_STREAM_CLASS_LIVE);
    ASSERT(read_u16_le(&scaffolds[2]) &
           PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER);
    ASSERT_EQ_INT(read_u16_le(&scaffolds[4]), 2);
    ASSERT_EQ_INT(read_u16_le(&scaffolds[6]), SCAFFOLD_RECORD_SIZE);
    ASSERT_EQ_INT(read_u16_le(&scaffolds[8]), MAX_SCAFFOLDS);
    ASSERT_EQ_INT(read_u16_le(&scaffolds[10]), 100);

    const uint8_t *scaffold_motion_q = find_protocol_stream(
        buf, NET_MSG_WORLD_SCAFFOLD_MOTION_Q);
    ASSERT(scaffold_motion_q != NULL);
    ASSERT_EQ_INT(scaffold_motion_q[1], PROTOCOL_STREAM_CLASS_LIVE);
    ASSERT(read_u16_le(&scaffold_motion_q[2]) &
           PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER);
    ASSERT_EQ_INT(read_u16_le(&scaffold_motion_q[4]),
                  SCAFFOLD_MOTION_Q_MSG_HEADER);
    ASSERT_EQ_INT(read_u16_le(&scaffold_motion_q[6]),
                  SCAFFOLD_MOTION_Q_RECORD_SIZE);
    ASSERT_EQ_INT(read_u16_le(&scaffold_motion_q[8]), MAX_SCAFFOLDS);
    ASSERT_EQ_INT(read_u16_le(&scaffold_motion_q[10]), 100);

    const uint8_t *scaffold_remove = find_protocol_stream(
        buf, NET_MSG_WORLD_SCAFFOLD_REMOVE);
    ASSERT(scaffold_remove != NULL);
    ASSERT_EQ_INT(scaffold_remove[1], PROTOCOL_STREAM_CLASS_LIVE);
    ASSERT(read_u16_le(&scaffold_remove[2]) &
           PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER);
    ASSERT_EQ_INT(read_u16_le(&scaffold_remove[4]),
                  SCAFFOLD_REMOVE_MSG_HEADER);
    ASSERT_EQ_INT(read_u16_le(&scaffold_remove[6]),
                  SCAFFOLD_REMOVE_RECORD_SIZE);
    ASSERT_EQ_INT(read_u16_le(&scaffold_remove[8]), MAX_SCAFFOLDS);
    ASSERT_EQ_INT(read_u16_le(&scaffold_remove[10]), 100);

    const uint8_t *cargo_pod_motion = find_protocol_stream(
        buf, NET_MSG_WORLD_CARGO_POD_MOTION);
    ASSERT(cargo_pod_motion != NULL);
    ASSERT_EQ_INT(cargo_pod_motion[1], PROTOCOL_STREAM_CLASS_LIVE);
    ASSERT(read_u16_le(&cargo_pod_motion[2]) &
           PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER);
    ASSERT_EQ_INT(read_u16_le(&cargo_pod_motion[4]),
                  CARGO_POD_MOTION_MSG_HEADER);
    ASSERT_EQ_INT(read_u16_le(&cargo_pod_motion[6]),
                  CARGO_POD_MOTION_RECORD_SIZE);
    ASSERT_EQ_INT(read_u16_le(&cargo_pod_motion[8]), MAX_CARGO_PODS);
    ASSERT_EQ_INT(read_u16_le(&cargo_pod_motion[10]), 100);

    const uint8_t *cargo_pod_motion_q = find_protocol_stream(
        buf, NET_MSG_WORLD_CARGO_POD_MOTION_Q);
    ASSERT(cargo_pod_motion_q != NULL);
    ASSERT_EQ_INT(cargo_pod_motion_q[1], PROTOCOL_STREAM_CLASS_LIVE);
    ASSERT(read_u16_le(&cargo_pod_motion_q[2]) &
           PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER);
    ASSERT_EQ_INT(read_u16_le(&cargo_pod_motion_q[4]),
                  CARGO_POD_MOTION_Q_MSG_HEADER);
    ASSERT_EQ_INT(read_u16_le(&cargo_pod_motion_q[6]),
                  CARGO_POD_MOTION_Q_RECORD_SIZE);
    ASSERT_EQ_INT(read_u16_le(&cargo_pod_motion_q[8]), MAX_CARGO_PODS);
    ASSERT_EQ_INT(read_u16_le(&cargo_pod_motion_q[10]), 100);

    const uint8_t *cargo_pods_q = find_protocol_stream(
        buf, NET_MSG_WORLD_CARGO_PODS_Q);
    ASSERT(cargo_pods_q != NULL);
    ASSERT_EQ_INT(cargo_pods_q[1], PROTOCOL_STREAM_CLASS_LIVE);
    ASSERT(read_u16_le(&cargo_pods_q[2]) &
           PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT);
    ASSERT(read_u16_le(&cargo_pods_q[2]) &
           PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER);
    ASSERT_EQ_INT(read_u16_le(&cargo_pods_q[4]), 2);
    ASSERT_EQ_INT(read_u16_le(&cargo_pods_q[6]),
                  CARGO_POD_Q_RECORD_SIZE);
    ASSERT_EQ_INT(read_u16_le(&cargo_pods_q[8]), MAX_CARGO_PODS);
    ASSERT_EQ_INT(read_u16_le(&cargo_pods_q[10]), 100);

    const uint8_t *cargo_pod_linear_q = find_protocol_stream(
        buf, NET_MSG_WORLD_CARGO_POD_LINEAR_Q);
    ASSERT(cargo_pod_linear_q != NULL);
    ASSERT_EQ_INT(cargo_pod_linear_q[1], PROTOCOL_STREAM_CLASS_LIVE);
    ASSERT(read_u16_le(&cargo_pod_linear_q[2]) &
           PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER);
    ASSERT_EQ_INT(read_u16_le(&cargo_pod_linear_q[4]),
                  CARGO_POD_LINEAR_Q_MSG_HEADER);
    ASSERT_EQ_INT(read_u16_le(&cargo_pod_linear_q[6]),
                  CARGO_POD_LINEAR_Q_RECORD_SIZE);
    ASSERT_EQ_INT(read_u16_le(&cargo_pod_linear_q[8]), MAX_CARGO_PODS);
    ASSERT_EQ_INT(read_u16_le(&cargo_pod_linear_q[10]), 100);

    const uint8_t *cargo_pod_remove = find_protocol_stream(
        buf, NET_MSG_WORLD_CARGO_POD_REMOVE);
    ASSERT(cargo_pod_remove != NULL);
    ASSERT_EQ_INT(cargo_pod_remove[1], PROTOCOL_STREAM_CLASS_LIVE);
    ASSERT(read_u16_le(&cargo_pod_remove[2]) &
           PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER);
    ASSERT_EQ_INT(read_u16_le(&cargo_pod_remove[4]),
                  CARGO_POD_REMOVE_MSG_HEADER);
    ASSERT_EQ_INT(read_u16_le(&cargo_pod_remove[6]),
                  CARGO_POD_REMOVE_RECORD_SIZE);
    ASSERT_EQ_INT(read_u16_le(&cargo_pod_remove[8]), MAX_CARGO_PODS);
    ASSERT_EQ_INT(read_u16_le(&cargo_pod_remove[10]), 100);

    const uint8_t *interactions = find_protocol_stream(
        buf, NET_MSG_WORLD_INTERACTIONS);
    ASSERT(interactions != NULL);
    ASSERT_EQ_INT(interactions[1], PROTOCOL_STREAM_CLASS_LIVE);
    ASSERT(read_u16_le(&interactions[2]) & PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT);
    ASSERT(read_u16_le(&interactions[2]) &
           PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER);
    ASSERT_EQ_INT(read_u16_le(&interactions[4]), 2);
    ASSERT_EQ_INT(read_u16_le(&interactions[6]), INTERACTION_RECORD_SIZE);
    ASSERT_EQ_INT(read_u16_le(&interactions[8]), SIM_MAX_INTERACTIONS);
    ASSERT_EQ_INT(read_u16_le(&interactions[10]), 100);

    const uint8_t *interactions_q = find_protocol_stream(
        buf, NET_MSG_WORLD_INTERACTIONS_Q);
    ASSERT(interactions_q != NULL);
    ASSERT_EQ_INT(interactions_q[1], PROTOCOL_STREAM_CLASS_LIVE);
    ASSERT(read_u16_le(&interactions_q[2]) &
           PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT);
    ASSERT(read_u16_le(&interactions_q[2]) &
           PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER);
    ASSERT_EQ_INT(read_u16_le(&interactions_q[4]), 2);
    ASSERT_EQ_INT(read_u16_le(&interactions_q[6]),
                  INTERACTION_Q_RECORD_SIZE);
    ASSERT_EQ_INT(read_u16_le(&interactions_q[8]), SIM_MAX_INTERACTIONS);
    ASSERT_EQ_INT(read_u16_le(&interactions_q[10]), 100);

    const uint8_t *interaction_drift = find_protocol_stream(
        buf, NET_MSG_WORLD_INTERACTION_DRIFT);
    ASSERT(interaction_drift != NULL);
    ASSERT_EQ_INT(interaction_drift[1], PROTOCOL_STREAM_CLASS_LIVE);
    ASSERT(read_u16_le(&interaction_drift[2]) &
           PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT);
    ASSERT(read_u16_le(&interaction_drift[2]) &
           PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER);
    ASSERT_EQ_INT(read_u16_le(&interaction_drift[4]),
                  INTERACTION_DRIFT_MSG_HEADER);
    ASSERT_EQ_INT(read_u16_le(&interaction_drift[6]),
                  INTERACTION_DRIFT_RECORD_SIZE);
    ASSERT_EQ_INT(read_u16_le(&interaction_drift[8]), SIM_MAX_INTERACTIONS);
    ASSERT_EQ_INT(read_u16_le(&interaction_drift[10]), 100);

    const uint8_t *input = find_protocol_stream(buf, NET_MSG_INPUT);
    ASSERT(input != NULL);
    ASSERT_EQ_INT((int)NET_INPUT_ACTIVE_HEARTBEAT_MS, 250);
    ASSERT_EQ_INT((int)NET_INPUT_ACTIVE_ACK_HEARTBEAT_MS, 1000);
    ASSERT_EQ_INT((int)NET_INPUT_IDLE_HEARTBEAT_MS, 1000);
    ASSERT_EQ_INT(read_u16_le(&input[4]), NET_INPUT_MSG_SIZE);
    ASSERT_EQ_INT(read_u16_le(&input[10]), NET_INPUT_ACTIVE_HEARTBEAT_MS);

    const uint8_t *input_applied = find_protocol_stream(buf, NET_MSG_INPUT_APPLIED);
    ASSERT(input_applied != NULL);
    ASSERT_EQ_INT(input_applied[1], PROTOCOL_STREAM_CLASS_LIVE);
    ASSERT(read_u16_le(&input_applied[2]) & PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT);
    ASSERT(read_u16_le(&input_applied[2]) & PROTOCOL_STREAM_FLAG_PER_PLAYER);
    ASSERT(read_u16_le(&input_applied[2]) & PROTOCOL_STREAM_FLAG_FIXED_SIZE);
    ASSERT_EQ_INT(read_u16_le(&input_applied[4]), NET_INPUT_APPLIED_SIZE);
    ASSERT_EQ_INT(read_u16_le(&input_applied[10]), 8);

    const uint8_t *known_ledger = find_protocol_stream(
        buf, NET_MSG_PLAYER_KNOWN_LEDGER);
    ASSERT(known_ledger != NULL);
    ASSERT_EQ_INT(known_ledger[1], PROTOCOL_STREAM_CLASS_PLAYER);
    ASSERT(read_u16_le(&known_ledger[2]) & PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT);
    ASSERT(read_u16_le(&known_ledger[2]) & PROTOCOL_STREAM_FLAG_PER_PLAYER);
    ASSERT(read_u16_le(&known_ledger[2]) & PROTOCOL_STREAM_FLAG_DIRTY_ONLY);
    ASSERT_EQ_INT(read_u16_le(&known_ledger[4]),
                  PLAYER_KNOWN_LEDGER_HEADER);
    ASSERT_EQ_INT(read_u16_le(&known_ledger[6]),
                  PLAYER_KNOWN_LEDGER_RECORD_SIZE);
    ASSERT_EQ_INT(read_u16_le(&known_ledger[8]),
                  PLAYER_KNOWN_LEDGER_MAX_RECORDS);

    const uint8_t *contracts = find_protocol_stream(buf, NET_MSG_CONTRACTS);
    ASSERT(contracts != NULL);
    ASSERT_EQ_INT(read_u16_le(&contracts[6]), CONTRACT_RECORD_SIZE);
    ASSERT_EQ_INT(CONTRACT_RECORD_SIZE, 104);

    const uint8_t *world_stations_q = find_protocol_stream(
        buf, NET_MSG_WORLD_STATIONS_Q);
    ASSERT(world_stations_q != NULL);
    ASSERT_EQ_INT(world_stations_q[1], PROTOCOL_STREAM_CLASS_ECON);
    ASSERT(read_u16_le(&world_stations_q[2]) &
           PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT);
    ASSERT(read_u16_le(&world_stations_q[2]) &
           PROTOCOL_STREAM_FLAG_DIRTY_ONLY);
    ASSERT_EQ_INT(read_u16_le(&world_stations_q[4]),
                  STATION_Q_HEADER_SIZE);
    ASSERT_EQ_INT(read_u16_le(&world_stations_q[6]), 0);
    ASSERT_EQ_INT(read_u16_le(&world_stations_q[8]), MAX_STATIONS);
    ASSERT_EQ_INT(read_u16_le(&world_stations_q[10]), 100);

    const uint8_t *contracts_q = find_protocol_stream(buf, NET_MSG_CONTRACTS_Q);
    ASSERT(contracts_q != NULL);
    ASSERT_EQ_INT(contracts_q[1], PROTOCOL_STREAM_CLASS_ECON);
    ASSERT(read_u16_le(&contracts_q[2]) &
           PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT);
    ASSERT(read_u16_le(&contracts_q[2]) &
           PROTOCOL_STREAM_FLAG_DIRTY_ONLY);
    ASSERT_EQ_INT(read_u16_le(&contracts_q[4]), CONTRACT_Q_HEADER_SIZE);
    ASSERT_EQ_INT(read_u16_le(&contracts_q[6]), 0);
    ASSERT_EQ_INT(read_u16_le(&contracts_q[8]), MAX_CONTRACTS);
    ASSERT_EQ_INT(read_u16_le(&contracts_q[10]), 100);

    const uint8_t *player_manifest = find_protocol_stream(buf, NET_MSG_PLAYER_MANIFEST);
    ASSERT(player_manifest != NULL);
    ASSERT(read_u16_le(&player_manifest[2]) & PROTOCOL_STREAM_FLAG_PER_PLAYER);
    ASSERT_EQ_INT(read_u16_le(&player_manifest[4]), PLAYER_MANIFEST_HEADER);
    ASSERT_EQ_INT(read_u16_le(&player_manifest[6]), PLAYER_MANIFEST_ENTRY);

    const uint8_t *delivery_ledger = find_protocol_stream(buf, NET_MSG_DELIVERY_LEDGER);
    ASSERT(delivery_ledger != NULL);
    ASSERT_EQ_INT(delivery_ledger[1], PROTOCOL_STREAM_CLASS_PLAYER);
    ASSERT(read_u16_le(&delivery_ledger[2]) & PROTOCOL_STREAM_FLAG_PER_PLAYER);
    ASSERT(read_u16_le(&delivery_ledger[2]) & PROTOCOL_STREAM_FLAG_DIRTY_ONLY);
    ASSERT_EQ_INT(read_u16_le(&delivery_ledger[4]), DELIVERY_LEDGER_HEADER);
    ASSERT_EQ_INT(read_u16_le(&delivery_ledger[6]), DELIVERY_LEDGER_RECORD_SIZE);
    ASSERT_EQ_INT(read_u16_le(&delivery_ledger[8]), DELIVERY_LEDGER_MAX_RECORDS);

    const uint8_t *handoff_request = find_protocol_stream(buf, NET_MSG_HANDOFF_REQUEST);
    ASSERT(handoff_request != NULL);
    ASSERT_EQ_INT(handoff_request[1], PROTOCOL_STREAM_CLASS_AUTH);
    ASSERT(read_u16_le(&handoff_request[2]) & PROTOCOL_STREAM_FLAG_FIXED_SIZE);
    ASSERT_EQ_INT(read_u16_le(&handoff_request[4]), NET_HANDOFF_REQUEST_SIZE);

    const uint8_t *handoff_present = find_protocol_stream(buf, NET_MSG_HANDOFF_PRESENT);
    ASSERT(handoff_present != NULL);
    ASSERT_EQ_INT(handoff_present[1], PROTOCOL_STREAM_CLASS_AUTH);
    ASSERT_EQ_INT(read_u16_le(&handoff_present[4]),
                  1 + HANDOFF_TICKET_SIZE + 4 + HANDOFF_SHIP_SNAPSHOT_HEADER_SIZE);
    ASSERT_EQ_INT(read_u16_le(&handoff_present[6]),
                  HANDOFF_CARGO_UNIT_WIRE_SIZE + 1 +
                  CARGO_RECEIPT_CHAIN_MAX_LEN * CARGO_RECEIPT_SIZE);
    ASSERT_EQ_INT(read_u16_le(&handoff_present[8]),
                  HANDOFF_SHIP_SNAPSHOT_MAX_CARGO);
}

TEST(test_buy_event_serializes_cost_and_quantity) {
    sim_events_t events;
    memset(&events, 0, sizeof(events));
    events.count = 1;
    events.events[0] = (sim_event_t){
        .type = SIM_EVENT_BUY,
        .player_id = 3,
        .buy = {
            .station = 2,
            .commodity = COMMODITY_FERRITE_INGOT,
            .grade = MINING_GRADE_RARE,
            .cost = 127,
            .quantity = 4,
        },
    };

    uint8_t buf[2 + NET_EVENT_RECORD_SIZE];
    int len = serialize_events(buf, &events);

    ASSERT_EQ_INT(len, 2 + NET_EVENT_RECORD_SIZE);
    ASSERT_EQ_INT(buf[0], NET_MSG_EVENTS);
    ASSERT_EQ_INT(buf[1], 1);

    const uint8_t *p = &buf[2];
    ASSERT_EQ_INT(p[0], SIM_EVENT_BUY);
    ASSERT_EQ_INT(p[1], 3);
    ASSERT_EQ_INT(p[2], 2);
    ASSERT_EQ_INT(p[3], COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(p[4], MINING_GRADE_RARE);
    ASSERT_EQ_INT((int)read_u32_le(&p[5]), 127);
    ASSERT_EQ_INT((int)read_u16_le(&p[9]), 4);
}

TEST(test_events_for_recipient_filters_local_only_damage) {
    sim_events_t events;
    memset(&events, 0, sizeof(events));
    events.count = 3;
    events.events[0] = (sim_event_t){
        .type = SIM_EVENT_DAMAGE,
        .player_id = 2,
        .damage = {
            .amount = 7.0f,
            .source_x = 10.0f,
            .source_y = 20.0f,
        },
    };
    events.events[1] = (sim_event_t){
        .type = SIM_EVENT_DAMAGE,
        .player_id = 5,
        .damage = {
            .amount = 3.0f,
            .source_x = 30.0f,
            .source_y = 40.0f,
        },
    };
    events.events[2] = (sim_event_t){
        .type = SIM_EVENT_DEATH,
        .player_id = 5,
        .death = {
            .cause = DEATH_CAUSE_ASTEROID,
        },
    };

    uint8_t buf[2 + SIM_MAX_EVENTS * NET_EVENT_RECORD_SIZE];
    int len = serialize_events_for_recipient(buf, &events, 2);

    ASSERT_EQ_INT(len, 2 + 2 * NET_EVENT_RECORD_SIZE);
    ASSERT_EQ_INT(buf[0], NET_MSG_EVENTS);
    ASSERT_EQ_INT(buf[1], 2);
    ASSERT_EQ_INT(buf[2], SIM_EVENT_DAMAGE);
    ASSERT_EQ_INT(buf[3], 2);
    ASSERT_EQ_FLOAT(read_f32_le(&buf[4]), 7.0f, 0.001f);
    ASSERT_EQ_INT(buf[2 + NET_EVENT_RECORD_SIZE], SIM_EVENT_DEATH);
    ASSERT_EQ_INT(buf[3 + NET_EVENT_RECORD_SIZE], 5);

    len = serialize_events_for_recipient(buf, &events, 4);
    ASSERT_EQ_INT(len, 2 + NET_EVENT_RECORD_SIZE);
    ASSERT_EQ_INT(buf[1], 1);
    ASSERT_EQ_INT(buf[2], SIM_EVENT_DEATH);

    len = serialize_events_for_recipient(buf, &events, 5);
    ASSERT_EQ_INT(len, 2 + 2 * NET_EVENT_RECORD_SIZE);
    ASSERT_EQ_INT(buf[2], SIM_EVENT_DAMAGE);
    ASSERT_EQ_INT(buf[3], 5);
    ASSERT_EQ_INT(buf[2 + NET_EVENT_RECORD_SIZE], SIM_EVENT_DEATH);
}

TEST(test_parse_input_action_accumulates) {
    input_intent_t intent;
    memset(&intent, 0, sizeof(intent));

    /* First input: dock action */
    uint8_t msg1[4] = { NET_MSG_INPUT, 0, NET_ACTION_DOCK, 0xFF };
    parse_input(msg1, 4, &intent);
    ASSERT(intent.dock);
    ASSERT(!intent.launch);
    ASSERT(intent.interact);

    /* Second input: sell action — should OR in, not replace */
    uint8_t msg2[4] = { NET_MSG_INPUT, 0, NET_ACTION_SELL_CARGO, 0xFF };
    parse_input(msg2, 4, &intent);
    ASSERT(intent.dock);           /* still true from first */
    ASSERT(intent.interact);       /* still true from first */
    ASSERT(intent.service_sell);   /* added by second */
}

TEST(test_parse_input_launch_keeps_semantic_action) {
    input_intent_t intent;
    memset(&intent, 0, sizeof(intent));

    uint8_t msg[4] = { NET_MSG_INPUT, 0, NET_ACTION_LAUNCH, 0xFF };
    parse_input(msg, 4, &intent);

    ASSERT(intent.launch);
    ASSERT(!intent.dock);
    ASSERT(intent.interact);
}

void register_protocol_main_tests(void) {
    TEST_SECTION("\nProtocol roundtrip tests:\n");
    RUN(test_wire_codec_roundtrips_and_fails_closed_on_bounds);
    RUN(test_roundtrip_player_state);
    RUN(test_authoritative_player_state_includes_ack_tail);
    RUN(test_roundtrip_batched_player_states);
    RUN(test_player_states_for_recipient_excludes_self);
    RUN(test_player_motion_stream_excludes_recipient_and_docked_players);
    RUN(test_player_motion_q_stream_quantizes_remote_players);
    RUN(test_player_motion_delta_q_stream_uses_baseline);
    RUN(test_player_motion_delta_q_skips_predicted_motion_until_heartbeat);
    RUN(test_player_motion_mixed_q_combines_delta_and_pose_records);
    RUN(test_player_motion_mixed_q_coalesces_clean_heartbeats);
    RUN(test_player_motion_delta_q_falls_back_when_delta_exceeds_i8);
    RUN(test_player_dock_stream_excludes_recipient_and_updates_status_flags);
    RUN(test_world_players_semantic_hash_ignores_pose_and_input_ack_tail);
    RUN(test_roundtrip_asteroids);
    RUN(test_asteroid_identity_budget_trickles_background_first_visible);
    RUN(test_asteroid_delta_suppresses_clean_static_repeat);
    RUN(test_asteroid_delta_sends_dirty_or_moving_repeat);
    RUN(test_asteroid_delta_throttles_clean_moving_repeat_by_tick);
    RUN(test_asteroid_delta_towed_fragments_use_tighter_motion_gate);
    RUN(test_asteroid_delta_throttles_far_slow_moving_repeat);
    RUN(test_asteroid_delta_relaxes_outer_near_slow_motion);
    RUN(test_asteroid_delta_keeps_old_far_cadence_quiet);
    RUN(test_asteroid_delta_keeps_old_near_heartbeat_quiet);
    RUN(test_asteroid_delta_relaxes_slow_near_safety_heartbeat);
    RUN(test_asteroid_delta_relaxes_crawl_safety_heartbeat);
    RUN(test_asteroid_delta_sends_crawl_when_prediction_diverges);
    RUN(test_asteroid_delta_keeps_old_very_far_cadence_quiet);
    RUN(test_asteroid_delta_relaxes_far_fast_moving_repeat);
    RUN(test_asteroid_delta_uses_far_error_budget_for_far_fast_motion);
    RUN(test_asteroid_delta_uses_very_far_error_budget);
    RUN(test_asteroid_delta_uses_motion_stream_for_clean_moving_repeat);
    RUN(test_asteroid_delta_quantizes_near_motion_when_available);
    RUN(test_asteroid_delta_elides_unchanged_quantized_velocity);
    RUN(test_asteroid_delta_uses_pos8_q_for_low_index_position_only);
    RUN(test_asteroid_position_only_prefers_signed_byte_delta_streams);
    RUN(test_asteroid_position_delta_falls_back_when_delta_exceeds_i8);
    RUN(test_asteroid_identity_prefers_compact_quantized_upserts);
    RUN(test_asteroid_delta_uses_pos_q_for_small_velocity_drift);
    RUN(test_asteroid_delta_uses_quantized_motion_stream_for_far_repeat);
    RUN(test_asteroid_delta_uses_quantized_motion_stream_for_settling);
    RUN(test_asteroid_delta_uses_compact_state_stream_for_known_dirty);
    RUN(test_asteroid_identity_change_forces_full_upsert);
    RUN(test_asteroid_cache_invalidation_preserves_pending_removal);
    RUN(test_asteroid_delta_coalesces_dirty_state_stream_per_player);
    RUN(test_asteroid_delta_sends_inactive_removal);
    RUN(test_asteroid_delta_uses_compact_removal_stream_when_available);
    RUN(test_roundtrip_asteroids_full_skips_inactive_slots);
    RUN(test_roundtrip_cargo_pods);
    RUN(test_roundtrip_cargo_pods_q_quantizes_visual_pose);
    RUN(test_world_cargo_pods_semantic_hash_ignores_pose_drift);
    RUN(test_world_cargo_pods_q_semantic_hash_ignores_pose_drift);
    RUN(test_world_cargo_pods_metadata_refresh_uses_sparse_safety_heartbeat);
    RUN(test_cargo_pod_delta_uses_compact_removal_stream_when_available);
    RUN(test_cargo_pod_q_delta_uses_compact_identity_and_removal);
    RUN(test_cargo_pod_motion_stream_uses_relevance_filter);
    RUN(test_cargo_pod_motion_q_stream_quantizes_pose);
    RUN(test_cargo_pod_motion_linear_q_uses_position_velocity_when_rotation_matches);
    RUN(test_cargo_pod_motion_prediction_gate_skips_predicted_pose);
    RUN(test_cargo_pod_motion_prediction_gate_sends_divergence);
    RUN(test_scaffold_delta_uses_compact_removal_stream_when_available);
    RUN(test_roundtrip_interactions);
    RUN(test_roundtrip_interactions_q_quantizes_visual_tail);
    RUN(test_world_interactions_semantic_hash_ignores_endpoint_drift);
    RUN(test_world_interactions_q_semantic_hash_ignores_visual_tail);
    RUN(test_world_interactions_metadata_refresh_uses_sparse_safety_heartbeat);
    RUN(test_interaction_drift_stream_quantizes_visual_fields);
    RUN(test_interaction_drift_repeat_uses_visual_cadence);
    RUN(test_interaction_streams_use_relevance_filter);
    RUN(test_roundtrip_npcs);
    RUN(test_npc_snapshot_serializes_embedded_ship_tow_slot);
    RUN(test_world_npcs_semantic_hash_ignores_pose_drift);
    RUN(test_world_npcs_metadata_refresh_uses_sparse_safety_heartbeat);
    RUN(test_world_npc_status_semantic_hash_ignores_thrust_only);
    RUN(test_world_npc_status8_semantic_hash_ignores_thrust_only);
    RUN(test_npc_motion_stream_uses_relevance_filter);
    RUN(test_npc_motion_q_stream_quantizes_pose);
    RUN(test_npc_motion8_q_stream_uses_byte_velocity_and_angle);
    RUN(test_npc_motion_pos_q_uses_position_only_when_baseline_matches);
    RUN(test_npc_motion_pose_q_uses_pose_when_angle_changes_only);
    RUN(test_npc_motion_linear_q_uses_position_velocity_when_angle_matches);
    RUN(test_npc_motion_prediction_gate_skips_predicted_pose);
    RUN(test_npc_motion_prediction_gate_sends_divergence);
    RUN(test_npc_status_stream_serializes_visual_status);
    RUN(test_npc_status8_stream_serializes_low_refs_and_rejects_high_refs);
    RUN(test_relevance_filtered_world_snapshots);
    RUN(test_world_snapshot_emitter_sequence_shared);
    RUN(test_world_snapshot_emits_compact_asteroid_motion_stream);
    RUN(test_world_snapshot_prioritizes_local_towed_asteroid_identity);
    RUN(test_world_snapshot_defers_asteroids_while_docked);
    RUN(test_world_snapshot_defers_live_drift_while_docked);
    RUN(test_world_time_snapshot_reconciles_at_low_cadence);
    RUN(test_private_snapshot_emitter_sequence_shared);
    RUN(test_private_snapshot_emits_local_authoritative_baseline);
    RUN(test_private_snapshot_emits_idle_authoritative_heartbeat);
    RUN(test_station_snapshot_emitter_sequence_shared);
    RUN(test_fracture_update_emitter_shared);
    RUN(test_fracture_challenge_rebroadcast_suppresses_seen_players);
    RUN(test_fracture_resolve_retry_suppresses_seen_players);
    RUN(test_input_applied_emitter_sends_only_on_sequence_change);
    RUN(test_pending_input_ack_coalesces_to_latest_sequence);
    RUN(test_authoritative_player_state_emitter_sends_only_on_sequence_change);
    RUN(test_pending_input_ack_emits_single_authoritative_state);
    RUN(test_pending_input_ack_adaptive_prefers_tiny_clean_ack);
    RUN(test_pending_input_ack_adaptive_promotes_first_or_forced_state);
    RUN(test_pending_input_ack_adaptive_promotes_drifted_state);
    RUN(test_pending_input_ack_adaptive_promotes_heartbeat_state);
    RUN(test_sim_event_transport_hooks_cover_freshness_buckets);
    RUN(test_pending_action_result_status_shared);
    RUN(test_hail_response_serializes_reason_tail);
    RUN(test_npc_role_default_hull_mapping_covers_tow);
    RUN(test_roundtrip_inspect_snapshot_npc_manifest_chain);
    RUN(test_inspect_snapshot_npc_expands_matching_receipt_chain);
    RUN(test_inspect_snapshot_npc_retrieves_matching_station_receipt_chain);
    RUN(test_roundtrip_inspect_snapshot_player_manifest_chain);
    RUN(test_inspect_snapshot_npc_includes_market_memory_diagnostics);
    RUN(test_inspect_snapshot_npc_expands_matching_job_source_memory);
    RUN(test_inspect_snapshot_npc_includes_job_offer_diagnostics);
    RUN(test_inspect_snapshot_npc_includes_hnn_trace_diagnostics);
    RUN(test_inspect_snapshot_groups_anonymous_ingots_by_grade);
    RUN(test_inspect_snapshot_groups_finished_goods_by_grade);
    RUN(test_inspect_snapshot_keeps_named_ingots_individual);
    RUN(test_roundtrip_stations);
    RUN(test_payload_cache_suppresses_unchanged_world_stations_per_connection);
    RUN(test_station_identity_serializes_module_commodities);
    RUN(test_world_stations_q_omits_zero_inventory_slots);
    RUN(test_station_identity_serializes_operator_text);
    RUN(test_station_identity_q_compacts_sparse_text_and_lists);
    RUN(test_station_identity_serializes_pending_ship_builds);
    RUN(test_station_identity_serializes_faction_trailer);
    RUN(test_station_identity_semantic_hash_ignores_ring_drift);
    RUN(test_payload_cache_suppresses_unchanged_station_identity_per_connection);
    RUN(test_deferable_snapshot_classification_preserves_ack_lane);
    RUN(test_deferable_snapshot_backpressure_reserves_control_lane);
    RUN(test_bug92_station_record_size_matches_buffer);
    RUN(test_player_known_contract_mask_uses_compact_contract_ordinals);
    RUN(test_player_known_ledger_serializes_station_balances);
    RUN(test_delivery_contract_action_serializes);
    RUN(test_contracts_q_omits_zero_optional_tails);
    RUN(test_contracts_semantic_hash_ignores_age_only);
    RUN(test_delivery_ledger_serializes_player_shipments);
    RUN(test_bug93_hint_mines_small_shard_with_minor_desync);
    RUN(test_roundtrip_player_ship);
    RUN(test_named_ingot_record_serializes_grade);
    RUN(test_parse_input_valid);
    RUN(test_parse_input_reverse_flag);
    RUN(test_parse_input_too_short);
    RUN(test_parse_input_no_action);
    RUN(test_parse_input_v2_uint16_mining_target);
    RUN(test_parse_input_v3_action_id);
    RUN(test_parse_input_v4_client_tick);
    RUN(test_socket_player_requires_session_for_gameplay);
    RUN(test_ticked_movement_input_applies_on_sim_tick);
    RUN(test_input_applied_carries_input_transport_timestamps);
    RUN(test_latency_pong_can_arrive_before_authoritative_input_ack);
    RUN(test_action_ack_roundtrip);
    RUN(test_action_result_roundtrip);
    RUN(test_input_applied_roundtrip);
    RUN(test_cargo_receipt_bundle_roundtrip);
    RUN(test_latency_pong_roundtrip);
    RUN(test_protocol_info_serializes_stream_map);
    RUN(test_buy_event_serializes_cost_and_quantity);
    RUN(test_events_for_recipient_filters_local_only_damage);
    RUN(test_parse_input_action_accumulates);
    RUN(test_parse_input_launch_keeps_semantic_action);
}
