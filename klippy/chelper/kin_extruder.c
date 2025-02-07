// Extruder stepper pulse time generation
//
// Copyright (C) 2018-2019  Kevin O'Connor <kevin@koconnor.net>
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include <stddef.h> // offsetof
#include <stdlib.h> // malloc
#include <string.h> // memset
#include "compiler.h" // __visible
#include "itersolve.h" // struct stepper_kinematics
#include "scurve.h" // scurve_tn_antiderivative
#include "trapq.h" // move_get_distance

// Without pressure advance, the extruder stepper position is:
//     extruder_position(t) = nominal_position(t)
// When pressure advance is enabled, additional filament is pushed
// into the extruder during acceleration (and retracted during
// deceleration). The formula is:
//     pa_position(t) = (nominal_position(t)
//                       + pressure_advance * nominal_velocity(t))
// Which is then "smoothed" using a weighted average:
//     smooth_position(t) = (
//         definitive_integral(pa_position(x) * (smooth_time/2 - abs(t-x)) * dx,
//                             from=t-smooth_time/2, to=t+smooth_time/2)
//         / ((smooth_time/2)**2))

// Calculate the definitive integral of the motion formula:
//   position(t) = extrude_pos + s(t), with s(t) - Bezier S-Curve
static double
extruder_integrate(double extrude_pos, const struct scurve *s
                   , double start, double end)
{
    // Calculate base position and velocity with pressure advance
    double base = extrude_pos * (end - start);
    double integral = scurve_tn_antiderivative(s, 0, end)
        - scurve_tn_antiderivative(s, 0, start);
    return base + integral;
}

// Calculate the definitive integral of time weighted position:
//   weighted_position(t) = t * (extrude_pos + s(t))
static double
extruder_integrate_time(double extrude_pos, const struct scurve *s
                        , double start, double end)
{
    // Calculate base position and velocity with pressure advance
    double base = .5 * extrude_pos * (end * end - start * start);
    double integral = scurve_tn_antiderivative(s, 1, end)
        - scurve_tn_antiderivative(s, 1, start);
    return base + integral;
}

// Calculate the definitive integral of extruder for a given move
static double
pa_move_integrate(struct move *m, double pressure_advance
                  , double base, double start, double end, double time_offset)
{
    if (start < 0.)
        start = 0.;
    if (end > m->move_t)
        end = m->move_t;
    double extrude_pos = base;
    double extrude_r = m->axes_r.x;
    pressure_advance = m->axes_r.y;
    struct scurve s;
    scurve_copy_scaled(&m->s, extrude_r, &s);
    extrude_pos += scurve_add_deriv(&m->s, extrude_r * pressure_advance, &s);
    double iext = extruder_integrate(extrude_pos, &s, start, end);
    double wgt_ext = extruder_integrate_time(extrude_pos, &s, start, end);
    return wgt_ext - time_offset * iext;
}

// Calculate the definitive integral of the extruder over a range of moves
static double
pa_range_integrate(struct move *m, double move_time
                   , double pressure_advance, double hst)
{
    // Calculate integral for the current move
    double res = 0., start = move_time - hst, end = move_time + hst;
    double start_base = m->start_pos.x;
    res += pa_move_integrate(m, pressure_advance, 0., start, move_time, start);
    res -= pa_move_integrate(m, pressure_advance, 0., move_time, end, end);
    // Integrate over previous moves
    struct move *prev = m;
    while (unlikely(start < 0.)) {
        prev = list_prev_entry(prev, node);
        start += prev->move_t;
        double base = prev->start_pos.x - start_base;
        res += pa_move_integrate(prev, pressure_advance, base, start
                                 , prev->move_t, start);
    }
    // Integrate over future moves
    while (unlikely(end > m->move_t)) {
        end -= m->move_t;
        m = list_next_entry(m, node);
        double base = m->start_pos.x - start_base;
        res -= pa_move_integrate(m, pressure_advance, base, 0., end, end);
    }
    return res;
}

struct extruder_stepper {
    struct stepper_kinematics sk;
    double pressure_advance, half_smooth_time, inv_half_smooth_time2;
};

static double
extruder_calc_position(struct stepper_kinematics *sk, struct move *m
                       , double move_time)
{
    struct extruder_stepper *es = container_of(sk, struct extruder_stepper, sk);
    double hst = es->half_smooth_time;
    if (!hst)
        // Pressure advance not enabled
        return m->start_pos.x + m->axes_r.x * move_get_distance(m, move_time);
    // Apply pressure advance and average over smooth_time
    double area = pa_range_integrate(m, move_time, es->pressure_advance, hst);
    return m->start_pos.x + area * es->inv_half_smooth_time2;
}

void __visible
extruder_set_pressure_advance(struct stepper_kinematics *sk
                              , double pressure_advance, double smooth_time)
{
    struct extruder_stepper *es = container_of(sk, struct extruder_stepper, sk);
    double hst = smooth_time * .5;
    es->half_smooth_time = hst;
    es->sk.gen_steps_pre_active = es->sk.gen_steps_post_active = hst;
    if (! hst)
        return;
    es->inv_half_smooth_time2 = 1. / (hst * hst);
    es->pressure_advance = pressure_advance;
}

struct stepper_kinematics * __visible
extruder_stepper_alloc(void)
{
    struct extruder_stepper *es = malloc(sizeof(*es));
    memset(es, 0, sizeof(*es));
    es->sk.calc_position_cb = extruder_calc_position;
    es->sk.active_flags = AF_X;
    return &es->sk;
}
