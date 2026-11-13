// tp_logic: pure touchpad-slot to pointer-event logic for the KLD touchpad.
//
// no kernel or shim headers: the same translation unit runs in the container,
// in the host expectation tests, and (later) behind the broker. coordinates are
// screen coordinates, y positive down; wheel is +up. (satoru)

#ifndef KLD_TP_LOGIC_H
#define KLD_TP_LOGIC_H

#define TP_MAX_CONTACTS 5

struct tp_contact {
    int active;
    int x, y;
    int size;       // contact width/area; palm rejection threshold
};

struct tp_frame {
    struct tp_contact c[TP_MAX_CONTACTS];
    int n;          // active contacts reported by i2c-hid
    int buttons;    // physical button state, bit0 left
    unsigned now_ms;
};

enum tp_event_type {
    TP_EV_MOVE = 0,
    TP_EV_BUTTON = 1,
    TP_EV_SCROLL = 2,
};

struct tp_event {
    int type;
    int dx, dy;     // move: screen delta, y positive down
    int buttons;    // button: full left/right/middle bitmask
    int wheel;      // scroll: +up / -down clicks
};

struct tp_config {
    int tap_slop;    // max movement for a tap
    int tap_ms;      // max down time for a tap
    int scroll_step; // y pixels per wheel click
    int palm_size;   // contact size above which the frame is ignored
    int max_contacts;// more contacts than this are ignored
};

struct tp_state {
    int prev_count;
    int prev_x, prev_y;
    int prev_avg_y;
    int down_ms;
    int down_x, down_y;
    int tap_candidate;
    int moved;
    int buttons;
    int scroll_accum;
    int tap_count;
    int scroll_count;
};

void tp_logic_init(struct tp_state *s, const struct tp_config *cfg);
int tp_logic_frame(struct tp_state *s, const struct tp_frame *f,
                   struct tp_event *out, int max);

#endif
// end (satoru)
