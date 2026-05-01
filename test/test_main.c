/*
 * test_main.c — entry point. Defines storage for the framework via
 * SP_TEST_FRAMEWORK_IMPL. All other test TUs include the header without
 * the impl macro and just see externs. Tests register themselves at
 * static-init time.
 */
#define SP_TEST_FRAMEWORK_IMPL
#include "test_framework.h"

int main(void) {
    return sp_run_all_tests();
}
