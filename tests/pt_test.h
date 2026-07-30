/* Minimal test harness shared by every suite. */
#ifndef PT_TEST_H
#define PT_TEST_H

#include <stdio.h>

extern int pt_test_failures;
extern int pt_test_checks;

#define PT_CHECK(cond, ...)                                                   \
    do {                                                                      \
        ++pt_test_checks;                                                     \
        if (!(cond)) {                                                        \
            ++pt_test_failures;                                               \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);              \
            fprintf(stderr, __VA_ARGS__);                                     \
            fprintf(stderr, "\n");                                            \
        }                                                                     \
    } while (0)

#define PT_CHECK_EQ_INT(actual, expected)                                     \
    PT_CHECK((long)(actual) == (long)(expected),                              \
             #actual " == %ld, want %ld", (long)(actual), (long)(expected))

void pt_test_board(void);
void pt_test_units(void);
void pt_test_combat(void);
void pt_test_fixture(void);
void pt_test_economy(void);
void pt_test_simulate(void);
void pt_test_hud(void);

#endif
