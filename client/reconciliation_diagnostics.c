#include "reconciliation_diagnostics.h"

#include <string.h>

static uint32_t float_bits(float value)
{
    uint32_t bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

net_reconcile_pose_bits_t net_reconcile_pose_bits(float pos_x,
                                                  float pos_y,
                                                  float vel_x,
                                                  float vel_y,
                                                  float angle)
{
    return (net_reconcile_pose_bits_t){
        .pos_x = float_bits(pos_x),
        .pos_y = float_bits(pos_y),
        .vel_x = float_bits(vel_x),
        .vel_y = float_bits(vel_y),
        .angle = float_bits(angle),
    };
}

bool net_reconcile_pose_equal(const net_reconcile_pose_bits_t *a,
                              const net_reconcile_pose_bits_t *b)
{
    return a && b &&
        a->pos_x == b->pos_x &&
        a->pos_y == b->pos_y &&
        a->vel_x == b->vel_x &&
        a->vel_y == b->vel_y &&
        a->angle == b->angle;
}

bool net_reconcile_movement_intent_equal(const input_intent_t *a,
                                         const input_intent_t *b)
{
    if (!a || !b) return false;
    return a->turn == b->turn &&
        a->thrust == b->thrust &&
        a->mine == b->mine &&
        (!(a->mine || b->mine) ||
         a->mining_target_hint == b->mining_target_hint) &&
        a->tractor_hold == b->tractor_hold &&
        a->boost == b->boost &&
        a->reverse_thrust == b->reverse_thrust;
}

net_reconcile_class_t net_reconcile_classify(
    const net_reconcile_sample_t *sample)
{
    if (!sample ||
        net_reconcile_pose_equal(&sample->predicted,
                                 &sample->authoritative)) {
        return NET_RECONCILE_EXACT;
    }
    if (sample->bootstrap) return NET_RECONCILE_BOOTSTRAP;
    if (sample->semantic_discontinuity) return NET_RECONCILE_SEMANTIC;
    if (sample->transport_recovery) return NET_RECONCILE_TRANSPORT_RECOVERY;
    if (sample->input_frontier ||
        sample->server_tick == 0 ||
        sample->prediction_tick == 0 ||
        sample->server_tick != sample->prediction_tick ||
        (sample->predicted_input_seq != 0 &&
         sample->authoritative_input_seq != 0 &&
         sample->predicted_input_seq !=
             sample->authoritative_input_seq)) {
        return NET_RECONCILE_INPUT_FRONTIER;
    }
    return NET_RECONCILE_NUMERIC_DRIFT;
}

void net_reconcile_diagnostics_reset(net_reconcile_diagnostics_t *diagnostics)
{
    if (diagnostics) memset(diagnostics, 0, sizeof(*diagnostics));
}

static net_reconcile_domain_t first_divergent_domain(
    const net_reconcile_pose_bits_t *predicted,
    const net_reconcile_pose_bits_t *authoritative,
    uint32_t *predicted_bits,
    uint32_t *authoritative_bits)
{
#define CHECK_DOMAIN(field, domain_value) \
    do { \
        if (predicted->field != authoritative->field) { \
            *predicted_bits = predicted->field; \
            *authoritative_bits = authoritative->field; \
            return domain_value; \
        } \
    } while (0)

    CHECK_DOMAIN(pos_x, NET_RECONCILE_DOMAIN_PLAYER_POS_X);
    CHECK_DOMAIN(pos_y, NET_RECONCILE_DOMAIN_PLAYER_POS_Y);
    CHECK_DOMAIN(vel_x, NET_RECONCILE_DOMAIN_PLAYER_VEL_X);
    CHECK_DOMAIN(vel_y, NET_RECONCILE_DOMAIN_PLAYER_VEL_Y);
    CHECK_DOMAIN(angle, NET_RECONCILE_DOMAIN_PLAYER_ANGLE);

#undef CHECK_DOMAIN
    *predicted_bits = 0;
    *authoritative_bits = 0;
    return NET_RECONCILE_DOMAIN_NONE;
}

net_reconcile_class_t net_reconcile_diagnostics_observe(
    net_reconcile_diagnostics_t *diagnostics,
    const net_reconcile_sample_t *sample)
{
    net_reconcile_class_t classification = net_reconcile_classify(sample);
    if (!diagnostics || !sample) return classification;

    diagnostics->total_samples++;
    diagnostics->class_count[classification]++;
    if (classification != NET_RECONCILE_EXACT)
        diagnostics->total_corrections++;

    if (classification != NET_RECONCILE_NUMERIC_DRIFT ||
        diagnostics->first_numeric_drift_valid) {
        return classification;
    }

    diagnostics->first_numeric_drift_valid = true;
    diagnostics->first_entity_id = sample->entity_id;
    diagnostics->first_server_tick = sample->server_tick;
    diagnostics->first_prediction_tick = sample->prediction_tick;
    diagnostics->first_predicted_input_seq = sample->predicted_input_seq;
    diagnostics->first_authoritative_input_seq =
        sample->authoritative_input_seq;
    diagnostics->first_predicted_pose = sample->predicted;
    diagnostics->first_authoritative_pose = sample->authoritative;
    diagnostics->first_domain = first_divergent_domain(
        &sample->predicted,
        &sample->authoritative,
        &diagnostics->first_predicted_bits,
        &diagnostics->first_authoritative_bits);
    if (sample->authoritative_root) {
        diagnostics->first_authoritative_root_valid = true;
        memcpy(diagnostics->first_authoritative_root,
               sample->authoritative_root,
               sizeof(diagnostics->first_authoritative_root));
    }
    return classification;
}

const char *net_reconcile_class_name(net_reconcile_class_t classification)
{
    switch (classification) {
    case NET_RECONCILE_EXACT: return "exact";
    case NET_RECONCILE_BOOTSTRAP: return "bootstrap";
    case NET_RECONCILE_INPUT_FRONTIER: return "input-frontier";
    case NET_RECONCILE_SEMANTIC: return "semantic-discontinuity";
    case NET_RECONCILE_TRANSPORT_RECOVERY: return "transport-recovery";
    case NET_RECONCILE_NUMERIC_DRIFT: return "numeric-drift";
    case NET_RECONCILE_CLASS_COUNT: break;
    }
    return "unknown";
}

const char *net_reconcile_domain_name(net_reconcile_domain_t domain)
{
    switch (domain) {
    case NET_RECONCILE_DOMAIN_NONE: return "none";
    case NET_RECONCILE_DOMAIN_PLAYER_POS_X: return "player.ship.pos.x";
    case NET_RECONCILE_DOMAIN_PLAYER_POS_Y: return "player.ship.pos.y";
    case NET_RECONCILE_DOMAIN_PLAYER_VEL_X: return "player.ship.vel.x";
    case NET_RECONCILE_DOMAIN_PLAYER_VEL_Y: return "player.ship.vel.y";
    case NET_RECONCILE_DOMAIN_PLAYER_ANGLE: return "player.ship.angle";
    }
    return "unknown";
}
