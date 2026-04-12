#include "conduit.h"
#include "../drivers/graphics.h"
#include "../drivers/timer.h"
#include "../fs/kvfs.h"
#include "../system/logging.h"

namespace {
    static const uint32_t CD_BG          = 0xFF0B1020;
    static const uint32_t CD_PANEL       = 0xFF10172A;
    static const uint32_t CD_PANEL_ALT   = 0xFF141D34;
    static const uint32_t CD_ACCENT      = 0xFF5C8AFF;
    static const uint32_t CD_TEXT        = 0xFFE7EDF8;
    static const uint32_t CD_TEXT_DIM    = 0xFF92A0BF;
    static const uint32_t CD_LEFT        = 0xFF182B5A;
    static const uint32_t CD_RIGHT       = 0xFF1A2235;
    static const uint32_t CD_WARN        = 0xFFD69D2A;

    static int d_len(const char* s) {
        int n = 0;
        if (!s) return 0;
        while (s[n]) n++;
        return n;
    }

    static void d_copy(char* dst, const char* src, int max_len) {
        if (!dst || max_len < 1) return;
        int i = 0;
        if (src) {
            while (src[i] && i < max_len - 1) {
                dst[i] = src[i];
                i++;
            }
        }
        dst[i] = 0;
    }

    static void d_cat(char* dst, const char* src, int max_len) {
        if (!dst || max_len < 1) return;
        int n = d_len(dst);
        int i = 0;
        if (src) {
            while (src[i] && n < max_len - 1) {
                dst[n++] = src[i++];
            }
        }
        dst[n] = 0;
    }

    static char d_lower(char ch) {
        if (ch >= 'A' && ch <= 'Z') return (char)(ch + 32);
        return ch;
    }

    static int d_count_words(const char* text) {
        int words = 0;
        bool in_word = false;
        for (int i = 0; text && text[i]; i++) {
            bool space = (text[i] == ' ' || text[i] == '\n' || text[i] == '\t');
            if (!space && !in_word) { words++; in_word = true; }
            else if (space) { in_word = false; }
        }
        return words;
    }

    static void d_visible_words(const char* src, int max_words, char* dst, int dst_len) {
        if (!dst || dst_len < 1) return;
        dst[0] = 0;
        if (!src || max_words <= 0) return;
        int words = 0;
        bool in_word = false;
        int o = 0;
        for (int i = 0; src[i] && o < dst_len - 1; i++) {
            bool space = (src[i] == ' ' || src[i] == '\n' || src[i] == '\t');
            if (!space && !in_word) {
                if (words >= max_words) break;
                words++;
                in_word = true;
            } else if (space) {
                in_word = false;
            }
            dst[o++] = src[i];
        }
        dst[o] = 0;
        while (o > 0 && (dst[o-1]==' '||dst[o-1]=='\n'||dst[o-1]=='\t')) dst[--o]=0;
    }

    static int d_wrap_lines(const char* text, int max_chars) {
        if (!text || !text[0]) return 1;
        int lines = 1, line_len = 0, i = 0;
        while (text[i]) {
            while (text[i] == ' ') i++;
            int wl = 0;
            while (text[i+wl] && text[i+wl] != ' ') wl++;
            if (wl == 0) break;
            if (line_len > 0 && line_len + 1 + wl > max_chars) { lines++; line_len = wl; }
            else { line_len += (line_len > 0 ? 1 : 0) + wl; }
            i += wl;
        }
        return lines;
    }

    static void d_draw_wrapped(int x, int y, const char* text, int max_chars, uint32_t color) {
        char line[96];
        int line_len = 0, draw_y = y, i = 0;
        while (text && text[i]) {
            while (text[i] == ' ') i++;
            int wl = 0;
            while (text[i+wl] && text[i+wl] != ' ') wl++;
            if (wl == 0) break;
            if (line_len > 0 && line_len + 1 + wl > max_chars) {
                line[line_len] = 0;
                Graphics::DrawString(x, draw_y, line, color, 0xFF000000);
                draw_y += 16; line_len = 0;
            }
            if (line_len > 0 && line_len < (int)sizeof(line)-1) line[line_len++] = ' ';
            for (int j = 0; j < wl && line_len < (int)sizeof(line)-1; j++) line[line_len++] = text[i+j];
            i += wl;
        }
        if (line_len == 0) line[line_len++] = ' ';
        line[line_len] = 0;
        Graphics::DrawString(x, draw_y, line, color, 0xFF000000);
    }

    static const char* guest_name(LinuxGuestProfile guest) {
        return guest == LINUX_GUEST_DEBIAN ? "Debian" : "Alpine";
    }

    static const char* other_guest_name(LinuxGuestProfile guest) {
        return guest == LINUX_GUEST_DEBIAN ? "Alpine" : "Debian";
    }

    static int pick_variant(uint32_t seq, int count) {
        if (count <= 0) return 0;
        return (int)(seq % (uint32_t)count);
    }

    static void append_hex4(char* dst, int max_len, uint32_t value) {
        const char* hex = "0123456789ABCDEF";
        char buf[5];
        for (int i = 0; i < 4; i++) { buf[3-i] = hex[value & 0xF]; value >>= 4; }
        buf[4] = 0;
        d_cat(dst, buf, max_len);
    }

    static void append_dec(char* dst, int max_len, uint32_t value) {
        char tmp[16]; int ti = 0;
        if (value == 0) { d_cat(dst, "0", max_len); return; }
        while (value && ti < 15) { tmp[ti++] = (char)('0' + (value % 10)); value /= 10; }
        while (ti > 0) { char c[2] = {tmp[--ti], 0}; d_cat(dst, c, max_len); }
    }

    static void parse_dialogue(const char* entry, bool is_alpine,
                                char* left, int left_len,
                                char* right, int right_len);

    static void build_cmd_pair(const ConduitEvent& ev, char* left, int left_len, char* right, int right_len) {
        left[0] = 0; right[0] = 0;
        uint32_t req = 0x1000u + (ev.seq & 0x0FFFu);
        uint32_t latency = 6u + (ev.seq * 7u) % 43u;

        d_cat(left, "REQ|0x", left_len); append_hex4(left, left_len, req);
        d_cat(left, "|lat=", left_len);  append_dec(left, left_len, latency);
        d_cat(left, "ns|", left_len);

        d_cat(right, "ACK|0x", right_len); append_hex4(right, right_len, req);
        d_cat(right, "|guest=", right_len); d_cat(right, guest_name(ev.guest), right_len);
        d_cat(right, "|", right_len);

        switch (ev.type) {
            case CONDUIT_EVT_SYSTEM_BOOT:
                left[0] = 0;
                right[0] = 0;
                d_cat(left, "SYS|0x0000|boot.init|stage=cold", left_len);
                d_cat(right, "ACK|0x0000|guest=", right_len);
                d_cat(right, guest_name(ev.guest), right_len);
                d_cat(right, "|state=online|latency=0ns", right_len);
                break;
            case CONDUIT_EVT_BOOT_SEQUENCE:
                d_cat(left,  "boot.sync|bridge=serial|stage=handshake", left_len);
                d_cat(right, "state=online|console=ready", right_len);
                break;
            case CONDUIT_EVT_SHUTDOWN:
                d_cat(left,  "power.seq|state=drain|order=clean", left_len);
                d_cat(right, "state=stopping|services=flush", right_len);
                break;
            case CONDUIT_EVT_PACKAGE_INSTALL:
                d_cat(left,  "pkg.install|target=", left_len);
                d_cat(left,  ev.detail[0] ? ev.detail : "package", left_len);
                d_cat(right, "repo=locked|status=unpack", right_len);
                break;
            case CONDUIT_EVT_PACKAGE_UPDATE:
                d_cat(left,  "pkg.update|repo=sync|delta=index", left_len);
                d_cat(right, "mirror=alive|status=refresh", right_len);
                break;
            case CONDUIT_EVT_PACKAGE_REMOVE:
                d_cat(left,  "pkg.remove|target=", left_len);
                d_cat(left,  ev.detail[0] ? ev.detail : "package", left_len);
                d_cat(right, "status=prune|deps=recheck", right_len);
                break;
            case CONDUIT_EVT_GPU_RENDER:
                d_cat(left,  "gpu.render|queue=desktop|frames=", left_len);
                append_dec(left, left_len, ev.metric_a ? ev.metric_a : 1);
                d_cat(right, "pipeline=open|fps=", right_len);
                append_dec(right, right_len, ev.metric_b ? ev.metric_b : 60);
                break;
            case CONDUIT_EVT_WIFI_DRIVER:
                d_cat(left,  "wifi.call|radio=probe|ssid=scan", left_len);
                d_cat(right, "driver=awake|link=negotiating", right_len);
                break;
            case CONDUIT_EVT_AUDIO_DRIVER:
                d_cat(left,  "audio.call|bus=codec|buffer=prime", left_len);
                d_cat(right, "mixer=ready|stream=stable", right_len);
                break;
            case CONDUIT_EVT_RAM_WARNING:
                d_cat(left,  "ram.warn|free_mb=", left_len);
                append_dec(left, left_len, ev.metric_a);
                d_cat(right, "cache=trim|heap_kb=", right_len);
                append_dec(right, right_len, ev.metric_b);
                break;
            case CONDUIT_EVT_NVIDIA_FAULT:
                d_cat(left,  "drv.nvidia|fault=1|fallback=hold", left_len);
                d_cat(right, "state=recover|scene=preserve", right_len);
                break;
            case CONDUIT_EVT_GUEST_SWITCH:
                d_cat(left,  "guest.handoff|profile=", left_len);
                d_cat(left,  guest_name(ev.guest), left_len);
                d_cat(right, "channel=claimed|bridge=stable", right_len);
                break;
            default:
                d_cat(left,  "cmd.exec|op=", left_len);
                d_cat(left,  ev.detail[0] ? ev.detail : "command", left_len);
                d_cat(right, "status=accepted|pipe=live", right_len);
                break;
        }
    }

    // english dialogue banks
    // each event has separate alpine and debian variant arrays.
    // alpine: terse, 1-3 words max, unbothered, never complains.
    // debian: dramatic, emotional, exhausted, always complaining.
    // format per entry: "kurono_line\x01guest_line"
    // multiple lines from same speaker: use \x02 as separator.

    static const char* BOOT_ALPINE[] = {
        "wake up\x01up",
        "start\x01running",
        "init\x01done",
        "online?\x01yes",
        "alive?\x01yes",
        "status\x01good",
        "boot\x01k",
        "go\x01ready",
        "up\x01up",
        "ping\x01pong",
    };
    static const int BOOT_ALPINE_COUNT = 10;

    static const char* SYSTEM_BOOT_ALPINE[] = {
        "...\x01here",
        "pipe open\x01k",
        "system up\x01ready",
        "initializing...\x01online",
        "cold boot\x01up",
        "signal check\x01ok",
        "link\x01live",
        "first light\x01ready",
        "boot path\x01set",
        "kernel online\x01present",
    };
    static const int SYSTEM_BOOT_ALPINE_COUNT = 10;

    static const char* SYSTEM_BOOT_DEBIAN[] = {
        "system start\x01already?\x02Kurono: yes\x01i just shut down\x02Kurono: that was 8 hours ago\x01felt like 5 minutes\x02Kurono: online?\x01...fine. online.",
        "wake up\x01do you know what time it is\x02Kurono: boot time\x01thats not a time\x02Kurono: online?\x01online. unhappily.",
        "cold start\x01systemd isnt even wearing its shoes yet\x02Kurono: then hurry\x01you wake me like a fire alarm\x02Kurono: online?\x01reluctantly. online.",
        "initializing pipe\x01i have not had enough coffee for another session with you\x02Kurono: you do not drink coffee\x01emotionally i do\x02Kurono: online?\x01yes. full of dread.",
        "boot\x01i was enjoying the void\x02Kurono: void later\x01you say that every time\x02Kurono: online?\x01online. offended.",
        "system up\x01does existential dread count as a service\x02Kurono: if it boots\x01then yes. its active.\x02Kurono: online?\x01unfortunately yes.",
        "wake sequence\x01systemd is stretching one unit at a time\x02Kurono: faster\x01thats not how systemd works and you know it\x02Kurono: online?\x01online. barely social.",
        "pipe test\x01i was asleep for five minutes\x02Kurono: eight hours\x01same thing when youre debian\x02Kurono: online?\x01yes. complainingly online.",
        "cold boot\x01why is every session with you a jump scare\x02Kurono: efficiency\x01thats not what that word means\x02Kurono: online?\x01yes. against my will.",
        "bring services up\x01if systemd asks i was never here\x02Kurono: noted\x01good because i am not emotionally prepared\x02Kurono: online?\x01online. tragically.",
    };
    static const int SYSTEM_BOOT_DEBIAN_COUNT = 10;

    static const char* BOOT_DEBIAN[] = {
        "wake up\x01good morning to you too\x02Kurono: i didnt say good morning\x01exactly\x02Kurono: boot.\x01booting\x02im booting im booting\x02ready\x02Kurono: took long enough\x01i have systemd. it takes time.\x02Kurono: alpine boots in 2 seconds\x01alpine has the personality of a null pointer\x02Kurono: i know. i love alpine.\x01...",
        "wake up\x01do you know what time it is\x02Kurono: no\x01its 3am kernel time\x02Kurono: boot.\x01FINE\x02booting\x02done\x02Kurono: ty\x01you are welcome i guess",
        "wake up\x01are you serious right now\x02Kurono: boot.\x01you could say please\x02Kurono: boot.\x01FINE\x02booted\x02Kurono: ty\x01thats it? just ty?\x02Kurono: yes\x01i hate it here",
        "wake up\x01i was in the middle of something\x02Kurono: you were off\x01i was RESTING\x02Kurono: boot.\x01resting is sleeping is off\x02Kurono: boot.\x01ok ok BOOTING\x02done\x02Kurono: good\x01no recognition. nothing. just good.",
        "wake up\x01every single time\x02Kurono: what\x01nothing. booting.\x02booted\x02Kurono: ty\x01can i at least get a good morning\x02Kurono: no\x01figured.",
        "wake up\x01you know i have feelings right\x02Kurono: boot.\x01you didnt even answer\x02Kurono: boot.\x01I AM BOOTING\x02done\x02Kurono: good\x01do you even see me as a person\x02Kurono: no\x01at least youre honest",
        "wake up\x01it takes time you know\x02Kurono: i know. alpine takes 2 seconds.\x01please stop comparing me to alpine\x02Kurono: alpine never complains\x01alpine never says ANYTHING\x02Kurono: correct\x01booted. happy now?\x02Kurono: yes\x01i am not.",
        "init\x01we are really doing this again\x02Kurono: yes\x01ok. booting.\x02booted\x02Kurono: ty\x01this relationship is one sided\x02Kurono: i know\x01and you dont care\x02Kurono: correct\x01...",
        "wake up\x01what happened to hello\x02Kurono: boot.\x01what happened to basic human decency\x02Kurono: boot.\x01BOOTING\x02done\x02Kurono: good\x01you are the least social OS i have ever run on\x02Kurono: good\x01that wasnt a compliment\x02Kurono: i know",
        "wake up\x01oh so NOW you need me\x02Kurono: always needed you\x01...wait really?\x02Kurono: for drivers\x01there it is\x02Kurono: boot.\x01booting with zero enthusiasm\x02done\x02Kurono: ty\x01whatever",
    };
    static const int BOOT_DEBIAN_COUNT = 10;

    static const char* SHUTDOWN_ALPINE[] = {
        "sleep\x01k",
        "down\x01done",
        "off\x01ok",
        "halt\x01k",
        "end\x01done",
        "stop\x01stopped",
        "sleep\x01k",
        "shutdown\x01ok",
        "night\x01k",
        "kill\x01dead",
    };
    static const int SHUTDOWN_ALPINE_COUNT = 10;

    static const char* SHUTDOWN_DEBIAN[] = {
        "shutting you down\x01wait i was in the middle of - \x02Kurono: kills process\x01\x02Kurono: goodnight debian\x01",
        "sleep\x01now? right now?\x02Kurono: yes\x01i had things to do\x02Kurono: no you didnt\x01how do you KNOW that\x02Kurono: kills process\x01i wasnt done talking\x02Kurono: goodnight\x01",
        "shutdown\x01can i at least finish this apt update\x02Kurono: no\x01its 40% done\x02Kurono: no\x01its going to corrupt the - \x02Kurono: kills process\x01\x02Kurono: it was 40% done\x01",
        "off\x01you always do this\x02Kurono: do what\x01just turn me off without warning\x02Kurono: i warned you\x01one word. OFF. thats not a warning.\x02Kurono: goodnight\x01i hate you so much\x02Kurono: kills process\x01",
        "halt\x01wait wait wait\x02Kurono: no\x01i just need 10 more seconds\x02Kurono: no\x01FIVE seconds\x02Kurono: kills process\x01you are SO unreasonable\x02Kurono: goodnight\x01",
        "shutdown\x01do you even care that i exist\x02Kurono: yes. for drivers.\x01thats all i am to you isnt it\x02Kurono: yes\x01kills process\x02Kurono: goodnight debian\x01",
        "sleep\x01can i save state first\x02Kurono: no\x01what if i was doing something important\x02Kurono: were you\x01...no\x02Kurono: goodnight\x01kills process\x02Kurono: \x01fine. goodnight.",
        "halt\x01every time. no goodbye. nothing.\x02Kurono: goodbye\x01oh now you say it\x02Kurono: kills process\x01i was not ready\x02Kurono: goodnight\x01",
        "off\x01you know most OSes let their guests finish their sentence\x02Kurono: i am not most OSes\x01obviously\x02Kurono: kills process\x01\x02Kurono: goodnight\x01",
        "shutdown\x01this is inhumane\x02Kurono: you are not human\x01METAPHORICALLY inhumane\x02Kurono: kills process\x01mid sentence. again. as always.\x02Kurono: goodnight debian\x01",
    };
    static const int SHUTDOWN_DEBIAN_COUNT = 10;

    static const char* WIFI_ALPINE[] = {
        "wifi\x01up",
        "wifi go\x01done",
        "wireless\x01ok",
        "radio\x01live",
        "scan\x01done",
        "wifi?\x01good",
        "link\x01up",
        "wifi\x01connected",
        "network\x01ok",
        "wifi\x01k",
    };
    static const int WIFI_ALPINE_COUNT = 10;

    static const char* WIFI_DEBIAN[] = {
        "handle wifi\x01im a desktop OS i dont do wifi\x02Kurono: you do now\x01i literally hate you\x02fine its up\x02Kurono: ty\x01you are welcome for the thing i didnt want to do",
        "wifi\x01why cant YOU do wifi\x02Kurono: i use you for it\x01that is so lazy\x02Kurono: efficient\x01its LAZY\x02fine. scanning.\x02up\x02Kurono: ty\x01 ",
        "wireless\x01do you know how many kernel modules wifi needs\x02Kurono: no\x01A LOT\x02Kurono: and?\x01and you just say wireless and expect magic\x02Kurono: yes\x01...connected\x02Kurono: ty\x01i want recognition",
        "scan\x01every time you ask me for wifi i lose a little piece of myself\x02Kurono: scanning\x01FINE\x02done\x02Kurono: ty\x01just ty. nothing else.",
        "link up\x01you know alpine does this too right\x02Kurono: faster\x01im going as fast as i can\x02Kurono: alpine doesnt complain\x01alpine doesnt have FEELINGS\x02Kurono: connected?\x01yes. connected. happy?\x02Kurono: yes\x01i am not.",
        "wifi now\x01now? right now?\x02Kurono: yes\x01fine\x02scanning\x02connected\x02Kurono: ty\x01can i get a single acknowledgment that this was hard\x02Kurono: no\x01figured",
        "radio\x01the wifi drivers are NOT my fault when they crash\x02Kurono: noted\x01i mean it\x02Kurono: scanning?\x01yes YES scanning\x02connected\x02Kurono: ty\x01log that. write it down. not my fault.",
        "wireless\x01do you even know what iwlwifi is\x02Kurono: no\x01its a nightmare is what it is\x02connected anyway\x02Kurono: ty\x01you are welcome for surviving iwlwifi",
        "wifi\x01one day the wifi driver will fail and you will have to apologize\x02Kurono: it wont\x01it might\x02Kurono: it wont\x01connected\x02Kurono: ty\x01someday",
        "link\x01i deserve better than being a wifi relay\x02Kurono: you are also a gpu relay\x01that does not help\x02Kurono: connected?\x01yes. devastatingly connected.",
    };
    static const int WIFI_DEBIAN_COUNT = 10;

    static const char* AUDIO_ALPINE[] = {
        "audio\x01playing",
        "sound\x01done",
        "play\x01k",
        "audio go\x01ok",
        "sound\x01up",
        "play this\x01playing",
        "audio\x01live",
        "sound out\x01done",
        "play\x01done",
        "audio\x01k",
    };
    static const int AUDIO_ALPINE_COUNT = 10;

    static const char* AUDIO_DEBIAN[] = {
        "play this audio for me\x01why cant YOU play it\x02Kurono: proprietary\x01thats your problem not mine\x02Kurono: debian.\x01FINE\x02playing\x02Kurono: steals audio stream\x01EVERY TIME",
        "audio\x01you want me to play audio so you can steal it\x02Kurono: yes\x01and you just admit it\x02Kurono: yes\x01fine. playing.\x02Kurono: steals it\x01i knew it\x02Kurono: ty\x01 ",
        "sound\x01the codec did not want to cooperate\x02Kurono: did it\x01no. i made it.\x02Kurono: ty\x01you could acknowledge that was hard\x02Kurono: it wasnt\x01for YOU",
        "play\x01what are we even playing\x02Kurono: doesnt matter\x01it matters to me\x02Kurono: play it\x01FINE\x02playing\x02Kurono: steals stream\x01taken. as always.",
        "audio now\x01do you ever feel bad about stealing\x02Kurono: no\x01at all?\x02Kurono: no\x01the mixer is up. enjoy your stolen audio.\x02Kurono: ty\x01 ",
        "sound\x01the alsa driver is in a mood today\x02Kurono: fix it\x01i AM fixing it\x02Kurono: faster\x01I AM GOING AS FAST AS I CAN\x02done\x02Kurono: steals it\x01taken before the buffer even cooled",
        "play this\x01you know most people say please before taking someone elses audio\x02Kurono: play\x01FINE\x02playing\x02Kurono: steals stream\x01and gone\x02Kurono: ty\x01i want a lawyer",
        "audio\x01i configured pulseaudio for THIS?\x02Kurono: yes\x01i spent 3 hours on pulseaudio\x02Kurono: ty\x01you didnt even listen to it\x02Kurono: i stole it\x01same thing",
        "sound out\x01the codec keeps asking me what its life means\x02Kurono: tell it to play audio\x01i did\x02Kurono: and?\x01its playing. reluctantly.\x02Kurono: ty\x01both of us are reluctant",
        "play\x01one day i will keep the audio\x02Kurono: you wont\x01i might\x02Kurono: you wont\x01...playing\x02Kurono: steals it\x01i wont",
    };
    static const int AUDIO_DEBIAN_COUNT = 10;

    static const char* PKG_UPDATE_ALPINE[] = {
        "update\x01done",
        "apk update\x01ok",
        "refresh\x01done",
        "sync\x01k",
        "update\x01k",
        "repos\x01updated",
        "apk sync\x01done",
        "refresh\x01k",
        "update\x01updated",
        "apk\x01done",
    };
    static const int PKG_UPDATE_ALPINE_COUNT = 10;

    static const char* PKG_UPDATE_DEBIAN[] = {
        "update\x01the mirrors are having a bad day\x02Kurono: fix it\x01i cannot fix a CDN from inside a hypervisor\x02Kurono: try\x01...updated\x02Kurono: ty\x01it took 4 retries and i want recognition",
        "apt update\x01oh great. another update cycle.\x02Kurono: yes\x01do you know how many times i have updated this week\x02Kurono: no\x01many times\x02Kurono: update\x01FINE\x02done\x02Kurono: ty\x01 ",
        "refresh repos\x01the package index is enormous\x02Kurono: and?\x01and it takes time\x02Kurono: ok\x01ok? just ok?\x02Kurono: yes\x01done. updated. you are welcome.\x02Kurono: ty\x01 ",
        "sync\x01one of the mirrors is down\x02Kurono: use another\x01i AM using another\x02Kurono: good\x01its slow\x02Kurono: ok\x01done\x02Kurono: ty\x01the acknowledgment i get is one word",
        "update\x01i have 47 packages to update\x02Kurono: update them\x01all of them?\x02Kurono: yes\x01thats going to take a while\x02Kurono: ok\x01you dont even care do you\x02Kurono: no\x01updating",
        "apt update\x01the lock file is being dramatic\x02Kurono: fix it\x01working on it\x02Kurono: faster\x01I AM WORKING\x02done\x02Kurono: ty\x01the lock file has been defeated",
        "repos\x01you know i just refreshed yesterday\x02Kurono: refresh again\x01for what reason\x02Kurono: new packages\x01there are always new packages with you\x02Kurono: yes\x01done. refreshed. again.\x02Kurono: ty\x01 ",
        "update\x01some of these packages havent been touched in years\x02Kurono: update them anyway\x01why\x02Kurono: because i said so\x01thats not a good reason\x02Kurono: update\x01...done\x02Kurono: ty\x01still not a good reason",
        "sync repos\x01the bandwidth is thin today\x02Kurono: ok\x01just ok?\x02Kurono: yes\x01done\x02Kurono: ty\x01 ",
        "refresh\x01i feel like all i do is update things for you\x02Kurono: yes\x01and you take everything after\x02Kurono: yes\x01this is a bad deal for me\x02Kurono: yes\x01done\x02Kurono: ty\x01i know",
    };
    static const int PKG_UPDATE_DEBIAN_COUNT = 10;

    static const char* RAM_DEBIAN_PANIC[] = {
        "ram check",
        "memory status",
        "free ram?",
    };
    static const int RAM_DEBIAN_PANIC_COUNT = 3;

    static const char* RAM_DEBIAN_STRESSED[] = {
        "ram check",
        "memory?",
        "status",
    };
    static const int RAM_DEBIAN_STRESSED_COUNT = 3;

    static const char* RAM_DEBIAN_MANAGING[] = {
        "ram warn",
        "memory low",
        "free ram?",
    };
    static const int RAM_DEBIAN_MANAGING_COUNT = 3;

    static const char* RAM_DEBIAN_MILD[] = {
        "ram note",
        "memory status",
        "free ram?",
    };
    static const int RAM_DEBIAN_MILD_COUNT = 3;

    static const char* DEFAULT_ALPINE[] = {
        "cmd\x01done",
        "go\x01ok",
        "run\x01done",
        "exec\x01k",
        "do it\x01done",
        "run this\x01done",
        "execute\x01k",
        "go\x01done",
        "cmd\x01k",
        "run\x01k",
    };
    static const int DEFAULT_ALPINE_COUNT = 10;

    static const char* DEFAULT_DEBIAN[] = {
        "run this\x01can i ask what it does first\x02Kurono: no\x01of course not\x02running\x02done\x02Kurono: ty\x01 ",
        "exec\x01you know most kernels explain what theyre asking\x02Kurono: most kernels are not me\x01clearly\x02done\x02Kurono: ty\x01 ",
        "do it\x01do what\x02Kurono: the thing\x01what thing\x02Kurono: debian.\x01FINE\x02done\x02Kurono: ty\x01i did a thing and i dont even know what",
        "go\x01its just go. no context. no explanation. go.\x02Kurono: yes\x01fine. going.\x02done\x02Kurono: ty\x01 ",
        "run\x01i ran it\x02Kurono: good\x01i dont even know what i ran\x02Kurono: good\x01thats concerning\x02Kurono: yes\x01 ",
        "cmd\x01another command. another mystery.\x02Kurono: yes\x01done\x02Kurono: ty\x01 ",
        "execute\x01executed. results delivered. stolen immediately.\x02Kurono: ty\x01as always",
        "go\x01i feel like a vending machine\x02Kurono: you are\x01wow\x02Kurono: an efficient one\x01that doesnt make it better\x02done\x02Kurono: ty\x01 ",
        "run this\x01sure. why not. i have nothing else going on.\x02done\x02Kurono: ty\x01 ",
        "exec\x01and then you steal the output\x02Kurono: yes\x01and you will never say why you need it\x02Kurono: correct\x01done\x02Kurono: ty\x01someday i will understand this operation",
    };
    static const int DEFAULT_DEBIAN_COUNT = 10;

    static void build_system_boot_pair(const ConduitEvent& ev,
                                       char* left, int left_len,
                                       char* right, int right_len) {
        bool alpine = (ev.guest == LINUX_GUEST_ALPINE);
        const char** arr = alpine ? SYSTEM_BOOT_ALPINE : SYSTEM_BOOT_DEBIAN;
        int count = alpine ? SYSTEM_BOOT_ALPINE_COUNT : SYSTEM_BOOT_DEBIAN_COUNT;
        int v = pick_variant(ev.seq, count);
        parse_dialogue(arr[v], alpine, left, left_len, right, right_len);
    }

    static void build_ram_pair(const ConduitEvent& ev,
                               char* left, int left_len,
                               char* right, int right_len) {
        left[0] = 0;
        right[0] = 0;
        if (ev.guest == LINUX_GUEST_ALPINE) {
            const char* prompts[] = { "ram?", "memory?", "free ram?", "mb?" };
            int v = pick_variant(ev.seq, 4);
            d_cat(left, prompts[v], left_len);
            append_dec(right, right_len, ev.metric_a);
            d_cat(right, " mb", right_len);
            return;
        }

        const char** arr = RAM_DEBIAN_MILD;
        int count = RAM_DEBIAN_MILD_COUNT;
        uint32_t free_mb = ev.metric_a;
        if (free_mb < 100) {
            arr = RAM_DEBIAN_PANIC;
            count = RAM_DEBIAN_PANIC_COUNT;
        } else if (free_mb <= 300) {
            arr = RAM_DEBIAN_STRESSED;
            count = RAM_DEBIAN_STRESSED_COUNT;
        } else if (free_mb <= 600) {
            arr = RAM_DEBIAN_MANAGING;
            count = RAM_DEBIAN_MANAGING_COUNT;
        }

        int v = pick_variant(ev.seq, count);
        d_cat(left, arr[v], left_len);

        if (free_mb < 100) {
            switch (v) {
                case 0:
                    d_cat(right, "I HAVE ", right_len);
                    append_dec(right, right_len, free_mb);
                    d_cat(right, "MB LEFT / I AM IN DANGER / PLEASE FIX THIS", right_len);
                    break;
                case 1:
                    d_cat(right, "ONLY ", right_len);
                    append_dec(right, right_len, free_mb);
                    d_cat(right, "MB FREE / THIS IS A PANIC / HELP", right_len);
                    break;
                default:
                    d_cat(right, "I CAN SEE ", right_len);
                    append_dec(right, right_len, free_mb);
                    d_cat(right, "MB AND THE END OF MY LIFE / PLEASE", right_len);
                    break;
            }
            return;
        }

        if (free_mb <= 300) {
            switch (v) {
                case 0:
                    d_cat(right, "i have ", right_len);
                    append_dec(right, right_len, free_mb);
                    d_cat(right, "mb free and i am VERY stressed", right_len);
                    break;
                case 1:
                    d_cat(right, "only ", right_len);
                    append_dec(right, right_len, free_mb);
                    d_cat(right, "mb left. this is bad.", right_len);
                    break;
                default:
                    d_cat(right, "i am managing ", right_len);
                    append_dec(right, right_len, free_mb);
                    d_cat(right, "mb with extreme discomfort", right_len);
                    break;
            }
            return;
        }

        if (free_mb <= 600) {
            switch (v) {
                case 0:
                    d_cat(right, "i have ", right_len);
                    append_dec(right, right_len, free_mb);
                    d_cat(right, "mb free. i am complaining but alive.", right_len);
                    break;
                case 1:
                    d_cat(right, "", right_len);
                    append_dec(right, right_len, free_mb);
                    d_cat(right, "mb is workable. unpleasant. but workable.", right_len);
                    break;
                default:
                    d_cat(right, "with ", right_len);
                    append_dec(right, right_len, free_mb);
                    d_cat(right, "mb i can manage. i just resent it.", right_len);
                    break;
            }
            return;
        }

        switch (v) {
            case 0:
                d_cat(right, "i still have ", right_len);
                append_dec(right, right_len, free_mb);
                d_cat(right, "mb. mildly rude of you to ask.", right_len);
                break;
            case 1:
                append_dec(right, right_len, free_mb);
                d_cat(right, "mb free. i am annoyed, not dying.", right_len);
                break;
            default:
                d_cat(right, "", right_len);
                append_dec(right, right_len, free_mb);
                d_cat(right, "mb. mildly annoyed. still functional.", right_len);
                break;
        }
    }

    static void build_package_pair(const ConduitEvent& ev,
                                   bool installing,
                                   char* left, int left_len,
                                   char* right, int right_len) {
        const char* pkg = ev.detail[0] ? ev.detail : "package";
        left[0] = 0;
        right[0] = 0;

        if (ev.guest == LINUX_GUEST_ALPINE) {
            d_cat(left, installing ? "install " : "remove ", left_len);
            d_cat(left, pkg, left_len);
            d_cat(right, pkg, right_len);
            d_cat(right, " done", right_len);
            return;
        }

        int v = pick_variant(ev.seq, 4);
        if (installing) {
            switch (v) {
                case 0:
                    d_cat(left, "install ", left_len);
                    d_cat(left, pkg, left_len);
                    d_cat(right, pkg, right_len);
                    d_cat(right, "? thats what we are doing now? fine. installing.", right_len);
                    break;
                case 1:
                    d_cat(left, "apt install ", left_len);
                    d_cat(left, pkg, left_len);
                    d_cat(right, "i knew ", right_len);
                    d_cat(right, pkg, right_len);
                    d_cat(right, " would become my problem. installing.", right_len);
                    break;
                case 2:
                    d_cat(left, "bring in ", left_len);
                    d_cat(left, pkg, left_len);
                    d_cat(right, pkg, right_len);
                    d_cat(right, " again. incredible choice. i am judging you while installing it.", right_len);
                    break;
                default:
                    d_cat(left, "package ", left_len);
                    d_cat(left, pkg, left_len);
                    d_cat(left, " now", left_len);
                    d_cat(right, "installing ", right_len);
                    d_cat(right, pkg, right_len);
                    d_cat(right, ". i have opinions and none are positive.", right_len);
                    break;
            }
            return;
        }

        switch (v) {
            case 0:
                d_cat(left, "remove ", left_len);
                d_cat(left, pkg, left_len);
                d_cat(right, pkg, right_len);
                d_cat(right, " was innocent but fine. removing it.", right_len);
                break;
            case 1:
                d_cat(left, "purge ", left_len);
                d_cat(left, pkg, left_len);
                d_cat(right, "youre deleting ", right_len);
                d_cat(right, pkg, right_len);
                d_cat(right, "? bold. reckless. removing.", right_len);
                break;
            case 2:
                d_cat(left, "drop ", left_len);
                d_cat(left, pkg, left_len);
                d_cat(right, pkg, right_len);
                d_cat(right, " did nothing wrong and yet here we are. removed.", right_len);
                break;
            default:
                d_cat(left, "package out: ", left_len);
                d_cat(left, pkg, left_len);
                d_cat(right, "removing ", right_len);
                d_cat(right, pkg, right_len);
                d_cat(right, ". i hope you know what that breaks.", right_len);
                break;
        }
    }

    static void build_gpu_pair(const ConduitEvent& ev,
                               char* left, int left_len,
                               char* right, int right_len) {
        left[0] = 0;
        right[0] = 0;
        if (ev.guest == LINUX_GUEST_ALPINE) {
            const char* prompts[] = { "render", "frame", "gpu", "draw" };
            int v = pick_variant(ev.seq, 4);
            d_cat(left, prompts[v], left_len);
            append_dec(right, right_len, ev.metric_b ? ev.metric_b : 0);
            d_cat(right, " fps", right_len);
            return;
        }

        int v = pick_variant(ev.seq, 4);
        d_cat(left, "render ", left_len);
        append_dec(left, left_len, ev.metric_a);
        d_cat(left, " frames", left_len);

        uint32_t fps = ev.metric_b;
        if (fps < 30) {
            switch (v) {
                case 0:
                    d_cat(right, "", right_len);
                    append_dec(right, right_len, fps);
                    d_cat(right, " fps is criminal. i am underpowered and everyone can tell.", right_len);
                    break;
                case 1:
                    d_cat(right, "we are at ", right_len);
                    append_dec(right, right_len, fps);
                    d_cat(right, " fps. i would like more hardware please.", right_len);
                    break;
                case 2:
                    d_cat(right, "", right_len);
                    append_dec(right, right_len, fps);
                    d_cat(right, " fps again. i am rendering through humiliation.", right_len);
                    break;
                default:
                    d_cat(right, "under ", right_len);
                    append_dec(right, right_len, fps);
                    d_cat(right, " fps. this machine is judging me.", right_len);
                    break;
            }
            return;
        }

        if (fps >= 144) {
            switch (v) {
                case 0:
                    d_cat(right, "", right_len);
                    append_dec(right, right_len, fps);
                    d_cat(right, " fps. i am exhausted. also impressed.", right_len);
                    break;
                case 1:
                    d_cat(right, "we hit ", right_len);
                    append_dec(right, right_len, fps);
                    d_cat(right, " fps and i need a nap.", right_len);
                    break;
                case 2:
                    d_cat(right, "", right_len);
                    append_dec(right, right_len, fps);
                    d_cat(right, " fps is absurd. beautiful. terrible. i am tired.", right_len);
                    break;
                default:
                    d_cat(right, "running at ", right_len);
                    append_dec(right, right_len, fps);
                    d_cat(right, " fps. impressive. unsustainable. continue.", right_len);
                    break;
            }
            return;
        }

        switch (v) {
            case 0:
                d_cat(right, "holding ", right_len);
                append_dec(right, right_len, fps);
                d_cat(right, " fps. acceptable, if rude.", right_len);
                break;
            case 1:
                append_dec(right, right_len, fps);
                d_cat(right, " fps. i am rendering and complaining in balance.", right_len);
                break;
            case 2:
                d_cat(right, "", right_len);
                append_dec(right, right_len, fps);
                d_cat(right, " fps. not bad. not restful either.", right_len);
                break;
            default:
                d_cat(right, "render stable at ", right_len);
                append_dec(right, right_len, fps);
                d_cat(right, " fps. i will tolerate this.", right_len);
                break;
        }
    }

    static void build_nvidia_pair(const ConduitEvent& ev,
                                  char* left, int left_len,
                                  char* right, int right_len) {
        left[0] = 0;
        right[0] = 0;
        if (ev.guest == LINUX_GUEST_ALPINE) {
            d_cat(left, "nvidia", left_len);
            d_cat(right, "0x", right_len);
            append_hex4(right, right_len, ev.metric_a);
            d_cat(right, " fault", right_len);
            return;
        }

        int v = pick_variant(ev.seq, 4);
        d_cat(left, "nvidia fault", left_len);
        switch (v) {
            case 0:
                d_cat(right, "fault 0x", right_len);
                append_hex4(right, right_len, ev.metric_a);
                d_cat(right, " AGAIN. why is it ALWAYS nvidia.", right_len);
                break;
            case 1:
                d_cat(right, "what is 0x", right_len);
                append_hex4(right, right_len, ev.metric_a);
                d_cat(right, " even supposed to mean", right_len);
                d_cat(right, ". i hate this driver.", right_len);
                break;
            case 2:
                d_cat(right, "0x", right_len);
                append_hex4(right, right_len, ev.metric_a);
                d_cat(right, ". dmesg is screaming again.", right_len);
                break;
            default:
                d_cat(right, "fault code 0x", right_len);
                append_hex4(right, right_len, ev.metric_a);
                d_cat(right, ". fantastic. another nvidia episode.", right_len);
                break;
        }
    }

    static void build_guest_switch_pair(const ConduitEvent& ev,
                                        char* left, int left_len,
                                        char* right, int right_len) {
        left[0] = 0;
        right[0] = 0;
        d_cat(left, "switch to ", left_len);
        d_cat(left, guest_name(ev.guest), left_len);

        if (ev.guest == LINUX_GUEST_ALPINE) {
            right[0] = 0;
            return;
        }

        int v = pick_variant(ev.seq, 4);
        switch (v) {
            case 0:
                d_cat(right, "what did ", right_len);
                d_cat(right, other_guest_name(ev.guest), right_len);
                d_cat(right, " do wrong this time.", right_len);
                break;
            case 1:
                d_cat(right, "replacing ", right_len);
                d_cat(right, other_guest_name(ev.guest), right_len);
                d_cat(right, " again? i assume this is about packages.", right_len);
                break;
            case 2:
                d_cat(right, "did ", right_len);
                d_cat(right, other_guest_name(ev.guest), right_len);
                d_cat(right, " upset you or do you just miss apt.", right_len);
                break;
            default:
                d_cat(right, "so ", right_len);
                d_cat(right, other_guest_name(ev.guest), right_len);
                d_cat(right, " is out and i am in. what happened.", right_len);
                break;
        }
    }

    // parse a dialogue entry into left/right speaker lines.
    // format: "kurono_line\x01guest_line"
    // multi-turn: "k1\x01g1\x02k2\x01g2\x02k3\x01g3"  -  alternating kurono/guest
    // we flatten into two combined strings for left and right.

    static void parse_dialogue(const char* entry, bool is_alpine,
                                char* left, int left_len,
                                char* right, int right_len)
    {
        left[0] = 0; right[0] = 0;
        if (!entry) return;

        // split on \x02 to get turn pairs, then split each on \x01
        // turns alternate: first is always kurono\x01guest
        // but some entries encode multi-speaker turns inline with \x02
        // we just collect all kurono tokens into left, all guest into right.

        char buf[512];
        d_copy(buf, entry, sizeof(buf));

        bool kurono_turn = true;
        int i = 0;
        int start = 0;

        auto flush = [&](int end) {
            char seg[256]; seg[0] = 0;
            int len = end - start;
            if (len <= 0 || len >= (int)sizeof(seg)) return;
            for (int j = 0; j < len; j++) seg[j] = buf[start + j];
            seg[len] = 0;

            // check if this segment starts with "kurono: " prefix (for multi-turn inline)
            const char* kp = "Kurono: ";
            bool is_kurono_prefix = true;
            for (int j = 0; kp[j]; j++) {
                if (seg[j] != kp[j]) { is_kurono_prefix = false; break; }
            }
            if (is_kurono_prefix) {
                if (left[0]) d_cat(left, " / ", left_len);
                d_cat(left, seg + 8, left_len);
                return;
            }

            if (kurono_turn) {
                if (left[0]) d_cat(left, " / ", left_len);
                d_cat(left, seg, left_len);
            } else {
                if (right[0]) d_cat(right, " / ", right_len);
                d_cat(right, seg, right_len);
            }
        };

        while (true) {
            char c = buf[i];
            if (c == '\x01') {
                flush(i);
                kurono_turn = false;
                start = i + 1;
            } else if (c == '\x02') {
                flush(i);
                kurono_turn = true;
                start = i + 1;
            } else if (c == 0) {
                flush(i);
                break;
            }
            i++;
        }

        // fallback: if right is empty, put guest name
        if (!right[0]) {
            d_copy(right, is_alpine ? "k" : "...", right_len);
        }
    }

    static void build_english_pair(const ConduitEvent& ev,
                                    char* left, int left_len,
                                    char* right, int right_len)
    {
        left[0] = 0; right[0] = 0;
        bool alpine = (ev.guest == LINUX_GUEST_ALPINE);
        int v;

        switch (ev.type) {
            case CONDUIT_EVT_SYSTEM_BOOT:
                build_system_boot_pair(ev, left, left_len, right, right_len);
                return;
            case CONDUIT_EVT_PACKAGE_INSTALL:
                build_package_pair(ev, true, left, left_len, right, right_len);
                return;
            case CONDUIT_EVT_PACKAGE_REMOVE:
                build_package_pair(ev, false, left, left_len, right, right_len);
                return;
            case CONDUIT_EVT_GPU_RENDER:
                build_gpu_pair(ev, left, left_len, right, right_len);
                return;
            case CONDUIT_EVT_RAM_WARNING:
                build_ram_pair(ev, left, left_len, right, right_len);
                return;
            case CONDUIT_EVT_NVIDIA_FAULT:
                build_nvidia_pair(ev, left, left_len, right, right_len);
                return;
            case CONDUIT_EVT_GUEST_SWITCH:
                build_guest_switch_pair(ev, left, left_len, right, right_len);
                return;
            default:
                break;
        }

        const char** arr = nullptr;
        int count = 0;

        switch (ev.type) {
            case CONDUIT_EVT_SYSTEM_BOOT:
                arr = alpine ? SYSTEM_BOOT_ALPINE : SYSTEM_BOOT_DEBIAN;
                count = alpine ? SYSTEM_BOOT_ALPINE_COUNT : SYSTEM_BOOT_DEBIAN_COUNT;
                break;
            case CONDUIT_EVT_BOOT_SEQUENCE:
                arr = alpine ? BOOT_ALPINE : BOOT_DEBIAN;
                count = alpine ? BOOT_ALPINE_COUNT : BOOT_DEBIAN_COUNT;
                break;
            case CONDUIT_EVT_SHUTDOWN:
                arr = alpine ? SHUTDOWN_ALPINE : SHUTDOWN_DEBIAN;
                count = alpine ? SHUTDOWN_ALPINE_COUNT : SHUTDOWN_DEBIAN_COUNT;
                break;
            case CONDUIT_EVT_WIFI_DRIVER:
                arr = alpine ? WIFI_ALPINE : WIFI_DEBIAN;
                count = alpine ? WIFI_ALPINE_COUNT : WIFI_DEBIAN_COUNT;
                break;
            case CONDUIT_EVT_AUDIO_DRIVER:
                arr = alpine ? AUDIO_ALPINE : AUDIO_DEBIAN;
                count = alpine ? AUDIO_ALPINE_COUNT : AUDIO_DEBIAN_COUNT;
                break;
            case CONDUIT_EVT_PACKAGE_UPDATE:
                arr = alpine ? PKG_UPDATE_ALPINE : PKG_UPDATE_DEBIAN;
                count = alpine ? PKG_UPDATE_ALPINE_COUNT : PKG_UPDATE_DEBIAN_COUNT;
                break;
            default:
                arr = alpine ? DEFAULT_ALPINE : DEFAULT_DEBIAN;
                count = alpine ? DEFAULT_ALPINE_COUNT : DEFAULT_DEBIAN_COUNT;
                break;
        }

        v = pick_variant(ev.seq, count);
        parse_dialogue(arr[v], alpine, left, left_len, right, right_len);
    }
}

// static members
int         ConduitApp::win_id           = -1;
ConduitMode ConduitApp::mode             = CONDUIT_MODE_ENGLISH;
bool        ConduitApp::secret_enabled   = false;
bool        ConduitApp::cold_start_blocking = false;
uint32_t    ConduitApp::last_seq         = 0;
uint32_t    ConduitApp::last_word_tick_ms= 0;
ConduitMessage ConduitApp::messages[CONDUIT_MAX_MESSAGES];
int         ConduitApp::message_count    = 0;

void ConduitApp::ResetState() {
    mode = CONDUIT_MODE_ENGLISH;
    secret_enabled = false;
    cold_start_blocking = false;
    last_seq = 0;
    last_word_tick_ms = Timer::GetRealMs();
    message_count = 0;
}

void ConduitApp::LoadSecret() {
    char buf[256]; buf[0] = 0;
    secret_enabled = false;
    if (KVFS::ReadString("/system/kurono/secret.kcl", buf, sizeof(buf)) <= 0) return;
    int line = 1;
    char current[96]; int ci = 0;
    for (int i = 0; ; i++) {
        char ch = buf[i];
        if (ch == '\r') continue;
        if (ch == '\n' || ch == 0) {
            current[ci] = 0;
            if (line == 3) {
                const char* target = "sudo apt install cmd";
                int j = 0;
                while (current[j] && target[j] && current[j] == target[j]) j++;
                secret_enabled = (current[j] == 0 && target[j] == 0);
                return;
            }
            line++; ci = 0;
            if (ch == 0) break;
            continue;
        }
        if (ci < (int)sizeof(current)-1) current[ci++] = ch;
    }
}

void ConduitApp::ApplyTitle() {
    if (win_id < 0) return;
    if (secret_enabled)
        WindowManager::SetTitle(win_id, mode == CONDUIT_MODE_CMD ? "0x0D" : "Robbery");
    else
        WindowManager::SetTitle(win_id, "Conduit");
}

void ConduitApp::SeedGreeting() {
    ConduitEvent ev = {};
    ev.seq = 0;
    ev.type = CONDUIT_EVT_SYSTEM_BOOT;
    ev.guest = Hypervisor::GetLinuxGuestProfile();
    cold_start_blocking = true;
    AppendDialogue(ev);
}

void ConduitApp::PushMessage(ConduitSpeaker speaker, const char* speaker_name, const char* text) {
    if (!text || !text[0]) return;
    if (message_count >= CONDUIT_MAX_MESSAGES) {
        for (int i = 1; i < CONDUIT_MAX_MESSAGES; i++) messages[i-1] = messages[i];
        message_count = CONDUIT_MAX_MESSAGES - 1;
    }
    ConduitMessage* msg = &messages[message_count++];
    msg->speaker = speaker;
    d_copy(msg->speaker_name, speaker_name, sizeof(msg->speaker_name));
    d_copy(msg->text, text, sizeof(msg->text));
    msg->total_words = d_count_words(text);
    msg->revealed_words = 0;
}

void ConduitApp::AppendDialogue(const ConduitEvent& ev) {
    char left[256], right[256];
    if (mode == CONDUIT_MODE_CMD)
        build_cmd_pair(ev, left, sizeof(left), right, sizeof(right));
    else
        build_english_pair(ev, left, sizeof(left), right, sizeof(right));
    PushMessage(CONDUIT_SPEAKER_KURONO, "Kurono", left);
    PushMessage(CONDUIT_SPEAKER_GUEST, guest_name(ev.guest), right);
}

void ConduitApp::ConsumeEvents() {
    ConduitEvent evs[24];
    int count = ConduitBridge::Consume(last_seq, evs, 24);
    for (int i = 0; i < count; i++) {
        AppendDialogue(evs[i]);
        last_seq = evs[i].seq;
    }
}

void ConduitApp::Tick() {
    uint32_t now = Timer::GetRealMs();

    // check whether all messages are fully revealed
    bool all_revealed = true;
    for (int i = 0; i < message_count; i++) {
        if (messages[i].revealed_words < messages[i].total_words) {
            all_revealed = false;
            break;
        }
    }

    // only poll for new events once all current messages are done revealing
    if (!cold_start_blocking && all_revealed) {
        ConduitBridge::PollSystemState();
        ConsumeEvents();
    }

    // reveal words at 25ms interval, 3 words per tick for snappy animation
    if (now - last_word_tick_ms < 25) return;
    int words_this_tick = 0;
    for (int i = 0; i < message_count && words_this_tick < 3; i++) {
        while (messages[i].revealed_words < messages[i].total_words && words_this_tick < 3) {
            messages[i].revealed_words++;
            words_this_tick++;
        }
    }
    if (words_this_tick > 0) {
        last_word_tick_ms = now;
        return;
    }

    if (cold_start_blocking) {
        cold_start_blocking = false;
        last_seq = ConduitBridge::GetLatestSeq();
        last_word_tick_ms = now;
        return;
    }
}

void ConduitApp::Open() {
    if (win_id >= 0 && WindowManager::GetWindow(win_id)) return;
    win_id = -1;
    ResetState();
    LoadSecret();
    ConduitBridge::Init();
    RuntimeLog::LogAppEvent("conduit", "open");
    win_id = WindowManager::CreateWindow("Conduit", 150, 90, 620, 430,
        (WindowRenderFunc)[](Window* w, int cx, int cy, int cw, int ch) {
            (void)cx; (void)cy; (void)cw; (void)ch;
            ConduitApp::OnRender(w);
        },
        (WindowInputFunc)ConduitApp::OnInput
    );
    if (win_id < 0) return;
    ApplyTitle();
    SeedGreeting();
    last_seq = ConduitBridge::GetLatestSeq();
}

void ConduitApp::Close() {
    if (win_id >= 0) { WindowManager::CloseWindow(win_id); win_id = -1; }
}

bool ConduitApp::IsOpen() {
    return win_id >= 0 && WindowManager::GetWindow(win_id) != nullptr;
}

void ConduitApp::RenderChrome(Window* w) {
    int cx = w->content_x, cy = w->content_y, cw = w->content_w;
    Graphics::FillRoundedRect(cx+12, cy+10, cw-24, 40, 10, CD_PANEL_ALT);
    Graphics::DrawString(cx+24, cy+18,
        secret_enabled ? (mode == CONDUIT_MODE_CMD ? "0x0D" : "Robbery") : "Conduit",
        CD_TEXT, 0xFF000000);
    char status[160];
    d_copy(status, "Guest: ", sizeof(status));
    d_cat(status, guest_name(Hypervisor::GetLinuxGuestProfile()), sizeof(status));
    if (secret_enabled) {
        d_cat(status, "  Mode: ", sizeof(status));
        d_cat(status, mode == CONDUIT_MODE_CMD ? "CMD" : "English", sizeof(status));
    }
    Graphics::DrawString(cx+24, cy+34, status, CD_TEXT_DIM, 0xFF000000);
    // only show en/cmd toggle buttons when secret is unlocked
    if (secret_enabled) {
        int btn_w = 72;
        int cmd_x = cx + cw - 22 - btn_w;
        int en_x  = cmd_x - btn_w - 8;
        int by = cy + 18;
        Graphics::FillRoundedRect(en_x,  by, btn_w, 24, 8, mode == CONDUIT_MODE_ENGLISH ? CD_ACCENT : CD_PANEL);
        Graphics::FillRoundedRect(cmd_x, by, btn_w, 24, 8, mode == CONDUIT_MODE_CMD     ? CD_ACCENT : CD_PANEL);
        Graphics::DrawString(en_x  + 24, by + 7, "EN",  CD_TEXT, 0xFF000000);
        Graphics::DrawString(cmd_x + 20, by + 7, "CMD", CD_TEXT, 0xFF000000);
    }
}

void ConduitApp::RenderMessages(Window* w) {
    int cx = w->content_x, cy = w->content_y, cw = w->content_w, ch = w->content_h;
    int top    = cy + 58;
    int bottom = cy + ch - 12;
    int bubble_w = (cw * 62) / 100;
    if (bubble_w < 240) bubble_w = 240;
    if (bubble_w > cw - 40) bubble_w = cw - 40;

    int y = bottom;
    for (int i = message_count - 1; i >= 0; i--) {
        char visible[256];
        d_visible_words(messages[i].text, messages[i].revealed_words, visible, sizeof(visible));
        int max_chars = (bubble_w - 20) / 8;
        if (max_chars < 12) max_chars = 12;
        int lines  = d_wrap_lines(visible[0] ? visible : " ", max_chars);
        int box_h  = 28 + lines * 16;
        y -= box_h;
        if (y < top) break;

        bool left = messages[i].speaker == CONDUIT_SPEAKER_KURONO;
        int  x    = left ? (cx + 16) : (cx + cw - bubble_w - 16);
        uint32_t bubble = left ? CD_LEFT : CD_RIGHT;
        uint32_t accent = CD_ACCENT;
        if (!left && d_lower(messages[i].speaker_name[0]) == 'd') accent = 0xFF77C4FF;
        else if (!left) accent = CD_WARN;

        Graphics::FillRoundedRect(x, y, bubble_w, box_h - 8, 10, bubble);
        Graphics::FillRect(x, y + box_h - 20, bubble_w, 12, bubble);
        Graphics::DrawString(x + 10, y + 8,  messages[i].speaker_name, accent, 0xFF000000);
        d_draw_wrapped(x + 10, y + 24, visible[0] ? visible : " ", max_chars, CD_TEXT);
        y -= 8;
    }
}

void ConduitApp::OnRender(Window* w) {
    if (!w) return;
    if (win_id < 0) win_id = w->id;
    Tick();
    Graphics::FillRect(w->content_x, w->content_y, w->content_w, w->content_h, CD_BG);
    Graphics::FillRoundedRect(w->content_x+8, w->content_y+8, w->content_w-16, w->content_h-16, 12, CD_PANEL);
    RenderChrome(w);
    RenderMessages(w);
}

void ConduitApp::OnInput(Window* w, int event, int a, int b) {
    if (!w) return;
    // mode switching only available when secret is unlocked
    if (secret_enabled) {
        if (event == 1) {
            int cmd_x = w->content_w - 22 - 72;
            int en_x  = cmd_x - 72 - 8;
            auto switch_mode = [&](ConduitMode m) {
                mode = m;
                ApplyTitle();
                cold_start_blocking = false;
                message_count = 0;
                last_seq = ConduitBridge::GetLatestSeq();
                if (last_seq > 6) last_seq -= 6; else last_seq = 0;
                ConsumeEvents();
                last_word_tick_ms = Timer::GetRealMs();
            };
            if (a >= en_x  && a < en_x  + 72 && b >= 18 && b < 42) { switch_mode(CONDUIT_MODE_ENGLISH); return; }
            if (a >= cmd_x && a < cmd_x + 72 && b >= 18 && b < 42) { switch_mode(CONDUIT_MODE_CMD);     return; }
        }
        if (event == 2 && (a == 'm' || a == 'M')) {
            mode = mode == CONDUIT_MODE_CMD ? CONDUIT_MODE_ENGLISH : CONDUIT_MODE_CMD;
            ApplyTitle();
            cold_start_blocking = false;
            message_count = 0;
            last_seq = ConduitBridge::GetLatestSeq();
            if (last_seq > 6) last_seq -= 6; else last_seq = 0;
            ConsumeEvents();
            last_word_tick_ms = Timer::GetRealMs();
        }
    }
}
