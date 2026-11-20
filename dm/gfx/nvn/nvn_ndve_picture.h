// The movie picture under DM_NDVE_PICTURE=auto (pure functions; nvn_ndve.inc ndve_auto measures and logs; unit test tests/t_ndve_picture.c). A movie is the network
// frames with no gap of NA_GAP frames between them. Each starts with the network and is decided once from its timed frames: a frame's GPU span (mtl_gpu_span), the
// network's GPU time in it, and the movie's picture period (1 / dm_movie_hz; 0 while no rate is known, a frame that is not counted). The first NA_WARM counted are not
// judged. OVER: the frame is late (span > 1.1 period) and fits without the network (span - network <= 0.9 period); UNDER: span <= 0.9 period. NA_RUN OVER in a row:
// the decoded picture for the rest of the movie; NA_RUN UNDER in a row: the network for the rest of it. Neither is taken back within the movie.
#pragma once
#include <stdint.h>

enum { NA_GAP = 30, NA_WARM = 2, NA_RUN = 3 };
enum { NA_MEASURING, NA_NETWORK, NA_DECODED };
typedef struct {
    uint64_t last;                    // the frame of the last network frame + 1 (0 = none yet)
    int picture, timed, over, under;  // NA_*; frames counted this movie; OVER / UNDER in a row
} ndve_movie;

// 1 when the network frame `frame` starts a movie (m measures again), else 0
static inline int ndve_movie_start(ndve_movie *m, uint64_t frame) {
    const int start = !m->last || frame + 1 > m->last + NA_GAP;
    if (start) { m->picture = NA_MEASURING; m->timed = m->over = m->under = 0; }
    m->last = frame + 1;
    return start;
}
// The movie's picture once a timed frame is counted: span and net in seconds, period in seconds (0: not counted)
static inline int ndve_movie_picture(ndve_movie *m, double span, double net, double period) {
    if (m->picture != NA_MEASURING || period <= 0 || ++m->timed <= NA_WARM) return m->picture;
    const int over = span > period * 1.1 && span - net <= period * 0.9, under = span <= period * 0.9;
    m->over = over ? m->over + 1 : 0; m->under = under ? m->under + 1 : 0;
    if (m->over >= NA_RUN) m->picture = NA_DECODED;
    else if (m->under >= NA_RUN) m->picture = NA_NETWORK;
    return m->picture;
}
