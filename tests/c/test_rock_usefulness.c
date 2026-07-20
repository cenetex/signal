/* Unit coverage for the one-reason collectible-rock HUD grammar. */
#include "test_harness.h"
#include "rock_usefulness.h"

TEST(test_rock_usefulness_precedence_is_contract_demand_route_smelt_grade) {
    rock_usefulness_candidate_t best = {0};
    const rock_usefulness_candidate_t rare = {
        .kind = ROCK_USEFULNESS_RARE_GRADE,
        .strength = 3,
    };
    const rock_usefulness_candidate_t smelt = {
        .kind = ROCK_USEFULNESS_SMELT_PATH,
        .station_a = 0,
    };
    const rock_usefulness_candidate_t route = {
        .kind = ROCK_USEFULNESS_REMEMBERED_ROUTE,
        .station_a = 1,
        .station_b = 0,
        .confidence = 210,
        .salience = 190,
    };
    const rock_usefulness_candidate_t demand = {
        .kind = ROCK_USEFULNESS_DIRECT_DEMAND,
        .station_a = 2,
        .strength = 700,
    };
    const rock_usefulness_candidate_t contract = {
        .kind = ROCK_USEFULNESS_TRACKED_CONTRACT,
        .station_a = 1,
    };

    rock_usefulness_select(&best, &rare);
    ASSERT_EQ_INT(best.kind, ROCK_USEFULNESS_RARE_GRADE);
    rock_usefulness_select(&best, &smelt);
    ASSERT_EQ_INT(best.kind, ROCK_USEFULNESS_SMELT_PATH);
    rock_usefulness_select(&best, &route);
    ASSERT_EQ_INT(best.kind, ROCK_USEFULNESS_REMEMBERED_ROUTE);
    rock_usefulness_select(&best, &demand);
    ASSERT_EQ_INT(best.kind, ROCK_USEFULNESS_DIRECT_DEMAND);
    rock_usefulness_select(&best, &contract);
    ASSERT_EQ_INT(best.kind, ROCK_USEFULNESS_TRACKED_CONTRACT);

    /* Lower reasons cannot displace the selected strongest reason. */
    rock_usefulness_select(&best, &route);
    ASSERT_EQ_INT(best.kind, ROCK_USEFULNESS_TRACKED_CONTRACT);
}

TEST(test_rock_usefulness_prefers_fresh_firsthand_route_memory) {
    const rock_usefulness_candidate_t stale_relay = {
        .kind = ROCK_USEFULNESS_REMEMBERED_ROUTE,
        .station_a = 2,
        .station_b = 1,
        .confidence = 120,
        .salience = 105,
        .hops = 3,
    };
    const rock_usefulness_candidate_t fresh_firsthand = {
        .kind = ROCK_USEFULNESS_REMEMBERED_ROUTE,
        .station_a = 1,
        .station_b = 0,
        .confidence = 235,
        .salience = 220,
        .hops = 0,
    };
    rock_usefulness_candidate_t best = stale_relay;

    ASSERT(rock_usefulness_evidence_score(&fresh_firsthand) >
           rock_usefulness_evidence_score(&stale_relay));
    rock_usefulness_select(&best, &fresh_firsthand);
    ASSERT_EQ_INT(best.station_a, 1);
    ASSERT_EQ_INT(best.station_b, 0);
}

void register_rock_usefulness_tests(void) {
    TEST_SECTION("\nRock usefulness:\n");
    RUN(test_rock_usefulness_precedence_is_contract_demand_route_smelt_grade);
    RUN(test_rock_usefulness_prefers_fresh_firsthand_route_memory);
}
