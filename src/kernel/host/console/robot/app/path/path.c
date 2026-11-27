#include "app/path/path.h"
#include "lib/fixed.h"

static struct {
    int state;
    int fault;
    int kind;
    i32 tick;
    i32 s;
    i32 length;
    i32 speed;
    i32 tol;
    i32 timeout;
    i32 dx;
    i32 dz;
    struct line2 line;
} path;

static void path_fault(int fault)
{
    path.state = PATH_STATE_FAULT;
    path.fault = fault;
}

int path_line_start(const struct path_line_params *params)
{
    i32 length, dx, dz;

    path.kind = PATH_KIND_LINE;
    path.state = PATH_STATE_IDLE;
    path.fault = PATH_FAULT_NONE;
    path.tick = 0;
    path.s = 0;
    path.length = 0;
    path.speed = 0;
    path.tol = 0;
    path.dx = 0;
    path.dz = 0;

    if (!params) {
        path_fault(PATH_FAULT_GEOM);
        return 0;
    }

    path.line.x0 = params->x0;
    path.line.z0 = params->z0;
    path.line.x1 = params->x1;
    path.line.z1 = params->z1;

    length = line2_length(&path.line);
    dx = params->x1 - params->x0;
    dz = params->z1 - params->z0;

    if (length <= 0 || (dx == 0 && dz == 0)) {
        path_fault(PATH_FAULT_GEOM);
        return 0;
    }
    if (params->speed < 0 || params->speed > PATH_MAX_SPEED) {
        path_fault(PATH_FAULT_SPEED);
        return 0;
    }

    path.length = length;
    path.speed = params->speed;
    path.tol = params->tol > 0 ? params->tol : 0;
    path.timeout = params->timeout > 0 ? params->timeout : PATH_DEFAULT_TIMEOUT;
    path.dx = fx_div(dx, length);
    path.dz = fx_div(dz, length);
    path.state = PATH_STATE_RUN;
    return 1;
}

void path_step(void)
{
    i32 step;

    if (path.state != PATH_STATE_RUN) return;

    path.tick++;
    if (path.tick >= path.timeout) {
        path_fault(PATH_FAULT_TIMEOUT);
        return;
    }

    if (path.speed == 0) return;

    step = fx_mul(path.speed, PATH_DT);
    if (step <= 0) step = 1;
    path.s += step;
    if (path.s >= path.length) {
        path.s = path.length;
        path.state = PATH_STATE_DONE;
    }
}

int path_has_status(void)
{
    return path.state != PATH_STATE_IDLE;
}

void path_status_get(struct path_status *out)
{
    i32 x = path.line.x0;
    i32 z = path.line.z0;
    i32 t;

    if (!out) return;

    if (path.length > 0) {
        t = fx_div(path.s, path.length);
        if (t < 0) t = 0;
        if (t > FXONE) t = FXONE;
        line2_point(&path.line, t, &x, &z);
    } else {
        t = 0;
    }

    out->kind = path.kind;
    out->state = path.state;
    out->fault = path.fault;
    out->tick = path.tick;
    out->s = path.s;
    out->t = t;
    out->length = path.length;
    out->x = x;
    out->z = z;
    out->dx = path.dx;
    out->dz = path.dz;
    out->speed = path.speed;
    out->tol = path.tol;
}
