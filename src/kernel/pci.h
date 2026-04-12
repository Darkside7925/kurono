#pragma once
//  kurono os  -  pci configuration space access
//  standard x86 pci via i/o ports 0xcf8 / 0xcfc
#include "types.h"
#include "io.h"

#define PCI_CONFIG_ADDR  0xCF8
#define PCI_CONFIG_DATA  0xCFC

class PCI {
public:
    static inline uint32_t MakeAddr(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
        return (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) |
               ((uint32_t)func << 8) | (offset & 0xFC);
    }

    static inline uint32_t Read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
        outl(PCI_CONFIG_ADDR, MakeAddr(bus, dev, func, offset));
        return inl(PCI_CONFIG_DATA);
    }

    static inline void Write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint32_t val) {
        outl(PCI_CONFIG_ADDR, MakeAddr(bus, dev, func, offset));
        outl(PCI_CONFIG_DATA, val);
    }

    static inline uint16_t Read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
        uint32_t val = Read32(bus, dev, func, offset & 0xFC);
        return (val >> ((offset & 2) * 8)) & 0xFFFF;
    }

    static inline uint8_t Read8(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
        uint32_t val = Read32(bus, dev, func, offset & 0xFC);
        return (val >> ((offset & 3) * 8)) & 0xFF;
    }

    static inline uint16_t GetVendor(uint8_t bus, uint8_t dev, uint8_t func) {
        return Read16(bus, dev, func, 0x00);
    }

    static inline uint16_t GetDevice(uint8_t bus, uint8_t dev, uint8_t func) {
        return Read16(bus, dev, func, 0x02);
    }

    static inline uint32_t GetClassCode(uint8_t bus, uint8_t dev, uint8_t func) {
        return Read32(bus, dev, func, 0x08) >> 8; // class:subclass:progif
    }

    static inline uint32_t GetBAR(uint8_t bus, uint8_t dev, uint8_t func, int bar) {
        return Read32(bus, dev, func, 0x10 + bar * 4);
    }

    static inline void EnableBusMaster(uint8_t bus, uint8_t dev, uint8_t func) {
        uint16_t cmd = Read16(bus, dev, func, 0x04);
        cmd |= (1 << 2) | (1 << 1); // bus master + memory space
        Write32(bus, dev, func, 0x04, cmd);
    }
};
