#include <stdio.h>
#include "app/path/path.h"
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
    struct path_line_params p;
    struct path_status st;
    i32 last_x;
    int i;

    p.x0 = 0;
    p.z0 = 0;
    p.x1 = FXONE * 3 / 10;   /* 0.30 m */
    p.z1 = 0;
    p.speed = FXONE / 10;    /* 0.10 m/s */
    p.tol = FXONE / 100;
    p.timeout = 1000;

    check(path_line_start(&p) == 1, "line accepted");
    path_status_get(&st);
    check(st.state == PATH_STATE_RUN && st.s == 0 && st.x == 0 && st.z == 0,
          "line starts at p0");

    last_x = 0;
    for (i = 0; i < 400 && st.state == PATH_STATE_RUN; i++) {
        path_step();
        path_status_get(&st);
        if (st.x < last_x) {
            check(0, "x must not go backwards on a forward line");
            break;
        }
        last_x = st.x;
    }

    check(st.state == PATH_STATE_DONE, "line stops after finite distance");
    check(st.s == st.length, "s clamps to length");
    check(st.t == FXONE, "t reaches 1");
    check(st.x == p.x1 && st.z == p.z1, "endpoint reached exactly");
    check(st.dx == FXONE && st.dz == 0, "tangent is +x");

    path_step();
    path_status_get(&st);
    check(st.state == PATH_STATE_DONE && st.s == st.length,
          "done line does not advance further");

    p.x1 = p.x0;
    p.z1 = p.z0;
    check(path_line_start(&p) == 0, "degenerate line rejected");
    path_status_get(&st);
    check(st.state == PATH_STATE_FAULT && st.fault == PATH_FAULT_GEOM,
          "degenerate line reports geometry fault");

    p.x1 = FXONE;
    p.z1 = 0;
    p.speed = PATH_MAX_SPEED + 1;
    p.timeout = 1000;
    check(path_line_start(&p) == 0, "speed above limit rejected");
    path_status_get(&st);
    check(st.state == PATH_STATE_FAULT && st.fault == PATH_FAULT_SPEED,
          "speed fault reported");

    p.speed = FXONE / 100;   /* 0.01 m/s */
    p.timeout = 5;
    check(path_line_start(&p) == 1, "slow line accepted");
    for (i = 0; i < 6; i++) path_step();
    path_status_get(&st);
    check(st.state == PATH_STATE_FAULT && st.fault == PATH_FAULT_TIMEOUT,
          "timeout fault reported");

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all line path tests passed\n");
    return 0;
}
