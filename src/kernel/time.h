#pragma once
#include "types.h"

struct SysTime { uint64_t us; };
struct DateTime { uint16_t year; uint8_t mon; uint8_t dom; uint8_t dow; uint8_t h; uint8_t m; uint8_t s; };

class TimeManager {
public:
    static void SelectPIT(uint32_t hz);
    static void Init();
    static void SetTimezoneMinutes(int32_t minutes);
    static void EnableDST(bool on);
    static SysTime NowUTC();
    static SysTime NowLocal();
    static DateTime NowUTCDateTime();
    static DateTime NowLocalDateTime();
    static void AdjustUTC(int64_t delta_us);
    static void SetUTC(SysTime t);
    static void AdvanceByMs(uint32_t ms);
    static void MaybeSyncRTC(uint64_t now_us);
    static void ApplyNTPSeconds(uint32_t utc_s);
    static void SetAllowAdjust(bool en);
private:
    static uint32_t boot_utc_s;
    static uint32_t monotonic_ms;
    static uint32_t pit_hz;
    static int32_t tz_minutes;
    static bool dst_on;
    static int32_t ntp_offset_s;
    static bool rtc_ok;
    static bool battery_ok;
    static bool allow_adjust;
    static uint64_t last_rtc_sync_us;
    static uint32_t to_unix_s(uint16_t year, uint8_t mon, uint8_t dom, uint8_t h, uint8_t m, uint8_t s);
    static DateTime breakdown_s(uint32_t secs);
};

// convenience alias  -  many modules use time::getticks()
struct Time {
    static inline uint32_t GetTicks() {
        return (uint32_t)(TimeManager::NowUTC().us / 1000u);
    }
};
