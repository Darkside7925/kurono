#pragma once
#include "../kernel/types.h"

class RTC {
public:
    struct Time { uint8_t h; uint8_t m; uint8_t s; };
    struct Date { uint8_t dow; uint8_t dom; uint8_t mon; uint16_t year; };
    static Time Read();
    static Date ReadDate();
    static bool BatteryOk();
    static bool ReadDateTime(Date& d, Time& t);
private:
    static uint8_t Bcd(uint8_t v);
    static void Out(uint16_t p, uint8_t v);
    static uint8_t In(uint16_t p);
    static uint8_t ReadReg(uint8_t idx);
};
