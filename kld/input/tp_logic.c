// tp_logic: turn i2c-hid multitouch slots into what the desktop understands.
// one finger moves the pointer; a short still down/up is a left tap; two
// fingers scroll by their average y; a large/palm contact and too many
// contacts are ignored. pure function, no allocation, no time calls. (satoru)

#include "tp_logic.h"

static const struct tp_config g_default = {
    .tap_slop = 8,
    .tap_ms = 180,
    .scroll_step = 10,
    .palm_size = 30,
    .max_contacts = 3,
};

void tp_logic_init(struct tp_state *s, const struct tp_config *cfg)
{
    (void)cfg;
    for (unsigned i = 0; i < sizeof(*s); i++)
        ((unsigned char *)s)[i] = 0;
}

static int count_active(const struct tp_frame *f, int *first)
{
    int n = 0;
    *first = -1;
    for (int i = 0; i < TP_MAX_CONTACTS; i++) {
        if (!f->c[i].active)
            continue;
        if (*first < 0)
            *first = i;
        n++;
    }
    return n;
}

static int palm(const struct tp_frame *f, const struct tp_config *cfg)
{
    for (int i = 0; i < TP_MAX_CONTACTS; i++)
        if (f->c[i].active && f->c[i].size > cfg->palm_size)
            return 1;
    return 0;
}

int tp_logic_frame(struct tp_state *s, const struct tp_frame *f,
                   struct tp_event *out, int max)
{
    const struct tp_config *cfg = &g_default;
    int first = -1, n = count_active(f, &first);
    int ne = 0;

    if (n > cfg->max_contacts || palm(f, cfg)) {
        // a palm frame is not a pointer frame. reset the gesture so the
        // release does not become a tap. (satoru)
        s->prev_count = 0;
        s->tap_candidate = 0;
        return 0;
    }

    if (s->prev_count == 0 && n == 1) {
        s->down_ms = (int)f->now_ms;
        s->down_x = f->c[first].x;
        s->down_y = f->c[first].y;
        s->tap_candidate = 1;
        s->moved = 0;
        s->prev_x = f->c[first].x;
        s->prev_y = f->c[first].y;
    }

    if (n == 1 && first >= 0) {
        int dx = f->c[first].x - s->prev_x;
        int dy = f->c[first].y - s->prev_y;
        if (dx || dy) {
            s->prev_x = f->c[first].x;
            s->prev_y = f->c[first].y;
            if (dx > cfg->tap_slop || dx < -cfg->tap_slop ||
                dy > cfg->tap_slop || dy < -cfg->tap_slop)
                s->tap_candidate = 0;
            if (s->moved)
                s->tap_candidate = 0;
            s->moved = 1;
            while ((dx || dy) && ne < max) {
                int cx = dx > 100 ? 100 : dx < -100 ? -100 : dx;
                int cy = dy > 100 ? 100 : dy < -100 ? -100 : dy;
                out[ne].type = TP_EV_MOVE;
                out[ne].dx = cx;
                out[ne].dy = cy;
                out[ne].buttons = f->buttons;
                out[ne].wheel = 0;
                ne++;
                dx -= cx;
                dy -= cy;
            }
        }
    }

    if (n == 2) {
        int avg = (f->c[0].active && f->c[1].active) ?
                  (f->c[0].y + f->c[1].y) / 2 : 0;
        if (s->prev_count == 2) {
            s->scroll_accum += avg - s->prev_avg_y;
            while (s->scroll_accum >= cfg->scroll_step) {
                if (ne < max) {
                    out[ne].type = TP_EV_SCROLL;
                    out[ne].dx = 0;
                    out[ne].dy = 0;
                    out[ne].buttons = f->buttons;
                    out[ne].wheel = -1;   // content down (natural-scroll-ready)
                    ne++;
                }
                s->scroll_accum -= cfg->scroll_step;
                s->scroll_count++;
            }
            while (s->scroll_accum <= -cfg->scroll_step) {
                if (ne < max) {
                    out[ne].type = TP_EV_SCROLL;
                    out[ne].dx = 0;
                    out[ne].dy = 0;
                    out[ne].buttons = f->buttons;
                    out[ne].wheel = 1;
                    ne++;
                }
                s->scroll_accum += cfg->scroll_step;
                s->scroll_count++;
            }
        }
        s->prev_avg_y = avg;
        s->tap_candidate = 0;
    }

    if (f->buttons != s->buttons) {
        s->buttons = f->buttons;
        if (ne < max) {
            out[ne].type = TP_EV_BUTTON;
            out[ne].dx = 0;
            out[ne].dy = 0;
            out[ne].buttons = f->buttons;
            out[ne].wheel = 0;
            ne++;
        }
    }

    if (s->prev_count == 1 && n == 0) {
        int still = s->tap_candidate && !s->moved &&
                    (int)f->now_ms - s->down_ms <= cfg->tap_ms;
        if (still) {
            if (ne < max) {
                out[ne].type = TP_EV_BUTTON;
                out[ne].buttons = 1;
                ne++;
            }
            if (ne < max) {
                out[ne].type = TP_EV_BUTTON;
                out[ne].buttons = 0;
                ne++;
            }
            s->tap_count++;
        }
        s->tap_candidate = 0;
    }

    s->prev_count = n;
    return ne;
}

// end (satoru)
