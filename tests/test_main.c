#include "pt_test.h"

int pt_test_failures;
int pt_test_checks;

int main(void)
{
    pt_test_board();
    pt_test_maps();
    pt_test_units();
    pt_test_combat();
    pt_test_fixture();
    pt_test_economy();
    pt_test_simulate();
    pt_test_feedback();
    pt_test_hud();

    if (pt_test_failures) {
        fprintf(stderr, "FAILED %d of %d checks\n", pt_test_failures,
                pt_test_checks);
        return 1;
    }
    printf("PASS %d checks\n", pt_test_checks);
    return 0;
}
