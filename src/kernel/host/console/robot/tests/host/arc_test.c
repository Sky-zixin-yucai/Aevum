#include <stdio.h>
#include "lib/geom/arc.h"
#include "lib/fixed.h"
#include "lib/trig.h"

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

static int near(i32 a, i32 b, i32 tol)
{
    i32 d = a - b;
    if (d < 0) d = -d;
    return d <= tol;
}

int main(void)
{
    i32 quarter = fx_rad(DEG90);
    i32 full = fx_rad(DEG360);
    struct arc2 ccw = {0, 0, FXONE, 0, quarter};
    struct arc2 cw = {0, 0, FXONE, 0, -quarter};
    struct arc2 bad_r = {0, 0, 0, 0, quarter};
    struct arc2 bad_sweep = {0, 0, FXONE, 0, 0};
    i32 x = 0, z = 0, dx = 0, dz = 0;

    check(near(arc2_length(&ccw), 102943, 3), "quarter circle length = pi/2 m");
    check(arc2_point(&ccw, FXONE / 2, &x, &z) == 1 &&
          near(x, 46341, 3) && near(z, 46341, 3),
          "quarter midpoint = (sqrt2/2, sqrt2/2)");
    check(arc2_point(&ccw, FXONE, &x, &z) == 1 &&
          near(x, 0, 2) && near(z, FXONE, 2),
          "quarter endpoint = (0, 1)");

    check(arc2_tangent(&ccw, 0, &dx, &dz) == 1 &&
          near(dx, 0, 2) && near(dz, FXONE, 2),
          "CCW start tangent = +z");
    check(arc2_tangent(&ccw, FXONE, &dx, &dz) == 1 &&
          near(dx, -FXONE, 2) && near(dz, 0, 2),
          "CCW end tangent = -x");
    check(arc2_curvature(&ccw) == -FXONE, "CCW curvature = -1/r");

    check(arc2_point(&cw, FXONE, &x, &z) == 1 &&
          near(x, 0, 2) && near(z, -FXONE, 2),
          "CW quarter endpoint = (0, -1)");
    check(arc2_tangent(&cw, 0, &dx, &dz) == 1 &&
          near(dx, 0, 2) && near(dz, -FXONE, 2),
          "CW start tangent = -z");
    check(arc2_curvature(&cw) == FXONE, "CW curvature = +1/r");

    check(arc2_length(&bad_r) == 0, "zero radius has zero length");
    check(arc2_point(&bad_r, FXONE / 2, &x, &z) == 0, "zero radius point rejected");
    check(arc2_tangent(&bad_r, FXONE / 2, &dx, &dz) == 0, "zero radius tangent rejected");
    check(arc2_length(&bad_sweep) == 0, "zero sweep has zero length");
    check(arc2_point(&bad_sweep, FXONE / 2, &x, &z) == 0, "zero sweep point rejected");

    check(near(arc2_length(&(struct arc2){0, 0, FXONE, 0, full}), 411775, 4),
          "full circle length = 2*pi m");

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all arc geometry tests passed\n");
    return 0;
}
