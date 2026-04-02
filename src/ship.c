#include "ship.h"

const hull_def_t* ship_hull_def(const ship_t* ship) {
    return &HULL_DEFS[ship->hull_class];
}

const hull_def_t* npc_hull_def(const npc_ship_t* npc) {
    return &HULL_DEFS[npc->hull_class];
}

vec2 ship_forward(float angle) {
    return v2_from_angle(angle);
}

vec2 ship_muzzle(vec2 pos, float angle, const ship_t* ship) {
    vec2 forward = v2_from_angle(angle);
    return v2_add(pos, v2_scale(forward, ship_hull_def(ship)->ship_radius + 8.0f));
}

float ship_max_hull(const ship_t* ship) {
    return ship_hull_def(ship)->max_hull;
}

float ship_cargo_capacity(const ship_t* ship) {
    return ship_hull_def(ship)->ore_capacity + ((float)ship->hold_level * SHIP_HOLD_UPGRADE_STEP);
}

float ship_mining_rate(const ship_t* ship) {
    return ship_hull_def(ship)->mining_rate + ((float)ship->mining_level * SHIP_MINING_UPGRADE_STEP);
}

float ship_tractor_range(const ship_t* ship) {
    return ship_hull_def(ship)->tractor_range + ((float)ship->tractor_level * SHIP_TRACTOR_UPGRADE_STEP);
}

float ship_collect_radius(const ship_t* ship) {
    return SHIP_BASE_COLLECT_RADIUS + ((float)ship->tractor_level * SHIP_COLLECT_UPGRADE_STEP);
}

int ship_upgrade_level(const ship_t* ship, ship_upgrade_t upgrade) {
    switch (upgrade) {
        case SHIP_UPGRADE_MINING:
            return ship->mining_level;
        case SHIP_UPGRADE_HOLD:
            return ship->hold_level;
        case SHIP_UPGRADE_TRACTOR:
            return ship->tractor_level;
        case SHIP_UPGRADE_COUNT:
        default:
            return 0;
    }
}

bool ship_upgrade_maxed(const ship_t* ship, ship_upgrade_t upgrade) {
    return ship_upgrade_level(ship, upgrade) >= SHIP_UPGRADE_MAX_LEVEL;
}

int ship_upgrade_cost(const ship_t* ship, ship_upgrade_t upgrade) {
    int level = ship_upgrade_level(ship, upgrade);
    int tier = level + 1;
    switch (upgrade) {
        case SHIP_UPGRADE_MINING:
            return 180 + (tier * 110) + (level * level * 120);
        case SHIP_UPGRADE_HOLD:
            return 210 + (tier * 120) + (level * level * 135);
        case SHIP_UPGRADE_TRACTOR:
            return 160 + (tier * 100) + (level * level * 110);
        case SHIP_UPGRADE_COUNT:
        default:
            return 0;
    }
}

product_t upgrade_required_product(ship_upgrade_t upgrade) {
    switch (upgrade) {
        case SHIP_UPGRADE_HOLD:
            return PRODUCT_FRAME;
        case SHIP_UPGRADE_MINING:
            return PRODUCT_LASER_MODULE;
        case SHIP_UPGRADE_TRACTOR:
            return PRODUCT_TRACTOR_MODULE;
        case SHIP_UPGRADE_COUNT:
        default:
            return PRODUCT_FRAME;
    }
}

float upgrade_product_cost(const ship_t* ship, ship_upgrade_t upgrade) {
    int level = ship_upgrade_level(ship, upgrade);
    int next = level + 1;
    return UPGRADE_BASE_PRODUCT * (float)next;
}

const char* product_name(product_t product) {
    switch (product) {
        case PRODUCT_FRAME: return "Frames";
        case PRODUCT_LASER_MODULE: return "Laser Modules";
        case PRODUCT_TRACTOR_MODULE: return "Tractor Modules";
        case PRODUCT_COUNT:
        default: return "Products";
    }
}
