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
    i32 kappa;
    struct line2 line;
    struct arc2 arc;
} path;

static void path_reset(int kind)
{
    path.kind = kind;
    path.state = PATH_STATE_IDLE;
    path.fault = PATH_FAULT_NONE;
    path.tick = 0;
    path.s = 0;
    path.length = 0;
    path.speed = 0;
    path.tol = 0;
    path.timeout = PATH_DEFAULT_TIMEOUT;
    path.dx = 0;
    path.dz = 0;
    path.kappa = 0;
    path.line.x0 = 0;
    path.line.z0 = 0;
    path.line.x1 = 0;
    path.line.z1 = 0;
    path.arc.cx = 0;
    path.arc.cz = 0;
    path.arc.r = 0;
    path.arc.a0 = 0;
    path.arc.sweep = 0;
}

static void path_fault(int fault)
{
    path.state = PATH_STATE_FAULT;
    path.fault = fault;
}

static int path_speed_ok(i32 speed)
{
    return speed >= 0 && speed <= PATH_MAX_SPEED;
}

int path_line_start(const struct path_line_params *params)
{
    i32 length, dx, dz;

    path_reset(PATH_KIND_LINE);
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
    if (!path_speed_ok(params->speed)) {
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

int path_arc_start(const struct path_arc_params *params)
{
    i32 length;

    path_reset(PATH_KIND_ARC);
    if (!params) {
        path_fault(PATH_FAULT_GEOM);
        return 0;
    }

    path.arc.cx = params->cx;
    path.arc.cz = params->cz;
    path.arc.r = params->r;
    path.arc.a0 = params->a0;
    path.arc.sweep = params->sweep;

    length = arc2_length(&path.arc);
    if (params->r <= 0 || params->sweep == 0 || length <= 0) {
        path_fault(PATH_FAULT_GEOM);
        return 0;
    }
    if (!path_speed_ok(params->speed)) {
        path_fault(PATH_FAULT_SPEED);
        return 0;
    }

    path.length = length;
    path.speed = params->speed;
    path.tol = params->tol > 0 ? params->tol : 0;
    path.timeout = params->timeout > 0 ? params->timeout : PATH_DEFAULT_TIMEOUT;
    path.kappa = arc2_curvature(&path.arc);
    path.state = PATH_STATE_RUN;
    return 1;
}

int path_start(int kind, const i32 *values, int count)
{
    if (!values || count < 7) {
        path_reset(kind);
        path_fault(PATH_FAULT_GEOM);
        return 0;
    }
    if (kind == PATH_KIND_LINE) {
        struct path_line_params p;
        p.x0 = values[0];
        p.z0 = values[1];
        p.x1 = values[2];
        p.z1 = values[3];
        p.speed = values[4];
        p.tol = values[5];
        p.timeout = values[6];
        return path_line_start(&p);
    }
    if (kind == PATH_KIND_ARC && count >= 8) {
        struct path_arc_params p;
        p.cx = values[0];
        p.cz = values[1];
        p.r = values[2];
        p.a0 = values[3];
        p.sweep = values[4];
        p.speed = values[5];
        p.tol = values[6];
        p.timeout = values[7];
        return path_arc_start(&p);
    }
    path_reset(kind);
    path_fault(PATH_FAULT_GEOM);
    return 0;
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
    i32 dx = path.dx;
    i32 dz = path.dz;
    i32 t;

    if (!out) return;

    if (path.length > 0) {
        t = fx_div(path.s, path.length);
        if (t < 0) t = 0;
        if (t > FXONE) t = FXONE;
    } else {
        t = 0;
    }

    if (path.kind == PATH_KIND_ARC) {
        arc2_point(&path.arc, t, &x, &z);
        arc2_tangent(&path.arc, t, &dx, &dz);
    } else if (path.length > 0) {
        line2_point(&path.line, t, &x, &z);
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
    out->dx = dx;
    out->dz = dz;
    out->kappa = path.kappa;
    out->speed = path.speed;
    out->tol = path.tol;
}
