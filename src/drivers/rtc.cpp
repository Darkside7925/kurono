#include "rtc.h"

RTC::Time RTC::Read() {
    for (int g = 0; (ReadReg(0x0A) & 0x80) && g < 100000; g++) { }  // bounded: a stuck UIP bit must not hang under the input lock (satoru)
    uint8_t sec = ReadReg(0x00);
    uint8_t min = ReadReg(0x02);
    uint8_t hour = ReadReg(0x04);
    uint8_t b = ReadReg(0x0B);
    bool bcd = ((b & 0x04) == 0);
    bool h24 = ((b & 0x02) != 0);
    if (bcd) { sec = Bcd(sec); min = Bcd(min); hour = Bcd(hour); }
    if (!h24) {
        bool pm = (hour & 0x80) != 0;
        hour &= 0x7F;
        if (hour == 12) hour = pm ? 12 : 0; else if (pm) hour = (uint8_t)(hour + 12);
    }
    Time t; t.h = hour; t.m = min; t.s = sec; return t;
}

RTC::Date RTC::ReadDate() {
    for (int g = 0; (ReadReg(0x0A) & 0x80) && g < 100000; g++) { }  // bounded: a stuck UIP bit must not hang under the input lock (satoru)
    uint8_t dow = ReadReg(0x06);
    uint8_t dom = ReadReg(0x07);
    uint8_t mon = ReadReg(0x08);
    uint8_t yr  = ReadReg(0x09);
    uint8_t cen = ReadReg(0x32);
    uint8_t b = ReadReg(0x0B);
    bool bcd = ((b & 0x04) == 0);
    if (bcd) { dow = Bcd(dow); dom = Bcd(dom); mon = Bcd(mon); yr = Bcd(yr); cen = Bcd(cen); }
    uint16_t year = (cen ? ((uint16_t)cen * 100) : 2000) + (uint16_t)yr;
    Date d; d.dow = dow; d.dom = dom; d.mon = mon; d.year = year; return d;
}

bool RTC::BatteryOk() {
    uint8_t d = ReadReg(0x0D);
    return (d & 0x80) != 0;
}

bool RTC::ReadDateTime(Date& d, Time& t) {
    int timeout = 1000000;
    while ((ReadReg(0x0A) & 0x80) && --timeout > 0) { }
    if (timeout <= 0) return false;
    t = Read();
    d = ReadDate();
    return true;
}

uint8_t RTC::Bcd(uint8_t v) { return (uint8_t)((v & 0x0F) + ((v >> 4) * 10)); }
void RTC::Out(uint16_t p, uint8_t v) { __asm__ __volatile__("outb %0, %1" : : "a"(v), "Nd"(p)); }
uint8_t RTC::In(uint16_t p) { uint8_t r; __asm__ __volatile__("inb %1, %0" : "=a"(r) : "Nd"(p)); return r; }
uint8_t RTC::ReadReg(uint8_t idx) { Out(0x70, idx); return In(0x71); }
