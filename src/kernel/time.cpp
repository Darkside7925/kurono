#include "time.h"
#include "../drivers/rtc.h"
#include "../drivers/timer.h"

uint32_t TimeManager::boot_utc_s = 0;
uint32_t TimeManager::monotonic_ms = 0;
uint32_t TimeManager::pit_hz = 1000;
int32_t TimeManager::tz_minutes = 0;
bool TimeManager::dst_on = false;
int32_t TimeManager::ntp_offset_s = 0;
bool TimeManager::rtc_ok = false;
bool TimeManager::battery_ok = false;
bool TimeManager::allow_adjust = false;
uint64_t TimeManager::last_rtc_sync_us = 0;

void TimeManager::SelectPIT(uint32_t hz) { pit_hz = hz ? hz : 1000; }

void TimeManager::Init() {
    RTC::Date d; RTC::Time t;
    rtc_ok = RTC::ReadDateTime(d, t);
    battery_ok = RTC::BatteryOk();
    uint32_t u = rtc_ok ? to_unix_s(d.year, d.mon, d.dom, t.h, t.m, t.s) : 0;
    boot_utc_s = u;
    monotonic_ms = 0;
    ntp_offset_s = 0;
    last_rtc_sync_us = 0;
}

void TimeManager::SetTimezoneMinutes(int32_t minutes) { tz_minutes = minutes; }
void TimeManager::EnableDST(bool on) { dst_on = on; }

SysTime TimeManager::NowUTC() {
    uint32_t div = (1193182u / (pit_hz ? pit_hz : 1000u)); if (div == 0) div = 1;
    uint16_t cur = Timer::ReadCounter();
    uint32_t frac = (uint32_t)((div - (cur % div)) & 0xFFFF);
    uint32_t us_per_tick = (1000000u / pit_hz);
    uint32_t sub_us = (uint32_t)((frac * us_per_tick) / div);
    uint64_t secs = (uint64_t)boot_utc_s + (uint64_t)ntp_offset_s + (uint64_t)(monotonic_ms / 1000u);
    uint32_t rem_ms = (uint32_t)(monotonic_ms % 1000u);
    uint64_t now_us = secs * 1000000ull + (uint64_t)rem_ms * 1000ull + (uint64_t)sub_us;
    SysTime r; r.us = now_us; return r;
}

SysTime TimeManager::NowLocal() {
    SysTime u = NowUTC();
    int64_t off = (int64_t)tz_minutes * 60ll * 1000000ll;
    if (dst_on) off += 3600ll * 1000000ll;
    SysTime r; r.us = (uint64_t)((int64_t)u.us + off); return r;
}

DateTime TimeManager::NowUTCDateTime() {
    uint32_t now_s = boot_utc_s + ntp_offset_s + (monotonic_ms / 1000u);
    return breakdown_s(now_s);
}
DateTime TimeManager::NowLocalDateTime() {
    uint32_t now_s = boot_utc_s + ntp_offset_s + (monotonic_ms / 1000u);
    int32_t off_s = tz_minutes * 60 + (dst_on ? 3600 : 0);
    if (off_s >= 0) now_s += (uint32_t)off_s; else now_s -= (uint32_t)(-off_s);
    return breakdown_s(now_s);
}

void TimeManager::AdjustUTC(int64_t) { }

void TimeManager::SetUTC(SysTime) { }

void TimeManager::AdvanceByMs(uint32_t ms) { monotonic_ms += ms; }

void TimeManager::MaybeSyncRTC(uint64_t now_us) {
    if (!rtc_ok) return;
    uint64_t interval = 6ull * 3600ull * 1000000ull;
    if (last_rtc_sync_us == 0 || now_us - last_rtc_sync_us >= interval) {
        RTC::Date d; RTC::Time t;
        if (RTC::ReadDateTime(d, t)) {
            uint32_t u = to_unix_s(d.year, d.mon, d.dom, t.h, t.m, t.s);
            int32_t now_s = (int32_t)boot_utc_s + ntp_offset_s + (int32_t)(monotonic_ms / 1000u);
            int32_t delta = (int32_t)u - now_s;
            int32_t adj = delta / 8;
            boot_utc_s = (uint32_t)((int32_t)boot_utc_s + adj);
            last_rtc_sync_us = now_us;
        }
    }
}

void TimeManager::ApplyNTPSeconds(uint32_t utc_s) {
    uint32_t now_s = boot_utc_s + (monotonic_ms / 1000u);
    int32_t diff = (int32_t)utc_s - (int32_t)now_s;
    ntp_offset_s = diff;
}

void TimeManager::SetAllowAdjust(bool en) { allow_adjust = en; }

static inline bool leap(uint16_t y) { return ((y % 4) == 0 && (y % 100) != 0) || ((y % 400) == 0); }
static inline uint16_t mdays(uint16_t y, uint8_t m) {
    static const uint8_t t[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (m == 2) return (uint16_t)(t[1] + (leap(y) ? 1 : 0));
    return t[m-1];
}

uint32_t TimeManager::to_unix_s(uint16_t year, uint8_t mon, uint8_t dom, uint8_t h, uint8_t m, uint8_t s) {
    uint32_t days = 0;
    for (uint16_t y = 1970; y < year; y++) days += (leap(y) ? 366u : 365u);
    for (uint8_t mm = 1; mm < mon; mm++) days += mdays(year, mm);
    days += (uint32_t)(dom - 1);
    uint32_t secs = days * 86400u + (uint32_t)h * 3600u + (uint32_t)m * 60u + (uint32_t)s;
    return secs;
}

DateTime TimeManager::breakdown_s(uint32_t secs) {
    uint32_t days = secs / 86400u;
    uint32_t rem = secs % 86400u;
    uint16_t year = 1970;
    while (true) { uint16_t dpy = (uint16_t)(leap(year) ? 366 : 365); if (days >= (uint32_t)dpy) { days -= (uint32_t)dpy; year++; } else break; }
    uint8_t mon = 1;
    while (true) { uint16_t dm = mdays(year, mon); if (days >= (uint32_t)dm) { days -= (uint32_t)dm; mon++; } else break; }
    uint8_t dom = (uint8_t)(days + 1);
    uint8_t h = (uint8_t)(rem / 3600u); rem %= 3600u;
    uint8_t mi = (uint8_t)(rem / 60u); rem %= 60u;
    uint8_t se = (uint8_t)rem;
    uint32_t days_since_epoch = secs / 86400u;
    uint8_t dow = (uint8_t)(((days_since_epoch + 4u) % 7u));
    dow = (uint8_t)(dow == 0 ? 7 : dow);
    DateTime dt; dt.year = year; dt.mon = mon; dt.dom = dom; dt.dow = dow; dt.h = h; dt.m = mi; dt.s = se; return dt;
}
