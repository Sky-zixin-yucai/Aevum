#include <stdio.h>
#include "lib/geom/line.h"
#include "lib/fixed.h"

static int failures;

static void check(int cond, const char *what)
{
    if (!cond) {
        printf("FAIL: %s\n", what);
        failures++;
    } else {
        printf("ok:   %s\n", what);
    }
}

int main(void)
{
    struct line2 straight = {0, 0, 0, 10 * FXONE};
    struct line2 diagonal = {0, 0, 3 * FXONE, 4 * FXONE};
    struct line2 degenerate = {7 * FXONE, 9 * FXONE, 7 * FXONE, 9 * FXONE};
    i32 x = 0, z = 0, t = 0;

    check(line2_length(&straight) == 10 * FXONE, "straight length = 10 m");
    check(line2_point(&straight, FXONE / 2, &x, &z) == 1 &&
          x == 0 && z == 5 * FXONE, "straight midpoint = (0, 5)");
    check(line2_point(&straight, -1, &x, &z) == 0, "t < 0 rejected");
    check(line2_point(&straight, FXONE + 1, &x, &z) == 0, "t > 1 rejected");

    check(line2_project(&straight, FXONE, 5 * FXONE, &t) == 1 &&
          t == FXONE / 2, "project point beside middle gives t=0.5");
    check(line2_project(&straight, 0, -FXONE, &t) == 0,
          "project point behind start is outside");
    check(t < 0, "behind start gives negative t");
    check(line2_project(&straight, 0, -1, &t) == 0 && t < 0,
          "one Q16 behind start still outside");
    check(line2_project(&straight, 0, 10 * FXONE + 1, &t) == 0 && t > FXONE,
          "one Q16 beyond end still outside");

    check(line2_length(&diagonal) == 5 * FXONE, "3-4-5 diagonal length = 5 m");
    check(line2_point(&diagonal, FXONE / 2, &x, &z) == 1 &&
          x == 3 * FXONE / 2 && z == 2 * FXONE,
          "3-4-5 midpoint = (1.5, 2)");

    check(line2_length(&degenerate) == 0, "degenerate length = 0");
    check(line2_point(&degenerate, FXONE / 2, &x, &z) == 0,
          "degenerate point rejected");
    check(line2_project(&degenerate, 0, 0, &t) == -1,
          "degenerate projection rejected");

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all line geometry tests passed\n");
    return 0;
}
