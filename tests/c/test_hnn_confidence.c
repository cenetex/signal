#include "test_harness.h"

#include "holographic_nn_confidence.h"

static hnn_memory_contract_t hnn_confidence_test_contract(void) {
    hnn_memory_t memory;
    hnn_memory_init(&memory);
    memory.experience_count = 16;
    return hnn_memory_contract(&memory);
}

TEST(test_hnn_confidence_rejects_each_unsafe_condition) {
    static const hnn_backend_kind_t backends[] = {
        HNN_BACKEND_BUILTIN_RADIX2,
        HNN_BACKEND_LECORE_DIRECT,
        HNN_BACKEND_LECORE_RADIX2,
    };
    for (size_t i = 0; i < sizeof(backends) / sizeof(backends[0]); i++) {
        hnn_memory_contract_t contract = hnn_confidence_test_contract();
        hnn_confidence_decision_t result = hnn_confidence_evaluate(
            backends[i], &contract, 0.90f, 0.50f, true, true);
        ASSERT(result.accepted);
        ASSERT_EQ_INT(result.reason, HNN_CONFIDENCE_ACCEPTED);

        contract.stored_count = 0;
        result = hnn_confidence_evaluate(
            backends[i], &contract, 0.90f, 0.50f, true, true);
        ASSERT(!result.accepted);
        ASSERT_EQ_INT(result.reason, HNN_CONFIDENCE_REJECT_EMPTY);

        contract = hnn_confidence_test_contract();
        contract.encoder_version++;
        result = hnn_confidence_evaluate(
            backends[i], &contract, 0.90f, 0.50f, true, true);
        ASSERT_EQ_INT(result.reason, HNN_CONFIDENCE_REJECT_CONTRACT);

        contract = hnn_confidence_test_contract();
        contract.stored_count = HNN_TRACE_CAPACITY + 1;
        contract.capacity_load = 1.01f;
        result = hnn_confidence_evaluate(
            backends[i], &contract, 0.90f, 0.50f, true, true);
        ASSERT_EQ_INT(result.reason, HNN_CONFIDENCE_REJECT_CAPACITY);

        contract = hnn_confidence_test_contract();
        result = hnn_confidence_evaluate(
            backends[i], &contract, NAN, 0.50f, true, true);
        ASSERT_EQ_INT(result.reason, HNN_CONFIDENCE_REJECT_NONFINITE);

        result = hnn_confidence_evaluate(
            backends[i], &contract, 0.10f, 0.50f, true, true);
        ASSERT_EQ_INT(result.reason, HNN_CONFIDENCE_REJECT_SCORE);

        result = hnn_confidence_evaluate(
            backends[i], &contract, 0.90f, 0.01f, true, true);
        ASSERT_EQ_INT(result.reason, HNN_CONFIDENCE_REJECT_MARGIN);

        result = hnn_confidence_evaluate(
            backends[i], &contract, 0.90f, 0.50f, false, true);
        ASSERT_EQ_INT(result.reason, HNN_CONFIDENCE_REJECT_ILLEGAL);

        result = hnn_confidence_evaluate(
            backends[i], &contract, 0.90f, 0.50f, true, false);
        ASSERT_EQ_INT(result.reason, HNN_CONFIDENCE_REJECT_UNSAFE);
    }
}

TEST(test_hnn_confidence_shadow_and_rejection_always_choose_teacher) {
    hnn_memory_contract_t contract = hnn_confidence_test_contract();
    hnn_confidence_decision_t accepted = hnn_confidence_evaluate(
        HNN_BACKEND_BUILTIN_RADIX2, &contract, 0.90f, 0.50f, true, true);
    hnn_confidence_decision_t rejected = hnn_confidence_evaluate(
        HNN_BACKEND_BUILTIN_RADIX2, &contract, 0.10f, 0.01f, true, true);

    ASSERT_EQ_INT(hnn_confidence_select_action(
                      HNN_CONFIDENCE_MODE_SHADOW, &accepted, 6, 5),
                  5);
    ASSERT_EQ_INT(hnn_confidence_select_action(
                      HNN_CONFIDENCE_MODE_MIXED, &rejected, 6, 5),
                  5);
    ASSERT_EQ_INT(hnn_confidence_select_action(
                      HNN_CONFIDENCE_MODE_MIXED, &accepted, 6, 5),
                  6);
}

TEST(test_hnn_confidence_mode_is_fixed_to_safe_default) {
    ASSERT_EQ_INT(hnn_confidence_mode_from_string(NULL),
                  HNN_CONFIDENCE_MODE_SHADOW);
    ASSERT_EQ_INT(hnn_confidence_mode_from_string(""),
                  HNN_CONFIDENCE_MODE_SHADOW);
    ASSERT_EQ_INT(hnn_confidence_mode_from_string("active"),
                  HNN_CONFIDENCE_MODE_SHADOW);
    ASSERT_EQ_INT(hnn_confidence_mode_from_string("mixed"),
                  HNN_CONFIDENCE_MODE_MIXED);
    ASSERT(strcmp(hnn_confidence_mode_name(HNN_CONFIDENCE_MODE_SHADOW),
                  "shadow") == 0);
}

void register_hnn_confidence_tests(void);
void register_hnn_confidence_tests(void) {
    TEST_SECTION("\nHNN confidence gate:\n");
    RUN(test_hnn_confidence_rejects_each_unsafe_condition);
    RUN(test_hnn_confidence_shadow_and_rejection_always_choose_teacher);
    RUN(test_hnn_confidence_mode_is_fixed_to_safe_default);
}
