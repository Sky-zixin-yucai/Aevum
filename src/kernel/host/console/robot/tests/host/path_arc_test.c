#include <stdio.h>
#include "app/path/path.h"
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
    struct path_arc_params p;
    struct path_status st;
    i32 v[8];
    int i;

    p.cx = 0;
    p.cz = 0;
    p.r = FXONE;
    p.a0 = 0;
    p.sweep = fx_rad(DEG90);
    p.speed = 6554;   /* 0.1 m/s */
    p.tol = 655;
    p.timeout = 0;

    check(path_arc_start(&p) == 1, "quarter arc accepted");
    check(path_has_status() == 1, "arc reports status");
    path_status_get(&st);
    check(st.kind == PATH_KIND_ARC, "kind is ARC");
    check(st.state == PATH_STATE_RUN, "arc starts running");
    check(st.length == 102943, "arc length is pi/2 m");
    check(near(st.x, FXONE, 2) && near(st.z, 0, 2), "arc starts at (1, 0)");
    check(near(st.dx, 0, 2) && near(st.dz, FXONE, 2), "arc start tangent is +z");
    check(st.kappa == -FXONE, "arc curvature is -1/r");

    for (i = 0; i < 5000 && st.state == PATH_STATE_RUN; i++) {
        path_step();
        path_status_get(&st);
    }
    check(st.state == PATH_STATE_DONE, "arc reaches DONE");
    check(st.s == st.length, "arc s clamps to length");
    check(st.t == FXONE, "arc t reaches 1");
    check(near(st.x, 0, 2) && near(st.z, FXONE, 2), "arc endpoint is (0, 1)");
    check(near(st.dx, -FXONE, 2) && near(st.dz, 0, 2), "arc end tangent is -x");

    p.r = 0;
    check(path_arc_start(&p) == 0, "zero radius rejected");
    path_status_get(&st);
    check(st.state == PATH_STATE_FAULT && st.fault == PATH_FAULT_GEOM,
          "zero radius reports geometry fault");

    p.r = FXONE;
    p.sweep = 0;
    check(path_arc_start(&p) == 0, "zero sweep rejected");

    p.sweep = fx_rad(-DEG90);
    p.speed = PATH_MAX_SPEED + 1;
    check(path_arc_start(&p) == 0, "overspeed rejected");
    path_status_get(&st);
    check(st.fault == PATH_FAULT_SPEED, "overspeed reports speed fault");

    p.speed = 6554;
    p.timeout = 1;
    check(path_arc_start(&p) == 1, "tiny timeout accepted");
    path_step();
    path_status_get(&st);
    check(st.state == PATH_STATE_FAULT && st.fault == PATH_FAULT_TIMEOUT,
          "timeout reports timeout fault");

    v[0] = 0; v[1] = 0; v[2] = FXONE; v[3] = 0;
    v[4] = fx_rad(-DEG90); v[5] = 6554; v[6] = 655; v[7] = 0;
    check(path_start(PATH_KIND_ARC, v, 8) == 1, "generic ARC entry accepts raw fields");
    path_status_get(&st);
    check(st.kappa == FXONE, "clockwise arc curvature is +1/r");

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all arc path tests passed\n");
    return 0;
}
