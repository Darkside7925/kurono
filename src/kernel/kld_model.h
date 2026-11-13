// KLD model device: a synthetic pci display adapter for no-hardware rigs.
//
// kurono.kld.model=i915[:edp|:tc|:none] (default off) adds ONE device to the
// broker's scan table at bdf fe:00.0 with the ids, config space and BAR sizes
// of the laptop's iGPU (8086:7d67). its BARs are backed by kernel-owned RAM
// frames instead of a device, and config 0xFC (ASLS) points at a synthetic
// OpRegion holding a synthetic VBT. register BEHAVIOUR (power wells, aux,
// pll acks) is modelled inside the container (kld/drm/i915_model.c); this
// side only provides what the container must not invent itself: the device,
// its config space, its memory and its firmware tables. the broker treats the
// model like any granted device, so every broker policy (bar map, read-only
// and write-window bars, the opregion op) runs the same code as on hardware.
// (satoru)

#pragma once

#include "types.h"

namespace KldModel {

// parse kurono.kld.model=. call before the broker's first pci scan. (satoru)
void BootInit(const char* boot_cmdline);
bool Enabled();
// "edp", "tc" or "none": which synthetic VBT the opregion carries. (satoru)
const char* Variant();

// the model's bdf (fe:00.0) and its scan-table identity. (satoru)
uint32_t Bdf();
bool IsModel(uint32_t bdf);
void Identity(uint16_t* vendor, uint16_t* device, uint16_t* subvendor,
              uint16_t* subdevice, uint32_t* class_code);

// config space of the model device. writes to the bar registers are refused
// by the broker before they get here. (satoru)
uint32_t CfgRead(uint32_t off, uint32_t width);
void CfgWrite(uint32_t off, uint32_t width, uint32_t val);

// the model's host bridge (00:00.0) config, read-only. (satoru)
uint32_t HostCfgRead(uint32_t off, uint32_t width);

// bar sizing without the all-ones write protocol. (satoru)
uint64_t BarSize(uint32_t idx);
// the backing frame of one bar page, allocated and zeroed on first use; 0 for
// a page the model leaves unbacked (never mapped). (satoru)
uint64_t BarPagePhys(uint32_t idx, uint64_t page);

// before each container start: zero the device state (bar pages, config).
// (satoru)
void ResetForContainer();

// the synthetic opregion: true when [phys, phys+bytes) lies inside it. (satoru)
bool OwnsOpRegion(uint64_t phys, uint64_t bytes);

}  // namespace KldModel

// end (satoru)
