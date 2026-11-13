// tp_logic_test: pure-function expectations for the touchpad pointer logic.
// one-finger dx/dy sums, tap click count, two-finger scroll count, palm ignore
// and physical button pass-through. (satoru)

#include <linux/kernel.h>
#include <linux/string.h>

#include "kimpl/lx_kshim.h"
#include "../input/tp_logic.h"

static int g_pass, g_total;

static void result(const char *name, int ok, const char *detail)
{
    lx_log("[tp_logic] ");
    lx_log(name);
    lx_log(ok ? ": PASS " : ": FAIL ");
    if (detail)
        lx_log(detail);
    lx_log("\n");
    g_total++;
    if (ok)
        g_pass++;
}

static void clear(struct tp_state *s)
{
    tp_logic_init(s, NULL);
}

static void one_finger(void)
{
    struct tp_state s;
    struct tp_frame f;
    struct tp_event e[16];
    int n, dx = 0, dy = 0, moves = 0;

    clear(&s);
    memset(&f, 0, sizeof(f));
    f.n = 1; f.c[0].active = 1; f.c[0].x = 100; f.c[0].y = 100;
    f.now_ms = 10;
    n = tp_logic_frame(&s, &f, e, 16);
    for (int i = 0; i < n; i++) if (e[i].type == TP_EV_MOVE) { dx += e[i].dx; dy += e[i].dy; moves++; }
    f.c[0].x = 110; f.c[0].y = 105; f.now_ms = 20;
    n = tp_logic_frame(&s, &f, e, 16);
    for (int i = 0; i < n; i++) if (e[i].type == TP_EV_MOVE) { dx += e[i].dx; dy += e[i].dy; moves++; }
    f.c[0].x = 120; f.c[0].y = 112; f.now_ms = 30;
    n = tp_logic_frame(&s, &f, e, 16);
    for (int i = 0; i < n; i++) if (e[i].type == TP_EV_MOVE) { dx += e[i].dx; dy += e[i].dy; moves++; }
    f.n = 0; f.c[0].active = 0; f.now_ms = 40;
    n = tp_logic_frame(&s, &f, e, 16);
    (void)n;
    result("one_finger_motion", dx == 20 && dy == 12 && moves == 2 &&
           s.tap_count == 0, "dx=20 dy=12 moves=2");
}

static void tap(void)
{
    struct tp_state s;
    struct tp_frame f;
    struct tp_event e[8];
    int n, buttons = 0;

    clear(&s);
    memset(&f, 0, sizeof(f));
    f.n = 1; f.c[0].active = 1; f.c[0].x = 100; f.c[0].y = 100; f.now_ms = 10;
    tp_logic_frame(&s, &f, e, 8);
    f.n = 0; f.c[0].active = 0; f.now_ms = 100;
    n = tp_logic_frame(&s, &f, e, 8);
    for (int i = 0; i < n; i++) if (e[i].type == TP_EV_BUTTON) buttons++;
    result("tap_to_click", buttons == 2 && s.tap_count == 1, "press+release");
}

static void scroll(void)
{
    struct tp_state s;
    struct tp_frame f;
    struct tp_event e[16];
    int n, wheels = 0;

    clear(&s);
    memset(&f, 0, sizeof(f));
    f.n = 2; f.c[0].active = 1; f.c[1].active = 1;
    f.c[0].x = 100; f.c[0].y = 100; f.c[1].x = 140; f.c[1].y = 100;
    f.now_ms = 10; tp_logic_frame(&s, &f, e, 16);
    f.c[0].y = 115; f.c[1].y = 115; f.now_ms = 20;
    n = tp_logic_frame(&s, &f, e, 16);
    for (int i = 0; i < n; i++) if (e[i].type == TP_EV_SCROLL) wheels += e[i].wheel;
    f.c[0].y = 130; f.c[1].y = 130; f.now_ms = 30;
    n = tp_logic_frame(&s, &f, e, 16);
    for (int i = 0; i < n; i++) if (e[i].type == TP_EV_SCROLL) wheels += e[i].wheel;
    result("two_finger_scroll", wheels == -3 && s.scroll_count == 3, "3 wheel down");
}

static void palm(void)
{
    struct tp_state s;
    struct tp_frame f;
    struct tp_event e[8];
    int n;

    clear(&s);
    memset(&f, 0, sizeof(f));
    f.n = 1; f.c[0].active = 1; f.c[0].x = 100; f.c[0].y = 100;
    f.c[0].size = 50; f.now_ms = 10;
    n = tp_logic_frame(&s, &f, e, 8);
    f.n = 0; f.c[0].active = 0; f.now_ms = 50;
    n += tp_logic_frame(&s, &f, e, 8);
    result("palm_ignore", n == 0 && s.tap_count == 0, "large contact ignored");
}

static void fast_swipe(void)
{
    struct tp_state s;
    struct tp_frame f;
    struct tp_event e[16];
    int dx = 0, moves = 0, n;

    clear(&s);
    memset(&f, 0, sizeof(f));
    f.n = 1; f.c[0].active = 1; f.c[0].x = 100; f.c[0].y = 100; f.now_ms = 10;
    tp_logic_frame(&s, &f, e, 16);
    f.c[0].x = 500; f.now_ms = 20;
    n = tp_logic_frame(&s, &f, e, 16);
    for (int i = 0; i < n; i++) if (e[i].type == TP_EV_MOVE) { dx += e[i].dx; moves++; }
    result("fast_swipe_split", dx == 400 && moves >= 4, "400 total in chunks");
}

static void button(void)
{
    struct tp_state s;
    struct tp_frame f;
    struct tp_event e[8];
    int n, down = 0, up = 0;

    clear(&s);
    memset(&f, 0, sizeof(f));
    f.buttons = 1; f.now_ms = 10;
    n = tp_logic_frame(&s, &f, e, 8);
    for (int i = 0; i < n; i++) if (e[i].type == TP_EV_BUTTON && e[i].buttons) down++;
    f.buttons = 0; f.now_ms = 20;
    n = tp_logic_frame(&s, &f, e, 8);
    for (int i = 0; i < n; i++) if (e[i].type == TP_EV_BUTTON && !e[i].buttons) up++;
    result("physical_button", down == 1 && up == 1, "press+release");
}

int kld_test_main(int argc, char **argv)
{
    (void)argc; (void)argv;
    one_finger();
    tap();
    scroll();
    palm();
    button();
    fast_swipe();
    lx_log("[tp_logic] ");
    lx_log_u((unsigned long)g_pass);
    lx_log("/");
    lx_log_u((unsigned long)g_total);
    lx_log(g_pass == g_total ? " PASS\n" : " FAIL\n");
    return g_pass == g_total ? 0 : 1;
}

// end (satoru)
