#ifndef SIGNAL_SHARED_FACTION_H
#define SIGNAL_SHARED_FACTION_H

#include <stdint.h>
#include "types.h"

enum {
    STATION_FACTION_REL_WAR = -75,
    STATION_FACTION_REL_HOSTILE = -45,
    STATION_FACTION_REL_STRAINED = -15,
    STATION_FACTION_REL_PACT = 35,
    STATION_FACTION_REL_ALLIED = 70,
};

static inline const char *station_faction_name(uint8_t faction)
{
    switch ((station_faction_id_t)faction) {
        case STATION_FACTION_PROSPECTOR_GUILD:       return "Prospector Guild";
        case STATION_FACTION_KEPLER_COMPACT:         return "Kepler Compact";
        case STATION_FACTION_HELIOS_CONSORTIUM:      return "Helios Consortium";
        case STATION_FACTION_BLACKGLASS_SYNDICATE:   return "Blackglass Syndicate";
        case STATION_FACTION_UNALIGNED:
        default:                                     return "Unaligned";
    }
}

static inline const char *station_ideology_name(uint8_t ideology)
{
    switch ((station_ideology_t)ideology) {
        case STATION_IDEOLOGY_COOPERATIVE:  return "cooperative";
        case STATION_IDEOLOGY_INDUSTRIAL:   return "industrial";
        case STATION_IDEOLOGY_EXPANSIONIST: return "expansionist";
        case STATION_IDEOLOGY_OPPORTUNIST:  return "opportunist";
        case STATION_IDEOLOGY_PRAGMATIC:
        default:                            return "pragmatic";
    }
}

static inline const char *station_faction_relation_label(int relation)
{
    if (relation <= STATION_FACTION_REL_WAR) return "war";
    if (relation <= STATION_FACTION_REL_HOSTILE) return "hostile";
    if (relation <= STATION_FACTION_REL_STRAINED) return "strained";
    if (relation >= STATION_FACTION_REL_ALLIED) return "allied";
    if (relation >= STATION_FACTION_REL_PACT) return "pact";
    return "neutral";
}

static inline uint8_t station_faction_default_for_station(int station_idx)
{
    switch (station_idx) {
        case 0: return (uint8_t)STATION_FACTION_PROSPECTOR_GUILD;
        case 1: return (uint8_t)STATION_FACTION_KEPLER_COMPACT;
        case 2: return (uint8_t)STATION_FACTION_HELIOS_CONSORTIUM;
        case SIGNAL_FREEPORT_STATION_INDEX:
            return (uint8_t)STATION_FACTION_BLACKGLASS_SYNDICATE;
        default:
            return (uint8_t)STATION_FACTION_UNALIGNED;
    }
}

static inline uint8_t station_ideology_default_for_station(int station_idx)
{
    switch (station_idx) {
        case 0: return (uint8_t)STATION_IDEOLOGY_COOPERATIVE;
        case 1: return (uint8_t)STATION_IDEOLOGY_INDUSTRIAL;
        case 2: return (uint8_t)STATION_IDEOLOGY_EXPANSIONIST;
        case SIGNAL_FREEPORT_STATION_INDEX:
            return (uint8_t)STATION_IDEOLOGY_OPPORTUNIST;
        default:
            return (uint8_t)STATION_IDEOLOGY_PRAGMATIC;
    }
}

static inline int station_faction_default_relation(uint8_t from, uint8_t to)
{
    if (from >= (uint8_t)STATION_FACTION_COUNT ||
        to >= (uint8_t)STATION_FACTION_COUNT) {
        return 0;
    }
    if (from == to && from != (uint8_t)STATION_FACTION_UNALIGNED)
        return 100;

    switch ((station_faction_id_t)from) {
        case STATION_FACTION_PROSPECTOR_GUILD:
            if (to == (uint8_t)STATION_FACTION_KEPLER_COMPACT) return 62;
            if (to == (uint8_t)STATION_FACTION_HELIOS_CONSORTIUM) return 38;
            if (to == (uint8_t)STATION_FACTION_BLACKGLASS_SYNDICATE) return -68;
            break;
        case STATION_FACTION_KEPLER_COMPACT:
            if (to == (uint8_t)STATION_FACTION_PROSPECTOR_GUILD) return 62;
            if (to == (uint8_t)STATION_FACTION_HELIOS_CONSORTIUM) return 54;
            if (to == (uint8_t)STATION_FACTION_BLACKGLASS_SYNDICATE) return -58;
            break;
        case STATION_FACTION_HELIOS_CONSORTIUM:
            if (to == (uint8_t)STATION_FACTION_PROSPECTOR_GUILD) return 34;
            if (to == (uint8_t)STATION_FACTION_KEPLER_COMPACT) return 50;
            if (to == (uint8_t)STATION_FACTION_BLACKGLASS_SYNDICATE) return -82;
            break;
        case STATION_FACTION_BLACKGLASS_SYNDICATE:
            if (to == (uint8_t)STATION_FACTION_HELIOS_CONSORTIUM) return -52;
            if (to == (uint8_t)STATION_FACTION_PROSPECTOR_GUILD) return -34;
            if (to == (uint8_t)STATION_FACTION_KEPLER_COMPACT) return -26;
            break;
        case STATION_FACTION_UNALIGNED:
        default:
            break;
    }
    return 0;
}

static inline void station_faction_seed_station(station_t *st, int station_idx)
{
    if (!st) return;
    uint8_t faction = station_faction_default_for_station(station_idx);
    st->faction_id = faction;
    st->faction_allegiance = faction;
    st->faction_ideology = station_ideology_default_for_station(station_idx);
    for (int i = 0; i < STATION_FACTION_COUNT; i++) {
        st->faction_relations[i] =
            (int8_t)station_faction_default_relation(faction, (uint8_t)i);
    }
}

static inline int station_faction_relation_to(const station_t *st,
                                              uint8_t other_faction)
{
    if (!st || other_faction >= (uint8_t)STATION_FACTION_COUNT) return 0;
    if (st->faction_id >= (uint8_t)STATION_FACTION_COUNT) return 0;
    return st->faction_relations[other_faction];
}

static inline int station_faction_adjust_relation_to(station_t *st,
                                                     uint8_t other_faction,
                                                     int delta)
{
    if (!st || other_faction >= (uint8_t)STATION_FACTION_COUNT) return 0;
    if (st->faction_id >= (uint8_t)STATION_FACTION_COUNT) return 0;
    if (st->faction_id == other_faction &&
        st->faction_id != (uint8_t)STATION_FACTION_UNALIGNED) {
        st->faction_relations[other_faction] = 100;
        return 100;
    }
    int relation = st->faction_relations[other_faction] + delta;
    if (relation > 100) relation = 100;
    if (relation < -100) relation = -100;
    st->faction_relations[other_faction] = (int8_t)relation;
    return relation;
}

static inline int station_faction_relation_between(const station_t *from,
                                                   const station_t *to)
{
    if (!from || !to) return 0;
    if (from->faction_id == to->faction_id &&
        from->faction_id != (uint8_t)STATION_FACTION_UNALIGNED) {
        return 100;
    }
    return station_faction_relation_to(from, to->faction_id);
}

static inline bool station_faction_is_hostile_to(const station_t *from,
                                                 const station_t *to)
{
    return station_faction_relation_between(from, to) <=
           STATION_FACTION_REL_HOSTILE;
}

static inline bool station_faction_at_war_with(const station_t *from,
                                               const station_t *to)
{
    return station_faction_relation_between(from, to) <=
           STATION_FACTION_REL_WAR;
}

static inline bool station_faction_is_pirate_economy(const station_t *st)
{
    if (!st) return false;
    return st->faction_id == (uint8_t)STATION_FACTION_BLACKGLASS_SYNDICATE ||
           st->faction_ideology == (uint8_t)STATION_IDEOLOGY_OPPORTUNIST;
}

#endif /* SIGNAL_SHARED_FACTION_H */
