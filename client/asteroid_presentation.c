#include "asteroid_presentation.h"

#include "sim_physics.h"

#include <math.h>
#include <string.h>

static bool finite_vec(vec2 value)
{
    return isfinite(value.x) && isfinite(value.y);
}

static float vec_length(vec2 value)
{
    return sqrtf(value.x * value.x + value.y * value.y);
}

static bool asteroid_station_tow_active(
    const world_t *authority, int asteroid_index)
{
    if (!authority) return false;
    for (int i = 0; i < authority->interactions.count; i++) {
        const sim_interaction_t *interaction =
            &authority->interactions.items[i];
        if (interaction->type == SIM_INTERACTION_TRACTOR_BEAM &&
            interaction->visual ==
                SIM_INTERACTION_VISUAL_STATION_FRAGMENT_TRACTOR &&
            interaction->target.type ==
                SIM_INTERACTION_ENTITY_ASTEROID &&
            interaction->target.index == asteroid_index) {
            return true;
        }
    }
    return false;
}

asteroid_motion_class_t asteroid_motion_classify(
    const world_t *authority, int asteroid_index)
{
    if (!authority || asteroid_index < 0 ||
        asteroid_index >= MAX_ASTEROIDS) {
        return ASTEROID_MOTION_LOOSE;
    }
    const asteroid_t *asteroid =
        &authority->asteroids[asteroid_index];
    if (asteroid_station_tow_active(authority, asteroid_index))
        return ASTEROID_MOTION_STATION_TOW;
    if (asteroid_tractor_player(asteroid) >= 0)
        return ASTEROID_MOTION_PLAYER_TOW;
    if (asteroid_tractor_npc(asteroid) >= 0)
        return ASTEROID_MOTION_NPC_TOW;
    if (asteroid_is_ballistic(asteroid))
        return ASTEROID_MOTION_BALLISTIC;
    return ASTEROID_MOTION_LOOSE;
}

void asteroid_presentation_predict_motion(
    const asteroid_t *base, float elapsed, bool tow_driven,
    vec2 *out_pos, vec2 *out_vel)
{
    asteroid_presentation_predict_motion_accelerated(
        base, elapsed, tow_driven, v2(0.0f, 0.0f), false,
        out_pos, out_vel);
}

void asteroid_presentation_predict_motion_accelerated(
    const asteroid_t *base, float elapsed, bool tow_driven,
    vec2 acceleration, bool acceleration_valid,
    vec2 *out_pos, vec2 *out_vel)
{
    if (!out_pos || !out_vel) return;
    *out_pos = base ? base->pos : v2(0.0f, 0.0f);
    *out_vel = base ? base->vel : v2(0.0f, 0.0f);
    if (!base || !isfinite(elapsed) || elapsed <= 0.0f) return;

    if (tow_driven) {
        float accel_time = acceleration_valid && finite_vec(acceleration)
            ? fminf(elapsed, ASTEROID_PRESENTATION_ACCEL_HORIZON_SEC)
            : 0.0f;
        float accel_len = vec_length(acceleration);
        if (!isfinite(accel_len) ||
            accel_len > ASTEROID_PRESENTATION_MAX_ACCEL) {
            accel_time = 0.0f;
        }
        vec2 accelerated_vel = v2_add(
            base->vel, v2_scale(acceleration, accel_time));
        vec2 accelerated_pos = v2_add(
            base->pos,
            v2_add(v2_scale(base->vel, accel_time),
                   v2_scale(acceleration,
                            0.5f * accel_time * accel_time)));
        float coast_time = elapsed - accel_time;
        *out_pos = v2_add(
            accelerated_pos,
            v2_scale(accelerated_vel, coast_time));
        *out_vel = accelerated_vel;
        return;
    }

    /* Match sim_step_asteroid_dynamics(): integrate position, then apply
     * rational ambient drag once per fixed tick.  The exponential is the
     * smooth continuation between ticks and remains exact on tick edges. */
    static float drag_decay_rate;
    static float drag_displacement_limit;
    if (drag_decay_rate <= 0.0f) {
        float drag_step =
            1.0f / (1.0f + ASTEROID_AMBIENT_DRAG * SIM_DT);
        drag_decay_rate = -logf(drag_step) / SIM_DT;
        drag_displacement_limit = SIM_DT / (1.0f - drag_step);
    }
    float retained = expf(-drag_decay_rate * elapsed);
    float displacement_scale =
        drag_displacement_limit * (1.0f - retained);
    *out_pos = v2_add(base->pos,
                      v2_scale(base->vel, displacement_scale));
    *out_vel = v2_scale(base->vel, retained);
}

bool asteroid_presentation_acceleration_gate(
    float *out_max_position_error,
    float *out_max_velocity_error)
{
    const float render_dt = 1.0f / 60.0f;
    const float acceleration_value = 380.0f;
    asteroid_t packet = {0};
    packet.active = true;
    vec2 estimated_acceleration = v2(0.0f, 0.0f);
    vec2 last_packet_velocity = v2(0.0f, 0.0f);
    bool have_packet_velocity = false;
    bool acceleration_valid = false;
    float packet_elapsed = 0.0f;
    float max_position_error = 0.0f;
    float max_velocity_error = 0.0f;

    for (int frame = 0; frame <= 180; frame++) {
        float time = (float)frame * render_dt;
        vec2 authority_pos = v2(
            0.5f * acceleration_value * time * time, 0.0f);
        vec2 authority_vel = v2(acceleration_value * time, 0.0f);
        if ((frame % 6) == 0) {
            if (have_packet_velocity) {
                estimated_acceleration = v2_scale(
                    v2_sub(authority_vel, last_packet_velocity),
                    1.0f / 0.1f);
                acceleration_valid = true;
            }
            packet.pos = authority_pos;
            packet.vel = authority_vel;
            last_packet_velocity = authority_vel;
            have_packet_velocity = true;
            packet_elapsed = 0.0f;
        }

        vec2 presented_pos = {0};
        vec2 presented_vel = {0};
        asteroid_presentation_predict_motion_accelerated(
            &packet, packet_elapsed, true,
            estimated_acceleration, acceleration_valid,
            &presented_pos, &presented_vel);
        if (frame >= 12) {
            float position_error = vec_length(v2_sub(
                authority_pos, presented_pos));
            float velocity_error = vec_length(v2_sub(
                authority_vel, presented_vel));
            if (max_position_error < position_error)
                max_position_error = position_error;
            if (max_velocity_error < velocity_error)
                max_velocity_error = velocity_error;
        }
        packet_elapsed += render_dt;
    }

    if (out_max_position_error)
        *out_max_position_error = max_position_error;
    if (out_max_velocity_error)
        *out_max_velocity_error = max_velocity_error;
    return isfinite(max_position_error) &&
        isfinite(max_velocity_error) &&
        max_position_error <= 0.01f &&
        max_velocity_error <= 0.01f;
}

static bool asteroid_presentation_identity_compatible(
    const asteroid_t *client_asteroid,
    const asteroid_t *authority_asteroid)
{
    if (!client_asteroid || !authority_asteroid ||
        !client_asteroid->active || !authority_asteroid->active) {
        return false;
    }
    return client_asteroid->fracture_child ==
            authority_asteroid->fracture_child &&
        client_asteroid->tier == authority_asteroid->tier &&
        client_asteroid->commodity == authority_asteroid->commodity;
}

asteroid_presentation_action_t asteroid_presentation_resolve(
    const world_t *authority, const asteroid_t *client_asteroid,
    int asteroid_index, float render_ahead,
    asteroid_t *out_presented,
    asteroid_motion_class_t *out_motion_class)
{
    if (!authority || !client_asteroid || !out_presented ||
        asteroid_index < 0 || asteroid_index >= MAX_ASTEROIDS) {
        return ASTEROID_PRESENTATION_SKIP;
    }
    const asteroid_t *authoritative =
        &authority->asteroids[asteroid_index];
    if (!authoritative->active)
        return client_asteroid->active
            ? ASTEROID_PRESENTATION_RETIRE
            : ASTEROID_PRESENTATION_SKIP;
    asteroid_motion_class_t motion_class =
        asteroid_motion_classify(authority, asteroid_index);
    if (out_motion_class) *out_motion_class = motion_class;
    if (!asteroid_presentation_identity_compatible(
            client_asteroid, authoritative)) {
        return ASTEROID_PRESENTATION_SKIP;
    }

    if (!isfinite(render_ahead) || render_ahead < 0.0f)
        render_ahead = 0.0f;
    if (render_ahead > SIM_DT) render_ahead = SIM_DT;

    *out_presented = *client_asteroid;
    bool tow_driven =
        motion_class == ASTEROID_MOTION_PLAYER_TOW ||
        motion_class == ASTEROID_MOTION_NPC_TOW ||
        motion_class == ASTEROID_MOTION_STATION_TOW;
    asteroid_presentation_predict_motion(
        authoritative, render_ahead, tow_driven,
        &out_presented->pos, &out_presented->vel);
    out_presented->rotation = wrap_angle(
        authoritative->rotation +
        authoritative->spin * render_ahead);
    out_presented->spin = authoritative->spin;
    out_presented->age = authoritative->age + render_ahead;
    if (!finite_vec(out_presented->pos) ||
        !finite_vec(out_presented->vel) ||
        !isfinite(out_presented->rotation) ||
        !isfinite(out_presented->spin) ||
        !isfinite(out_presented->age)) {
        return ASTEROID_PRESENTATION_SKIP;
    }
    return ASTEROID_PRESENTATION_PRESENT;
}

void asteroid_presentation_diagnostics_reset(
    asteroid_presentation_diagnostics_t *diagnostics)
{
    if (diagnostics) memset(diagnostics, 0, sizeof(*diagnostics));
}

void asteroid_presentation_diagnostics_begin_frame(
    asteroid_presentation_diagnostics_t *diagnostics)
{
    if (diagnostics) diagnostics->frame_samples++;
}

static asteroid_presentation_slot_track_t *diagnostic_track(
    asteroid_presentation_diagnostics_t *diagnostics,
    int asteroid_index, asteroid_motion_class_t motion_class)
{
    if (!diagnostics || asteroid_index < 0 ||
        asteroid_index >= MAX_ASTEROIDS || motion_class < 0 ||
        motion_class >= ASTEROID_MOTION_CLASS_COUNT) {
        return NULL;
    }
    asteroid_presentation_slot_track_t *track =
        &diagnostics->slots[asteroid_index];
    if (track->motion_class_valid &&
        track->motion_class != motion_class) {
        tow_presentation_diagnostics_reset(&track->kinematics);
        diagnostics->class_transitions++;
    }
    track->motion_class = motion_class;
    track->motion_class_valid = true;
    return track;
}

void asteroid_presentation_diagnostics_present(
    asteroid_presentation_diagnostics_t *diagnostics,
    int asteroid_index, asteroid_motion_class_t motion_class,
    const asteroid_t *legacy_presented,
    const asteroid_t *final_presented,
    const asteroid_t *authoritative_target,
    float frame_dt, float pixels_per_world)
{
    if (!diagnostics || !legacy_presented || !final_presented ||
        !authoritative_target) return;
    asteroid_presentation_slot_track_t *track = diagnostic_track(
        diagnostics, asteroid_index, motion_class);
    if (!track) return;

    float correction = vec_length(v2_sub(
        authoritative_target->pos, final_presented->pos));
    float velocity_discontinuity = vec_length(v2_sub(
        authoritative_target->vel, final_presented->vel));
    float avoided = vec_length(v2_sub(
        authoritative_target->pos, legacy_presented->pos));
    if (diagnostics->max_correction_world < correction)
        diagnostics->max_correction_world = correction;
    if (diagnostics->max_velocity_discontinuity <
        velocity_discontinuity) {
        diagnostics->max_velocity_discontinuity =
            velocity_discontinuity;
    }
    if (diagnostics->max_legacy_correction_avoided < avoided)
        diagnostics->max_legacy_correction_avoided = avoided;

    /* Jerk belongs to the corrective displacement, not the asteroid's
     * authored physical acceleration. A collision can legitimately have
     * enormous world-space jerk; a render correction should not. */
    vec2 correction_error = v2_sub(
        final_presented->pos, authoritative_target->pos);
    vec2 velocity_error = v2_sub(
        final_presented->vel, authoritative_target->vel);
    tow_presentation_diagnostics_snapshot(
        &track->kinematics,
        correction_error, velocity_error,
        v2(0.0f, 0.0f), v2(0.0f, 0.0f));
    tow_presentation_diagnostics_frame(
        &track->kinematics, true, correction_error,
        frame_dt, pixels_per_world);
    if (diagnostics->max_screen_jerk <
        track->kinematics.max_screen_jerk) {
        diagnostics->max_screen_jerk =
            track->kinematics.max_screen_jerk;
    }
    diagnostics->presented_samples++;
    diagnostics->class_samples[motion_class]++;
}

void asteroid_presentation_diagnostics_skip(
    asteroid_presentation_diagnostics_t *diagnostics,
    int asteroid_index, asteroid_motion_class_t motion_class,
    const asteroid_t *legacy_presented,
    float frame_dt, float pixels_per_world)
{
    if (!diagnostics || !legacy_presented) return;
    asteroid_presentation_slot_track_t *track = diagnostic_track(
        diagnostics, asteroid_index, motion_class);
    if (!track) return;
    uint32_t starvation_before =
        track->kinematics.starvation_events;
    vec2 held_error = track->kinematics.have_position
        ? track->kinematics.last_position : v2(0.0f, 0.0f);
    tow_presentation_diagnostics_frame(
        &track->kinematics, legacy_presented->active, held_error,
        frame_dt, pixels_per_world);
    diagnostics->starvation_events +=
        track->kinematics.starvation_events - starvation_before;
    diagnostics->skipped_samples++;
}

void asteroid_presentation_diagnostics_retire(
    asteroid_presentation_diagnostics_t *diagnostics,
    int asteroid_index)
{
    if (!diagnostics || asteroid_index < 0 ||
        asteroid_index >= MAX_ASTEROIDS) return;
    asteroid_presentation_slot_track_t *track =
        &diagnostics->slots[asteroid_index];
    tow_presentation_diagnostics_reset(&track->kinematics);
    track->motion_class_valid = false;
    diagnostics->retired_samples++;
}

bool asteroid_presentation_diagnostics_within_thresholds(
    const asteroid_presentation_diagnostics_t *diagnostics)
{
    return diagnostics && diagnostics->frame_samples > 0 &&
        diagnostics->presented_samples > 0 &&
        diagnostics->starvation_events == 0 &&
        isfinite(diagnostics->max_correction_world) &&
        isfinite(diagnostics->max_velocity_discontinuity) &&
        isfinite(diagnostics->max_screen_jerk) &&
        diagnostics->max_correction_world <=
            ASTEROID_PRESENTATION_MAX_CORRECTION_WORLD &&
        diagnostics->max_velocity_discontinuity <=
            ASTEROID_PRESENTATION_MAX_VELOCITY_DISCONTINUITY &&
        diagnostics->max_screen_jerk <=
            ASTEROID_PRESENTATION_MAX_SCREEN_JERK;
}
