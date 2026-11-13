// terminal_r3.cpp: the terminal as a plain pty emulator on libkurono-ui. (satoru)
// it opens /dev/ptmx through the linux abi, spawns a shell on the slave and draws the pty (satoru)
// through the same Vt emulator and cell renderer the in-kernel terminal uses. (satoru)
// the child is busybox sh when /system/bin/busybox exists, else this same program's --ksh (satoru)
// mode, a small line shell that runs the kernel's built-in commands through /dev/kexec. (satoru)
#include "../kui/kui.h"
#include "vt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>

// the kss terminal chrome, the same defaults the in-kernel terminal resolves (satoru)
static uint32_t T_BG     = 0xFF1B1B1D;
static uint32_t T_FG     = 0xFFF0F0F2;
static uint32_t T_CURSOR = 0xFF3498DB;
static uint32_t T_GRAY   = 0xFF9A9CA8;
static const int PAD_X = 8;
static const int PAD_Y = 6;

static int   CELL_W = 8;
static int   CELL_H = 20;
static float cell_pxh = 16.0f;
static bool  metrics_ready = false;
static int   metrics_scale = -1;

static void EnsureMetrics() {
    int scale = kui::CurrentTheme().ui_scale;
    if (metrics_ready && scale == metrics_scale) return;
    metrics_scale = scale;
    float base_pxh = 16.0f * (float)(scale > 0 ? scale : 100) / 100.0f;
    if (base_pxh < 8.0f) base_pxh = 16.0f;
    cell_pxh = base_pxh;
    for (int k = 0; k < 3; k++) {
        int m = kui::MeasureWidth(kui::Face::Mono, cell_pxh, "M");
        if (m <= 0 || m == 8) break;
        cell_pxh = cell_pxh * 8.0f / (float)m;
    }
    CELL_W = 8;
    CELL_H = (int)(base_pxh * 1.30f + 0.5f);
    if (CELL_W < 4) CELL_W = 4;
    if (CELL_H < 10) CELL_H = 10;
    metrics_ready = true;
}

static void TermDrawRun(kui::Canvas& c, int sx, int sy, const char* s, uint32_t fg, bool bold) {
    if (!s || !s[0]) return;
    uint32_t draw_fg = fg;
    if (bold) {
        uint32_t r = (fg >> 16) & 0xFF, g = (fg >> 8) & 0xFF, b = fg & 0xFF;
        r = r + 50 > 255 ? 255 : r + 50;
        g = g + 50 > 255 ? 255 : g + 50;
        b = b + 50 > 255 ? 255 : b + 50;
        draw_fg = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
    int gy = sy + (CELL_H - (int)cell_pxh) / 2;
    if (gy < sy) gy = sy;
    c.Text(sx, gy, kui::Face::Mono, cell_pxh, s, draw_fg);
    if (bold) c.Text(sx + 1, gy, kui::Face::Mono, cell_pxh, s, draw_fg);
}

// arms as "URDL" digits: 0 none, 1 light, 2 heavy, 3 double. covers u+2500..257f. (satoru)
static const char* const BOX_MIXED[64] = {
    "0110","0210","0120","0220","0011","0012","0021","0022","1100","1200","2100","2200","1001","1002","2001","2002",
    "1110","1210","2110","1120","2120","2210","1220","2220","1011","1012","2011","1021","2021","2012","1022","2022",
    "0111","0112","0211","0212","0121","0122","0221","0222","1101","1102","1201","1202","2101","2102","2201","2202",
    "1111","1112","1211","1212","2111","1121","2121","2112","2211","1122","1221","2212","1222","2122","2221","2222"
};
static const char* const BOX_DBL[29] = {
    "0303","3030","0310","0130","0330","0013","0031","0033","1300","3100","3300","1003","3001","3003",
    "1310","3130","3330","1013","3031","3033","0313","0131","0333","1303","3101","3303","1313","3131","3333"
};

static bool BoxArms(unsigned int cp, const char** out) {
    static const char* const L = "0101"; static const char* const V = "1010";
    static const char* const HL = "0202"; static const char* const HV = "2020";
    if (cp >= 0x250C && cp <= 0x254B) { *out = BOX_MIXED[cp - 0x250C]; return true; }
    if (cp >= 0x2550 && cp <= 0x256C) { *out = BOX_DBL[cp - 0x2550]; return true; }
    switch (cp) {
    case 0x2500: case 0x2504: case 0x2508: case 0x254C: *out = L; return true;
    case 0x2501: case 0x2505: case 0x2509: case 0x254D: *out = HL; return true;
    case 0x2502: case 0x2506: case 0x250A: case 0x254E: *out = V; return true;
    case 0x2503: case 0x2507: case 0x250B: case 0x254F: *out = HV; return true;
    case 0x256D: *out = "0110"; return true;
    case 0x256E: *out = "0011"; return true;
    case 0x256F: *out = "1001"; return true;
    case 0x2570: *out = "1100"; return true;
    case 0x2574: *out = "0001"; return true;
    case 0x2575: *out = "1000"; return true;
    case 0x2576: *out = "0100"; return true;
    case 0x2577: *out = "0010"; return true;
    case 0x2578: *out = "0002"; return true;
    case 0x2579: *out = "2000"; return true;
    case 0x257A: *out = "0200"; return true;
    case 0x257B: *out = "0020"; return true;
    case 0x257C: *out = "0201"; return true;
    case 0x257D: *out = "1020"; return true;
    case 0x257E: *out = "0102"; return true;
    case 0x257F: *out = "2010"; return true;
    default: return false;
    }
}

static uint32_t BlendRGB(uint32_t a, uint32_t b, int num) {
    uint32_t r = (((a >> 16) & 255) * num + ((b >> 16) & 255) * (8 - num)) / 8;
    uint32_t g = (((a >> 8) & 255) * num + ((b >> 8) & 255) * (8 - num)) / 8;
    uint32_t bl = ((a & 255) * num + (b & 255) * (8 - num)) / 8;
    return 0xFF000000u | (r << 16) | (g << 8) | bl;
}

static bool DrawBlockGlyph(kui::Canvas& c, int x, int y, unsigned int cp, uint32_t fg, uint32_t bg) {
    const int W = CELL_W, H = CELL_H;
    if (cp >= 0x2580 && cp <= 0x259F) {
        if (cp == 0x2580) { c.FillRect(x, y, W, H / 2, fg); return true; }
        if (cp >= 0x2581 && cp <= 0x2588) { int h = H * (int)(cp - 0x2580) / 8; c.FillRect(x, y + H - h, W, h, fg); return true; }
        if (cp >= 0x2589 && cp <= 0x258F) { int w = W * (int)(0x2590 - cp) / 8; if (w < 1) w = 1; c.FillRect(x, y, w, H, fg); return true; }
        if (cp == 0x2590) { c.FillRect(x + W / 2, y, W - W / 2, H, fg); return true; }
        if (cp >= 0x2591 && cp <= 0x2593) { c.FillRect(x, y, W, H, BlendRGB(fg, bg, (int)(cp - 0x2590) * 2)); return true; }
        if (cp == 0x2594) { c.FillRect(x, y, W, H / 8 > 0 ? H / 8 : 1, fg); return true; }
        if (cp == 0x2595) { c.FillRect(x + W - W / 8 - 1, y, W / 8 + 1, H, fg); return true; }
        static const unsigned char Q[10] = { 4, 8, 1, 13, 9, 7, 11, 2, 6, 14 };   // ul=1 ur=2 ll=4 lr=8 (satoru)
        unsigned char m = Q[cp - 0x2596];
        int hw = W / 2, hh = H / 2;
        if (m & 1) c.FillRect(x, y, hw, hh, fg);
        if (m & 2) c.FillRect(x + hw, y, W - hw, hh, fg);
        if (m & 4) c.FillRect(x, y + hh, hw, H - hh, fg);
        if (m & 8) c.FillRect(x + hw, y + hh, W - hw, H - hh, fg);
        return true;
    }
    const char* a = nullptr;
    if (!(cp >= 0x2500 && cp <= 0x257F) || !BoxArms(cp, &a)) return false;
    int cxm = x + W / 2, cym = y + H / 2;
    for (int k = 0; k < 4; k++) {            // 0 up, 1 right, 2 down, 3 left (satoru)
        int w = a[k] - '0';
        if (w == 0) continue;
        int t = (w == 2) ? 2 : 1;
        int offs[2] = { 0, 0 }; int cnt = 1;
        if (w == 3) { offs[0] = -2; offs[1] = 1; cnt = 2; t = 1; }
        for (int i = 0; i < cnt; i++) {
            int o = offs[i];
            if (k == 0 || k == 2) {
                int lx = cxm - (t > 1 ? 1 : 0) + o;
                int y0 = (k == 0) ? y : cym - 1, y1 = (k == 0) ? cym + 1 : y + H;
                c.FillRect(lx, y0, t, y1 - y0, fg);
            } else {
                int ly = cym - (t > 1 ? 1 : 0) + o;
                int x0 = (k == 3) ? x : cxm - 1, x1 = (k == 3) ? cxm + 1 : x + W;
                c.FillRect(x0, ly, x1 - x0, t, fg);
            }
        }
    }
    return true;
}

static int Utf8Enc(unsigned int cp, char* o) {
    if (cp < 0x80) { o[0] = (char)cp; o[1] = 0; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 63)); o[2] = 0; return 2; }
    if (cp < 0x10000) { o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 63)); o[2] = (char)(0x80 | (cp & 63)); o[3] = 0; return 3; }
    o[0] = (char)(0xF0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 63)); o[2] = (char)(0x80 | ((cp >> 6) & 63)); o[3] = (char)(0x80 | (cp & 63)); o[4] = 0; return 4;
}

// ---- the --ksh child: a line shell over the kernel's exec bridge (satoru)

static volatile sig_atomic_t g_shell_int = 0;
static void shell_on_int(int) { g_shell_int = 1; }

static int ksh_run() {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = shell_on_int;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);

    int kexec = open("/dev/kexec", O_RDWR);
    char cwd[256];
    if (chdir("/home/user") != 0) { /* stay where we started (satoru) */ }
    if (!getcwd(cwd, sizeof(cwd))) { strcpy(cwd, "/"); }

    for (;;) {
        if (g_shell_int) { g_shell_int = 0; const char* nl = "\r\n"; if (write(1, nl, 2) < 0) {} }
        char prompt[320];
        int pn = snprintf(prompt, sizeof(prompt),
                          "\033[38;5;75m[kurono]\033[0m \033[92muser@kurono\033[0m:\033[96m%s\033[0m$ ", cwd);
        if (pn > 0 && write(1, prompt, (size_t)pn) < 0) {}
        char line[512];
        int n = 0, got = 0;
        while (n < (int)sizeof(line) - 1) {
            char ch;
            ssize_t r = read(0, &ch, 1);
            if (r < 0) {
                if (errno == EINTR) {
                    if (g_shell_int) { g_shell_int = 0; if (write(1, "\r\n", 2) < 0) {} n = 0; got = 1; break; }
                    continue;
                }
                break;
            }
            if (r == 0) return 0;
            got = 1;
            if (ch == '\n') break;
            if (ch == '\r') continue;
            line[n++] = ch;
        }
        line[n] = 0;
        if (!got) return 0;
        while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == ' ' || line[n - 1] == '\t')) line[--n] = 0;
        if (!line[0]) continue;
        if (!strcmp(line, "exit") || !strcmp(line, "logout")) return 0;
        if (!strcmp(line, "clear")) { if (write(1, "\033[2J\033[H", 7) < 0) {} continue; }
        if (!strcmp(line, "pwd")) {   // local: the kernel's pwd asks which backend to use (satoru)
            if (write(1, cwd, strlen(cwd)) < 0) {}
            if (write(1, "\r\n", 2) < 0) {}
            continue;
        }
        if (!strncmp(line, "cd", 2) && (line[2] == 0 || line[2] == ' ')) {
            const char* p = line + 2;
            while (*p == ' ') p++;
            if (!*p) p = "/home/user";
            if (chdir(p) == 0) { if (!getcwd(cwd, sizeof(cwd))) { strcpy(cwd, "/"); } }
            else { const char* err = "cd: no such directory\r\n"; if (write(1, err, strlen(err)) < 0) {} }
            continue;
        }
        if (kexec < 0) {
            const char* msg = "kurono: the kernel command bridge (/dev/kexec) is not available\r\n";
            if (write(1, msg, strlen(msg)) < 0) {}
            continue;
        }
        int len = (int)strlen(line);
        if (write(kexec, line, (size_t)len) != len || write(kexec, "\n", 1) != 1) {
            const char* msg = "kurono: the command bridge refused the command\r\n";
            if (write(1, msg, strlen(msg)) < 0) {}
            continue;
        }
        char ob[256];
        for (;;) {
            ssize_t rr = read(kexec, ob, sizeof(ob));
            if (rr > 0) continue;
            if (rr < 0 && errno == EINTR) {
                if (g_shell_int) {
                    g_shell_int = 0;
                    const char* cancel = "!cancel\n";
                    if (write(kexec, cancel, strlen(cancel)) < 0) {}
                }
                continue;
            }
            break;
        }
    }
}

// ---- the terminal app (satoru)

namespace {

struct TermR3 : kui::AppHandler {
    int master = -1;
    pid_t child = -1;
    bool exited = false;
    int exit_status = 0;
    Vt* vt = nullptr;
    int pty_off = 0;
    int last_w = 0, last_h = 0;
    size_t sel_n = 0;
    char sel_text[16384];

    ~TermR3() override {
        if (master >= 0) {
            kui::WatchFd(master, false);
            close(master);
            master = -1;
        }
        if (child > 0) {
            kill(child, SIGHUP);
            int st = 0;
            waitpid(child, &st, WNOHANG);
            child = -1;
        }
        delete vt;
        vt = nullptr;
    }

    void SizeTo(int w, int h, int cols, int rows) {
        last_w = w; last_h = h;
        if (cols < 20) cols = 20; if (cols > VT_MAXC) cols = VT_MAXC;
        if (rows < 4) rows = 4;   if (rows > VT_MAXR) rows = VT_MAXR;
        if (!vt) vt = new Vt;
        if (!vt) return;
        vt->Init(cols, rows, T_FG, T_BG);
        if (master >= 0) {
            struct winsize ws;
            memset(&ws, 0, sizeof(ws));
            ws.ws_row = (unsigned short)rows;
            ws.ws_col = (unsigned short)cols;
            ioctl(master, TIOCSWINSZ, &ws);
        }
    }

    bool Spawn(int cols, int rows) {
        master = open("/dev/ptmx", O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (master < 0) {
            kui::Log("terminal", "no /dev/ptmx");
            return false;
        }
        int unlock = 0;
        if (ioctl(master, TIOCSPTLCK, &unlock) < 0) {
            kui::Log("terminal", "unlockpt failed");
            close(master); master = -1;
            return false;
        }
        int ptn = -1;
        if (ioctl(master, TIOCGPTN, &ptn) < 0 || ptn < 0) {
            kui::Log("terminal", "TIOCGPTN failed");
            close(master); master = -1;
            return false;
        }
        char slave[32];
        snprintf(slave, sizeof(slave), "/dev/pts/%d", ptn);
        struct winsize ws;
        memset(&ws, 0, sizeof(ws));
        ws.ws_row = (unsigned short)rows;
        ws.ws_col = (unsigned short)cols;
        ioctl(master, TIOCSWINSZ, &ws);

        child = fork();
        if (child < 0) {
            kui::Log("terminal", "fork failed");
            close(master); master = -1;
            return false;
        }
        if (child == 0) {
            // the child owns the slave as its controlling tty and nothing else (satoru)
            setsid();
            int sfd = open(slave, O_RDWR);
            if (sfd < 0) _exit(127);
            dup2(sfd, 0);
            dup2(sfd, 1);
            dup2(sfd, 2);
            if (sfd > 2) close(sfd);
            for (int fd = 3; fd < 256; fd++) close(fd);
            const char* envp[] = {
                "PATH=/system/bin:/usr/bin:/system/kbin", "HOME=/home/user", "USER=user",
                "TERM=xterm-256color", "SHELL=/bin/ksh", "XDG_RUNTIME_DIR=/system/run/user/1000",
                // the same prompt as the --ksh shell, so both look like kurono's terminal (satoru)
                "PS1=\\[\\033[38;5;75m\\][kurono]\\[\\033[0m\\] \\[\\033[92m\\]user@kurono\\[\\033[0m\\]:\\[\\033[96m\\]\\w\\[\\033[0m\\]$ ",
                nullptr
            };
            if (chdir("/home/user") != 0) { /* stay in / (satoru) */ }
            const char* av[] = { "sh", "-i", nullptr };
            execve("/system/bin/busybox", (char* const*)av, (char* const*)envp);
            ksh_run();
            _exit(0);
        }
        if (!kui::WatchFd(master, true)) kui::Log("terminal", "no watch slot");
        return true;
    }

    void OnCreate(kui::Window& w) override {
        EnsureMetrics();
        kui::Size cs = w.ContentSize();
        int cols = (cs.w - 2 * PAD_X) / CELL_W;
        int rows = (cs.h - 2 * PAD_Y) / CELL_H;
        SizeTo(cs.w, cs.h, cols, rows);
        Spawn(vt ? vt->Cols() : 80, vt ? vt->Rows() : 24);
        w.Invalidate();
    }

    void Drain() {
        if (!vt || master < 0) return;
        uint8_t buf[4096];
        int total = 0, n;
        while (total < (1 << 18) && (n = (int)read(master, buf, sizeof(buf))) > 0) {
            vt->Feed(buf, n);
            total += n;
        }
        uint8_t rep[256];
        int rn;
        while ((rn = vt->TakeReply(rep, sizeof(rep))) > 0) {
            if (write(master, rep, (size_t)rn) < 0) break;
        }
        if (total > 0) pty_off = 0;
    }

    void OnTick(kui::Window& w, uint32_t) override {
        if (master < 0) return;
        Drain();
        if (child > 0) {
            int st = 0;
            pid_t r = waitpid(child, &st, WNOHANG);
            if (r == child) {
                child = -1;
                exit_status = st;
                Drain();
                exited = true;
                if (vt) {
                    char msg[96];
                    int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
                    int mn = snprintf(msg, sizeof(msg), "\r\n\033[90m[process completed (%d)]\033[0m\r\n", code);
                    if (mn > 0) vt->Feed((const uint8_t*)msg, mn);
                }
                w.Invalidate();
            }
        }
        if (vt && vt->Dirty()) { vt->ClearDirty(); w.Invalidate(); }
    }

    int Scrollback() const {
        if (!vt) return 0;
        return vt->HistCount();
    }

    const VtCell* RowPtr(int r) {
        if (!vt) return nullptr;
        int off = pty_off;
        int hn = vt->HistCount();
        if (off > hn) off = hn;
        int v = hn - off + r;
        if (v < hn) return vt->HistLine(hn - 1 - v);
        return &vt->Cell(v - hn, 0);
    }

    void OnFrame(kui::Window& w, kui::Canvas& c, uint32_t) override {
        EnsureMetrics();
        T_CURSOR = kui::CurrentTheme().pal.accent;
        if (!vt) { c.Clear(T_BG); return; }
        vt->SetDefaults(T_FG, T_BG);
        int cw = c.Width(), ch = c.Height();
        int cols = (cw - 2 * PAD_X) / CELL_W;
        int rows = (ch - 2 * PAD_Y) / CELL_H;
        if (cols < 20) cols = 20; if (cols > VT_MAXC) cols = VT_MAXC;
        if (rows < 4) rows = 4;   if (rows > VT_MAXR) rows = VT_MAXR;
        if (cols != vt->Cols() || rows != vt->Rows()) {
            vt->Resize(cols, rows);
            if (master >= 0) {
                struct winsize ws;
                memset(&ws, 0, sizeof(ws));
                ws.ws_row = (unsigned short)rows;
                ws.ws_col = (unsigned short)cols;
                ioctl(master, TIOCSWINSZ, &ws);
            }
        }
        int ox = PAD_X, oy = PAD_Y;
        c.Clear(T_BG);
        int hn = vt->HistCount();
        if (pty_off > hn) pty_off = hn;
        char run[VT_MAXC + 1];
        for (int r = 0; r < rows; r++) {
            const VtCell* row = RowPtr(r);
            if (!row) continue;
            int sy = oy + r * CELL_H;
            int col = 0;
            while (col < cols) {
                uint32_t bg = row[col].bg, fg = row[col].fg;
                if (row[col].attr & VA_INVERSE) bg = fg;
                if (bg == T_BG) { col++; continue; }
                int s = col;
                while (col < cols) {
                    uint32_t b2 = row[col].bg, f2 = row[col].fg;
                    if (row[col].attr & VA_INVERSE) b2 = f2;
                    if (b2 != bg) break;
                    col++;
                }
                c.FillRect(ox + s * CELL_W, sy, (col - s) * CELL_W, CELL_H, bg);
            }
            col = 0;
            while (col < cols) {
                const VtCell& k = row[col];
                uint32_t cp = k.cp;
                if (cp == 0 || cp == ' ' || cp == 0xFFFFFFFFu || (k.attr & VA_HIDDEN)) { col++; continue; }
                uint32_t fg = k.fg, bg = k.bg;
                if (k.attr & VA_INVERSE) { uint32_t t = fg; fg = bg; bg = t; }
                if (k.attr & VA_DIM) fg = BlendRGB(fg, bg, 5);
                bool bold = (k.attr & VA_BOLD) != 0;
                if (cp < 0x80) {
                    int s = col, rb = 0;
                    while (col < cols && rb < VT_MAXC) {
                        const VtCell& q = row[col];
                        uint32_t f2 = q.fg, b2 = q.bg;
                        if (q.attr & VA_INVERSE) { uint32_t t = f2; f2 = b2; b2 = t; }
                        if (q.attr & VA_DIM) f2 = BlendRGB(f2, b2, 5);
                        if (q.cp == 0 || q.cp >= 0x80 || q.cp == ' ') break;
                        if (f2 != fg || ((q.attr & VA_BOLD) != 0) != bold) break;
                        run[rb++] = (char)q.cp; col++;
                    }
                    run[rb] = 0;
                    if (rb > 0) TermDrawRun(c, ox + s * CELL_W, sy, run, fg, bold);
                    continue;
                }
                int x = ox + col * CELL_W;
                if (!DrawBlockGlyph(c, x, sy, cp, fg, bg)) {
                    char u[8];
                    Utf8Enc(cp, u);
                    TermDrawRun(c, x, sy, u, fg, bold);
                }
                col++;
            }
            for (int cc = 0; cc < cols; cc++) {
                uint8_t at = row[cc].attr;
                if (!(at & (VA_UNDER | VA_STRIKE))) continue;
                uint32_t fg = (at & VA_INVERSE) ? row[cc].bg : row[cc].fg;
                if (at & VA_UNDER) c.FillRect(ox + cc * CELL_W, sy + CELL_H - 3, CELL_W, 1, fg);
                if (at & VA_STRIKE) c.FillRect(ox + cc * CELL_W, sy + CELL_H / 2, CELL_W, 1, fg);
            }
        }
        if (vt->CursorVisible() && pty_off == 0) {
            int r = vt->CurRow(), col = vt->CurCol();
            if (r >= 0 && r < rows && col >= 0 && col < cols) {
                int x = ox + col * CELL_W, y = oy + r * CELL_H;
                if (w.Focused()) {
                    c.FillRect(x, y, CELL_W, CELL_H, T_CURSOR);
                    const VtCell& k = vt->Cell(r, col);
                    if (k.cp > ' ' && k.cp != 0xFFFFFFFFu) {
                        if (k.cp < 0x80) { char s[2] = { (char)k.cp, 0 }; TermDrawRun(c, x, y, s, T_BG, (k.attr & VA_BOLD) != 0); }
                        else if (!DrawBlockGlyph(c, x, y, k.cp, T_BG, T_CURSOR)) { char u[8]; Utf8Enc(k.cp, u); TermDrawRun(c, x, y, u, T_BG, false); }
                    }
                } else {
                    c.FillRect(x, y, CELL_W, 1, T_CURSOR);
                    c.FillRect(x, y + CELL_H - 1, CELL_W, 1, T_CURSOR);
                    c.FillRect(x, y, 1, CELL_H, T_CURSOR);
                    c.FillRect(x + CELL_W - 1, y, 1, CELL_H, T_CURSOR);
                }
            }
        }
    }

    int PtyCsi(uint8_t* o, int n, int num, char fin, int mod, bool app) {
        o[n++] = 0x1B;
        if (mod > 1) {
            o[n++] = '[';
            if (num == 0) o[n++] = '1';
            else { if (num >= 10) o[n++] = (char)('0' + num / 10); o[n++] = (char)('0' + num % 10); }
            o[n++] = ';'; o[n++] = (char)('0' + mod); o[n++] = fin;
            return n;
        }
        if (num == 0) { o[n++] = app ? 'O' : '['; o[n++] = fin; return n; }
        o[n++] = '[';
        if (num >= 10) o[n++] = (char)('0' + num / 10);
        o[n++] = (char)('0' + num % 10);
        o[n++] = fin;
        return n;
    }

    void Paste(const char* s) {
        if (!vt || master < 0 || !s) return;
        bool br = vt->BracketedPaste();
        if (br) { if (write(master, "\x1b[200~", 6) < 0) return; }
        char cb[256];
        int cn = 0, total = 0;
        for (int i = 0; s[i] && total < 8192; i++, total++) {
            uint8_t ch = (uint8_t)s[i];
            if (ch == '\n') ch = '\r';
            else if (ch < 0x20 && ch != '\t' && ch != '\r') continue;
            cb[cn++] = (char)ch;
            if (cn == (int)sizeof(cb)) { if (write(master, cb, (size_t)cn) < 0) return; cn = 0; }
        }
        if (cn) { if (write(master, cb, (size_t)cn) < 0) return; }
        if (br) { if (write(master, "\x1b[201~", 6) < 0) return; }
        pty_off = 0;
    }

    void CopyScreen() {
        sel_n = 0;
        sel_text[0] = 0;
        if (!vt) return;
        int rows = vt->Rows(), cols = vt->Cols();
        for (int r = 0; r < rows; r++) {
            const VtCell* row = RowPtr(r);
            if (!row) continue;
            int len = cols;
            while (len > 0) {
                uint32_t cp = row[len - 1].cp;
                if (cp != 0 && cp != ' ' && cp != 0xFFFFFFFFu) break;
                len--;
            }
            for (int col = 0; col < len && sel_n + 8 < sizeof(sel_text); col++) {
                uint32_t cp = row[col].cp;
                if (cp == 0xFFFFFFFFu) cp = ' ';
                char u[8];
                int un = Utf8Enc(cp ? cp : ' ', u);
                for (int k = 0; k < un && sel_n + 1 < sizeof(sel_text); k++) sel_text[sel_n++] = u[k];
            }
            if (sel_n + 2 < sizeof(sel_text)) { sel_text[sel_n++] = '\r'; sel_text[sel_n++] = '\n'; }
        }
        sel_text[sel_n] = 0;
        kui::Clipboard::SetText(sel_text);
    }

    bool OnEvent(kui::Window& w, const kui::Event& e) override {
        using kui::EventType;
        if (e.type == EventType::Resize) {
            int cols = (e.w - 2 * PAD_X) / CELL_W;
            int rows = (e.h - 2 * PAD_Y) / CELL_H;
            if (cols < 20) cols = 20; if (cols > VT_MAXC) cols = VT_MAXC;
            if (rows < 4) rows = 4;   if (rows > VT_MAXR) rows = VT_MAXR;
            if (vt && (cols != vt->Cols() || rows != vt->Rows())) {
                vt->Resize(cols, rows);
                if (master >= 0) {
                    struct winsize ws;
                    memset(&ws, 0, sizeof(ws));
                    ws.ws_row = (unsigned short)rows;
                    ws.ws_col = (unsigned short)cols;
                    ioctl(master, TIOCSWINSZ, &ws);
                }
            }
            pty_off = 0;
            return true;
        }
        if (e.type == EventType::Scroll) {
            int d = e.scroll_steps;
            if (d == 0) d = e.dy > 0 ? 1 : (e.dy < 0 ? -1 : 0);
            if (!vt) return false;
            if (d > 0) pty_off += d * 3; else if (d < 0) pty_off += d * 3;
            if (pty_off < 0) pty_off = 0;
            if (pty_off > vt->HistCount()) pty_off = vt->HistCount();
            return true;
        }
        if (e.type != EventType::KeyDown) return false;
        bool shift = (e.mods & kui::M_SHIFT) != 0;
        bool ctrl  = (e.mods & kui::M_CTRL) != 0;
        bool alt   = (e.mods & kui::M_ALT) != 0;
        int key = e.key;
        if (key == kui::K_SHIFT || key == kui::K_CTRL || key == kui::K_ALT || key == kui::K_SUPER) return false;
        if (ctrl && alt && key == 'm') { w.SetMaximized(!w.Maximized()); return true; }
        if (shift && !ctrl && !alt && (key == kui::K_PGUP || key == kui::K_PGDN)) {
            if (!vt) return false;
            pty_off += key == kui::K_PGUP ? 10 : -10;
            if (pty_off < 0) pty_off = 0;
            if (pty_off > vt->HistCount()) pty_off = vt->HistCount();
            return true;
        }
        if (ctrl && shift && key == 'V') { Paste(kui::Clipboard::GetText()); return true; }
        if (ctrl && shift && key == 'C') { CopyScreen(); return true; }
        if (master < 0 || !vt) return false;
        pty_off = 0;
        uint8_t o[64];
        int n = 0;
        int mod = 1 + (shift ? 1 : 0) + (alt ? 2 : 0) + (ctrl ? 4 : 0);
        bool app = vt->AppCursorKeys();
        switch (key) {
        case kui::K_UP:    n = PtyCsi(o, n, 0, 'A', mod, app); break;
        case kui::K_DOWN:  n = PtyCsi(o, n, 0, 'B', mod, app); break;
        case kui::K_RIGHT: n = PtyCsi(o, n, 0, 'C', mod, app); break;
        case kui::K_LEFT:  n = PtyCsi(o, n, 0, 'D', mod, app); break;
        case kui::K_HOME:  n = PtyCsi(o, n, 0, 'H', mod, app); break;
        case kui::K_END:   n = PtyCsi(o, n, 0, 'F', mod, app); break;
        case kui::K_DEL:   n = PtyCsi(o, n, 3, '~', mod, false); break;
        case kui::K_PGUP:  n = PtyCsi(o, n, 5, '~', mod, false); break;
        case kui::K_PGDN:  n = PtyCsi(o, n, 6, '~', mod, false); break;
        case kui::K_F1: case kui::K_F1 + 1: case kui::K_F1 + 2: case kui::K_F1 + 3: {
            char fin = (char)('P' + (key - kui::K_F1));
            if (mod > 1) n = PtyCsi(o, n, 0, fin, mod, false);
            else { o[n++] = 0x1B; o[n++] = 'O'; o[n++] = (uint8_t)fin; }
            break;
        }
        case kui::K_F1 + 4:  n = PtyCsi(o, n, 15, '~', mod, false); break;
        case kui::K_F1 + 5:  n = PtyCsi(o, n, 17, '~', mod, false); break;
        case kui::K_F1 + 6:  n = PtyCsi(o, n, 18, '~', mod, false); break;
        case kui::K_F1 + 7:  n = PtyCsi(o, n, 19, '~', mod, false); break;
        case kui::K_F1 + 8:  n = PtyCsi(o, n, 20, '~', mod, false); break;
        case kui::K_F1 + 9:  n = PtyCsi(o, n, 21, '~', mod, false); break;
        case kui::K_F1 + 10: n = PtyCsi(o, n, 23, '~', mod, false); break;
        case kui::K_F1 + 11: n = PtyCsi(o, n, 24, '~', mod, false); break;
        case kui::K_ENTER: if (alt) o[n++] = 0x1B; o[n++] = '\r'; break;
        case kui::K_BS:    if (alt) o[n++] = 0x1B; o[n++] = ctrl ? 0x08 : 0x7F; break;
        case kui::K_TAB:   if (shift) { o[n++] = 0x1B; o[n++] = '['; o[n++] = 'Z'; } else { if (alt) o[n++] = 0x1B; o[n++] = '\t'; } break;
        case kui::K_ESC:   o[n++] = 0x1B; break;
        default: {
            char ch = e.text[0] ? e.text[0] : ((key >= 32 && key < 127) ? (char)key : 0);
            if (!ch) return false;
            unsigned char u = (unsigned char)ch;
            if (ctrl) {
                if (u >= 'a' && u <= 'z') u = (unsigned char)(u - 'a' + 1);
                else if (u >= 'A' && u <= 'Z') u = (unsigned char)(u - 'A' + 1);
                else if (u == '[' || u == '{') u = 27;
                else if (u == '\\' || u == '|') u = 28;
                else if (u == ']' || u == '}') u = 29;
                else if (u == '^' || u == '6') u = 30;
                else if (u == '_' || u == '-' || u == '/') u = 31;
                else if (u == '@' || u == '2' || u == ' ') u = 0;
                else return false;
            }
            if (alt) o[n++] = 0x1B;
            o[n++] = u;
            break;
        }
        }
        if (n > 0) {
            ssize_t wr = write(master, o, (size_t)n);
            (void)wr;
        }
        return true;
    }
};

kui::AppHandler* MakeTerm() { return new TermR3; }

const kui::WindowSpec kTermSpec = {
    "Terminal", "terminal", 660, 432, 420, 240, -1, -1, true, true, 0, -1
};

}  // namespace

#if defined(KUI_BACKEND_WAYLAND)
int main(int argc, char** argv) {
    if (argc > 1 && argv[1] && !strcmp(argv[1], "--ksh")) return ksh_run();
    return kui::Run(kTermSpec, MakeTerm);
}
#else
int kui_launch_terminal() { return kui::Run(kTermSpec, MakeTerm); }
#endif

// end (satoru)
