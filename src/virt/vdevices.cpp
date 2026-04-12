//  kurono os  -  virtual device emulation implementation
//  full emulation of pic, apic, pit, hpet for vm guests
#include "vdevices.h"
#include "../drivers/serial.h"

VirtualPIC  VirtualDevices::master_pic;
VirtualPIC  VirtualDevices::slave_pic;
VirtualAPIC VirtualDevices::apic;
VirtualPIT  VirtualDevices::pit;
VirtualHPET VirtualDevices::hpet;
bool        VirtualDevices::initialized = false;

//  virtual pic implementation (8259a)

void VirtualPIC::Reset() {
    icw1 = 0; icw2 = 0; icw3 = 0; icw4 = 0;
    icw_step = 0; init_mode = false;
    irr = 0; isr = 0; imr = 0xFF; // all masked initially
    vector_base = 0;
    priority = 0;
    auto_eoi = false;
    special_mask = false;
    poll_mode = false;
    rotate_on_eoi = false;
    elcr = 0;
}

void VirtualPIC::WritePort(uint16_t port, uint8_t value) {
    bool is_cmd = (port & 1) == 0; // even port = command, odd = data

    if (is_cmd) {
        if (value & 0x10) {
            // icw1  -  initialization sequence start
            icw1 = value;
            init_mode = true;
            icw_step = 1;
            // reset state
            irr = 0;
            isr = 0;
            imr = 0;
            auto_eoi = false;
            priority = 0;
            special_mask = false;
            rotate_on_eoi = false;
        } else if (value & 0x08) {
            // ocw3  -  read irr/isr, set special mask
            if (value & 0x02) {
                poll_mode = (value & 0x04) != 0;
            }
            if (value & 0x40) {
                special_mask = (value & 0x20) != 0;
            }
        } else {
            // ocw2  -  eoi commands
            uint8_t eoi_type = (value >> 5) & 0x07;
            switch (eoi_type) {
                case 1: // non-specific eoi
                {
                    int highest = GetHighestPriorityIRQ();
                    if (highest >= 0) {
                        isr &= ~(1 << highest);
                    }
                    break;
                }
                case 3: // specific eoi
                {
                    int irq = value & 0x07;
                    isr &= ~(1 << irq);
                    break;
                }
                case 5: // rotate on non-specific eoi
                {
                    int highest = GetHighestPriorityIRQ();
                    if (highest >= 0) {
                        isr &= ~(1 << highest);
                        priority = (highest + 1) & 7;
                    }
                    break;
                }
                case 7: // rotate on specific eoi
                {
                    int irq = value & 0x07;
                    isr &= ~(1 << irq);
                    priority = (irq + 1) & 7;
                    break;
                }
                case 0: // rotate in auto-eoi mode (clear)
                    rotate_on_eoi = false;
                    break;
                case 4: // rotate in auto-eoi mode (set)
                    rotate_on_eoi = true;
                    break;
                case 6: // set priority command
                    priority = (value & 0x07 + 1) & 7;
                    break;
                default:
                    break;
            }
        }
    } else {
        // data port
        if (init_mode) {
            switch (icw_step) {
                case 1: // icw2  -  vector base
                    icw2 = value;
                    vector_base = value & 0xF8;
                    icw_step = 2;
                    break;
                case 2: // icw3  -  cascade
                    icw3 = value;
                    cascade_mask = value;
                    if (icw1 & 0x01) { // icw4 needed?
                        icw_step = 3;
                    } else {
                        init_mode = false;
                        icw_step = 0;
                    }
                    break;
                case 3: // icw4  -  mode
                    icw4 = value;
                    auto_eoi = (value & 0x02) != 0;
                    init_mode = false;
                    icw_step = 0;
                    break;
            }
        } else {
            // ocw1  -  write imr
            imr = value;
        }
    }
}

uint8_t VirtualPIC::ReadPort(uint16_t port) {
    bool is_cmd = (port & 1) == 0;

    if (is_cmd) {
        // depends on last ocw3 command  -  default to irr
        if (poll_mode) {
            int irq = GetHighestPriorityIRQ();
            poll_mode = false;
            if (irq >= 0) return (uint8_t)(0x80 | irq);
            return 0;
        }
        return irr; // default: return irr
    } else {
        return imr; // data port reads imr
    }
}

int VirtualPIC::GetHighestPriorityIRQ() {
    uint8_t pending = irr & ~imr;
    if (!pending) return -1;

    // scan from highest priority (considering rotation)
    for (int i = 0; i < 8; i++) {
        int irq = (priority + i) & 7;
        if (pending & (1 << irq)) {
            // check if a higher-priority interrupt is in service
            if (!special_mask && (isr & (1 << irq))) continue;
            return irq;
        }
    }
    return -1;
}

void VirtualPIC::SetIRQ(int irq, bool level) {
    if (irq < 0 || irq > 7) return;
    uint8_t mask = (uint8_t)(1 << irq);
    if (level) {
        // edge-triggered (elcr bit = 0): set on rising edge only
        // level-triggered (elcr bit = 1): set while high
        if (elcr & mask) {
            irr |= mask; // level: set while asserted
        } else {
            if (!(irr & mask)) irr |= mask; // edge: only on 0→1
        }
    } else {
        if (elcr & mask) {
            irr &= ~mask; // level: deassert clears
        }
    }
}

void VirtualPIC::AcknowledgeIRQ(int irq) {
    if (irq < 0 || irq > 7) return;
    uint8_t mask = (uint8_t)(1 << irq);
    irr &= ~mask;
    isr |= mask;
    if (auto_eoi) {
        isr &= ~mask;
        if (rotate_on_eoi) priority = (irq + 1) & 7;
    }
}

//  virtual apic implementation

void VirtualAPIC::Reset() {
    for (int i = 0; i < VAPIC_REG_SIZE / 4; i++) regs[i] = 0;

    apic_id = 0;
    enabled = false;
    bsp = true;

    timer_initial = 0;
    timer_current = 0;
    timer_divide = 1;
    timer_mode = APIC_TIMER_ONE_SHOT;
    timer_vector = 0;
    timer_masked = true;
    timer_ticks = 0;

    for (int i = 0; i < 8; i++) {
        isr[i] = 0;
        irr[i] = 0;
        tmr[i] = 0;
    }

    // set version register: version 0x14, max lvt entry 5
    regs[APIC_VERSION / 4] = 0x00050014;

    // set dfr to flat model
    regs[APIC_DFR / 4] = 0xFFFFFFFF;

    // set svr with vector 0xff
    regs[APIC_SVR / 4] = 0x000000FF;
}

void VirtualAPIC::WriteReg(uint32_t offset, uint32_t value) {
    if (offset >= VAPIC_REG_SIZE) return;

    switch (offset) {
        case APIC_ID:
            apic_id = (value >> 24) & 0xFF;
            break;

        case APIC_TPR:
            regs[offset / 4] = value & 0xFF;
            break;

        case APIC_EOI:
            EOI();
            break;

        case APIC_LDR:
            regs[offset / 4] = value & 0xFF000000;
            break;

        case APIC_DFR:
            regs[offset / 4] = value | 0x0FFFFFFF;
            break;

        case APIC_SVR:
            regs[offset / 4] = value;
            enabled = (value & (1 << 8)) != 0;
            break;

        case APIC_ICR_LO:
            regs[APIC_ICR_LO / 4] = value;
            regs[APIC_ICR_HI / 4] = regs[APIC_ICR_HI / 4]; // already set
            SendIPI(value, regs[APIC_ICR_HI / 4]);
            break;

        case APIC_ICR_HI:
            regs[offset / 4] = value;
            break;

        case APIC_LVT_TIMER:
            regs[offset / 4] = value;
            timer_vector = value & 0xFF;
            timer_masked = (value & (1 << 16)) != 0;
            timer_mode = (value >> 17) & 0x03;
            break;

        case APIC_LVT_THERMAL:
        case APIC_LVT_PERFCNT:
        case APIC_LVT_LINT0:
        case APIC_LVT_LINT1:
        case APIC_LVT_ERROR:
            regs[offset / 4] = value;
            break;

        case APIC_TIMER_INIT:
            timer_initial = value;
            timer_current = value;
            timer_ticks = 0;
            regs[offset / 4] = value;
            break;

        case APIC_TIMER_DIVIDE:
        {
            regs[offset / 4] = value;
            // decode divide value: bits [3,1:0] encode divisor
            static const uint32_t div_table[] = {2, 4, 8, 16, 32, 64, 128, 1};
            uint8_t idx = ((value & 0x08) >> 1) | (value & 0x03);
            timer_divide = div_table[idx & 7];
            break;
        }

        case APIC_ESR:
            regs[offset / 4] = 0; // writing clears esr
            break;

        default:
            regs[offset / 4] = value;
            break;
    }
}

uint32_t VirtualAPIC::ReadReg(uint32_t offset) {
    if (offset >= VAPIC_REG_SIZE) return 0;

    switch (offset) {
        case APIC_ID:
            return (apic_id << 24);
        case APIC_TIMER_CURRENT:
            return timer_current;
        case APIC_ISR_BASE ... APIC_ISR_BASE + 0x70:
            return isr[(offset - APIC_ISR_BASE) / 0x10];
        case APIC_IRR_BASE ... APIC_IRR_BASE + 0x70:
            return irr[(offset - APIC_IRR_BASE) / 0x10];
        case APIC_TMR_BASE ... APIC_TMR_BASE + 0x70:
            return tmr[(offset - APIC_TMR_BASE) / 0x10];
        default:
            return regs[offset / 4];
    }
}

void VirtualAPIC::Tick(uint32_t elapsed_us) {
    if (!enabled || timer_initial == 0) return;

    // count down based on divide value
    // approximate: each "tick" decrements by some amount per microsecond
    uint32_t ticks = (elapsed_us * 1000) / timer_divide; // simplified
    if (ticks == 0) ticks = 1;

    if (timer_current > ticks) {
        timer_current -= ticks;
    } else {
        // timer fired!
        timer_ticks++;
        if (!timer_masked && timer_vector > 0) {
            InjectInterrupt(timer_vector);
        }

        if (timer_mode == APIC_TIMER_PERIODIC) {
            timer_current = timer_initial;
        } else {
            timer_current = 0;
        }
    }
}

void VirtualAPIC::SendIPI(uint32_t icr_lo, uint32_t icr_hi) {
    (void)icr_hi;
    uint8_t vector = icr_lo & 0xFF;
    uint8_t delivery = (icr_lo >> 8) & 0x07;
    uint8_t dest_mode = (icr_lo >> 11) & 0x01;
    uint8_t shorthand = (icr_lo >> 18) & 0x03;

    SerialLogger::Log("APIC: IPI vector=");
    SerialLogger::LogHex(vector);
    SerialLogger::Log(" delivery=");
    SerialLogger::LogDec(delivery);
    SerialLogger::Log(" shorthand=");
    SerialLogger::LogDec(shorthand);
    SerialLogger::Log(" dest_mode=");
    SerialLogger::LogDec(dest_mode);
    SerialLogger::Log("\r\n");

    // for single-cpu vm, shorthand=1 (self) is handled locally
    if (shorthand == 1) {
        InjectInterrupt(vector);
    }
    // delivery mode 5 = init, 6 = sipi  -  used for ap startup
}

void VirtualAPIC::InjectInterrupt(uint8_t vector) {
    int idx = vector / 32;
    int bit = vector % 32;
    irr[idx] |= (1u << bit);
}

int VirtualAPIC::GetPendingVector() {
    uint8_t tpr = regs[APIC_TPR / 4] & 0xFF;
    uint8_t tpr_class = (tpr >> 4) & 0x0F;

    // find highest priority pending interrupt
    for (int idx = 7; idx >= 0; idx--) {
        uint32_t pending = irr[idx] & ~isr[idx];
        if (!pending) continue;

        // find highest set bit
        for (int bit = 31; bit >= 0; bit--) {
            if (pending & (1u << bit)) {
                int vector = idx * 32 + bit;
                uint8_t vec_class = (vector >> 4) & 0x0F;
                if (vec_class > tpr_class) {
                    return vector;
                }
                return -1; // inhibited by tpr
            }
        }
    }
    return -1;
}

void VirtualAPIC::EOI() {
    // clear highest-priority isr bit
    for (int idx = 7; idx >= 0; idx--) {
        if (!isr[idx]) continue;
        for (int bit = 31; bit >= 0; bit--) {
            if (isr[idx] & (1u << bit)) {
                isr[idx] &= ~(1u << bit);
                return;
            }
        }
    }
}

//  virtual pit implementation (8254)

void VirtualPIT::Reset() {
    for (int i = 0; i < 3; i++) {
        channels[i].reload_value = 0;
        channels[i].counter = 0;
        channels[i].latch_value = 0;
        channels[i].mode = 3; // default: mode 3 (square wave)
        channels[i].access = 3; // lo-hi access
        channels[i].bcd = 0;
        channels[i].gate = true;
        channels[i].output = (i == 0); // channel 0 starts high
        channels[i].latched = false;
        channels[i].null_count = true;
        channels[i].flip_flop = false;
        channels[i].tick_accum = 0;
    }
    speaker_gate = false;
    irq0_count = 0;
}

void VirtualPIT::WritePort(uint16_t port, uint8_t value) {
    int ch = port - 0x40;

    if (ch == 3) {
        // command register (port 0x43)
        int sel = (value >> 6) & 0x03;

        if (sel == 3) {
            // read-back command
            // bits [3:1] = channels to read back
            return;
        }

        PITChannel& c = channels[sel];
        c.access = (value >> 4) & 0x03;
        c.mode = (value >> 1) & 0x07;
        c.bcd = value & 0x01;

        if (c.access == 0) {
            // latch command
            c.latch_value = c.counter;
            c.latched = true;
        } else {
            c.flip_flop = false;
            c.null_count = true;
        }
        return;
    }

    if (ch < 0 || ch > 2) return;
    PITChannel& c = channels[ch];

    switch (c.access) {
        case 1: // low byte only
            c.reload_value = (c.reload_value & 0xFF00) | value;
            c.null_count = false;
            break;
        case 2: // high byte only
            c.reload_value = (c.reload_value & 0x00FF) | ((uint16_t)value << 8);
            c.null_count = false;
            break;
        case 3: // low then high
            if (!c.flip_flop) {
                c.reload_value = (c.reload_value & 0xFF00) | value;
            } else {
                c.reload_value = (c.reload_value & 0x00FF) | ((uint16_t)value << 8);
                c.null_count = false;
            }
            c.flip_flop = !c.flip_flop;
            break;
    }

    // on full reload value written, load counter
    if (!c.null_count) {
        c.counter = c.reload_value;
        c.output = (c.mode == 0) ? false : true;
    }
}

uint8_t VirtualPIT::ReadPort(uint16_t port) {
    int ch = port - 0x40;
    if (ch < 0 || ch > 2) return 0;

    PITChannel& c = channels[ch];

    uint16_t val = c.latched ? c.latch_value : c.counter;

    uint8_t ret = 0;
    switch (c.access) {
        case 1: ret = (uint8_t)(val & 0xFF); break;
        case 2: ret = (uint8_t)((val >> 8) & 0xFF); break;
        case 3:
            if (!c.flip_flop)
                ret = (uint8_t)(val & 0xFF);
            else
                ret = (uint8_t)((val >> 8) & 0xFF);
            c.flip_flop = !c.flip_flop;
            break;
    }

    if (c.latched && ((c.access != 3) || c.flip_flop == false)) {
        c.latched = false;
    }

    return ret;
}

void VirtualPIT::Tick(uint32_t elapsed_us) {
    // convert elapsed microseconds to pit ticks
    // pit frequency = 1,193,182 hz ≈ 1.193 ticks per microsecond
    for (int ch = 0; ch < 3; ch++) {
        PITChannel& c = channels[ch];
        if (c.null_count || !c.gate) continue;
        if (c.reload_value == 0) continue;

        // accumulate sub-tick precision
        c.tick_accum += elapsed_us * 1193;
        uint32_t pit_ticks = c.tick_accum / 1000;
        c.tick_accum -= pit_ticks * 1000;

        if (pit_ticks == 0) continue;

        switch (c.mode) {
            case 0: // interrupt on terminal count
                if (c.counter > pit_ticks) {
                    c.counter -= pit_ticks;
                } else {
                    c.counter = 0;
                    if (!c.output) {
                        c.output = true;
                        if (ch == 0) irq0_count++;
                    }
                }
                break;

            case 2: // rate generator
            case 6:
                while (pit_ticks > 0) {
                    if (c.counter > 1) {
                        uint32_t dec = (pit_ticks < c.counter - 1) ? pit_ticks : (c.counter - 1);
                        c.counter -= dec;
                        pit_ticks -= dec;
                    } else {
                        c.counter = c.reload_value;
                        if (ch == 0) irq0_count++;
                        pit_ticks--;
                    }
                }
                break;

            case 3: // square wave generator
            case 7:
            {
                uint16_t half = c.reload_value / 2;
                while (pit_ticks > 0) {
                    if (c.counter > 1) {
                        uint32_t dec = (pit_ticks < c.counter - 1) ? pit_ticks : (c.counter - 1);
                        c.counter -= dec;
                        pit_ticks -= dec;
                    } else {
                        c.output = !c.output;
                        c.counter = half;
                        if (ch == 0 && c.output) irq0_count++;
                        pit_ticks--;
                    }
                }
                break;
            }

            default:
                // modes 1, 4, 5  -  simplified
                if (c.counter > pit_ticks) {
                    c.counter -= pit_ticks;
                } else {
                    c.counter = c.reload_value;
                    if (ch == 0) irq0_count++;
                }
                break;
        }
    }
}

bool VirtualPIT::Channel0Fired() {
    if (irq0_count > 0) {
        irq0_count--;
        return true;
    }
    return false;
}

uint32_t VirtualPIT::GetFrequencyHz(int channel) {
    if (channel < 0 || channel > 2) return 0;
    uint16_t reload = channels[channel].reload_value;
    if (reload == 0) return PIT_FREQ_HZ; // 0 = 65536
    return PIT_FREQ_HZ / reload;
}

//  virtual hpet implementation

void VirtualHPET::Reset() {
    rev_id = 0x01;
    num_timers = 3;
    counter_size = 1; // 64-bit
    vendor_id = 0x4B52; // "kr" for kurono
    period_fs = 100000000; // 100ns period = 10mhz counter

    enabled = false;
    legacy_route = false;
    counter = 0;
    counter_offset = 0;
    int_status = 0;

    for (int i = 0; i < 3; i++) {
        timers[i].config = HPET_TN_PERIODIC_CAP | HPET_TN_SIZE_CAP;
        timers[i].comparator = 0;
        timers[i].fsb_route = 0;
        timers[i].int_pending = false;
    }
}

void VirtualHPET::WriteReg(uint32_t offset, uint32_t value) {
    switch (offset) {
        case HPET_CONFIG:
            enabled = (value & 0x01) != 0;
            legacy_route = (value & 0x02) != 0;
            if (!enabled) {
                // save counter offset for when re-enabled
                counter_offset = counter;
            }
            break;

        case HPET_INT_STATUS:
            // writing 1 clears bits
            int_status &= ~value;
            break;

        case HPET_COUNTER:
            if (!enabled) {
                counter = value;
            }
            break;

        case HPET_TIMER0_CONFIG:
            timers[0].config = value;
            break;
        case HPET_TIMER0_COMP:
            timers[0].comparator = value;
            if (timers[0].config & HPET_TN_VAL_SET) {
                // set accumulator for periodic mode
            }
            break;
        case HPET_TIMER0_FSB:
            timers[0].fsb_route = value;
            break;

        case HPET_TIMER1_CONFIG:
            timers[1].config = value;
            break;
        case HPET_TIMER1_COMP:
            timers[1].comparator = value;
            break;

        case HPET_TIMER2_CONFIG:
            timers[2].config = value;
            break;
        case HPET_TIMER2_COMP:
            timers[2].comparator = value;
            break;

        default:
            break;
    }
}

uint32_t VirtualHPET::ReadReg(uint32_t offset) {
    switch (offset) {
        case HPET_CAP_ID:
        {
            // bits [31:16] = period in femtoseconds
            // bits [15:13] = vendor id (high bits)
            // bit [13] = counter size (64-bit capable)
            // bits [12:8] = num_timers - 1
            // bits [7:0] = revision
            uint32_t cap = period_fs << 16;
            cap |= (counter_size ? (1 << 13) : 0);
            cap |= ((num_timers - 1) << 8);
            cap |= rev_id;
            return cap;
        }

        case HPET_CONFIG:
            return (enabled ? 1 : 0) | (legacy_route ? 2 : 0);

        case HPET_INT_STATUS:
            return int_status;

        case HPET_COUNTER:
            return (uint32_t)counter;

        case HPET_TIMER0_CONFIG: return (uint32_t)timers[0].config;
        case HPET_TIMER0_COMP:   return (uint32_t)timers[0].comparator;
        case HPET_TIMER1_CONFIG: return (uint32_t)timers[1].config;
        case HPET_TIMER1_COMP:   return (uint32_t)timers[1].comparator;
        case HPET_TIMER2_CONFIG: return (uint32_t)timers[2].config;
        case HPET_TIMER2_COMP:   return (uint32_t)timers[2].comparator;

        default:
            return 0;
    }
}

void VirtualHPET::Tick(uint32_t elapsed_us) {
    if (!enabled) return;

    // advance main counter: period_fs femtoseconds per tick
    // 100ns period = 10mhz → 10 ticks per microsecond
    uint64_t ticks = (uint64_t)elapsed_us * 10; // 10 ticks per us at 10mhz
    counter += ticks;

    // check comparators
    for (int i = 0; i < (int)num_timers; i++) {
        if (!(timers[i].config & HPET_TN_INT_ENABLE)) continue;

        if (counter >= timers[i].comparator && timers[i].comparator != 0) {
            // timer fired
            int_status |= (1u << i);
            timers[i].int_pending = true;

            if (timers[i].config & HPET_TN_PERIODIC) {
                // periodic: add comparator to itself
                timers[i].comparator += timers[i].comparator;
            } else {
                // one-shot: disable
                timers[i].config &= ~HPET_TN_INT_ENABLE;
            }
        }
    }
}

int VirtualHPET::CheckFiredTimers() {
    int mask = 0;
    for (int i = 0; i < (int)num_timers; i++) {
        if (timers[i].int_pending) {
            mask |= (1 << i);
            timers[i].int_pending = false;
        }
    }
    return mask;
}

//  virtual device manager

void VirtualDevices::Init() {
    if (initialized) return;
    initialized = true;

    master_pic.Reset();
    master_pic.is_master = true;
    slave_pic.Reset();
    slave_pic.is_master = false;

    apic.Reset();
    pit.Reset();
    hpet.Reset();

    SerialLogger::Log("VDevices: All virtual devices initialized\r\n");
    SerialLogger::Log("  PIC: dual 8259A (master + slave)\r\n");
    SerialLogger::Log("  APIC: xAPIC at 0xFEE00000\r\n");
    SerialLogger::Log("  PIT: 8254 (3 channels)\r\n");
    SerialLogger::Log("  HPET: 3 timers at 0xFED00000\r\n");
}

bool VirtualDevices::HandlePortIO(uint16_t port, bool is_out, uint8_t size,
                                   uint32_t& value) {
    (void)size;

    // pic master (0x20-0x21)
    if (port >= 0x20 && port <= 0x21) {
        if (is_out) master_pic.WritePort(port, (uint8_t)value);
        else value = master_pic.ReadPort(port);
        return true;
    }

    // pic slave (0xa0-0xa1)
    if (port >= 0xA0 && port <= 0xA1) {
        if (is_out) slave_pic.WritePort(port, (uint8_t)value);
        else value = slave_pic.ReadPort(port);
        return true;
    }

    // pit (0x40-0x43)
    if (port >= 0x40 && port <= 0x43) {
        if (is_out) pit.WritePort(port, (uint8_t)value);
        else value = pit.ReadPort(port);
        return true;
    }

    // elcr (edge/level control register)  -  ports 0x4d0-0x4d1
    if (port == 0x4D0) {
        if (is_out) master_pic.elcr = (uint8_t)value;
        else value = master_pic.elcr;
        return true;
    }
    if (port == 0x4D1) {
        if (is_out) slave_pic.elcr = (uint8_t)value;
        else value = slave_pic.elcr;
        return true;
    }

    // port 0x61  -  system control port (pit channel 2 gate, speaker)
    if (port == 0x61) {
        if (is_out) {
            pit.channels[2].gate = (value & 0x01) != 0;
            pit.speaker_gate = (value & 0x02) != 0;
        } else {
            value = (pit.channels[2].gate ? 1 : 0) |
                    (pit.speaker_gate ? 2 : 0) |
                    (pit.channels[2].output ? 0x20 : 0);
        }
        return true;
    }

    return false; // not handled
}

bool VirtualDevices::HandleMMIO(uint64_t phys_addr, bool is_write,
                                 uint8_t size, uint32_t& value) {
    (void)size;

    // apic  -  0xfee00000 to 0xfee00fff
    if (phys_addr >= VAPIC_BASE_ADDR && phys_addr < VAPIC_BASE_ADDR + 0x1000) {
        uint32_t offset = (uint32_t)(phys_addr - VAPIC_BASE_ADDR);
        offset &= ~0x0F; // align to 16 bytes
        if (is_write) apic.WriteReg(offset, value);
        else value = apic.ReadReg(offset);
        return true;
    }

    // hpet  -  0xfed00000 to 0xfed003ff
    if (phys_addr >= HPET_BASE_ADDR && phys_addr < HPET_BASE_ADDR + HPET_REG_SIZE) {
        uint32_t offset = (uint32_t)(phys_addr - HPET_BASE_ADDR);
        if (is_write) hpet.WriteReg(offset, value);
        else value = hpet.ReadReg(offset);
        return true;
    }

    return false;
}

void VirtualDevices::Tick(uint32_t elapsed_us) {
    pit.Tick(elapsed_us);
    apic.Tick(elapsed_us);
    hpet.Tick(elapsed_us);

    // route pit channel 0 → pic irq0 (or apic via legacy routing)
    if (pit.Channel0Fired()) {
        if (hpet.legacy_route) {
            // hpet legacy mode: pit is replaced, use hpet timer 0 → irq 0
        } else {
            master_pic.SetIRQ(0, true);
            master_pic.SetIRQ(0, false); // edge trigger
        }
    }
}

int VirtualDevices::GetPendingIRQ() {
    // check pic first (if apic is disabled or in virtual wire mode)
    int pic_irq = master_pic.GetHighestPriorityIRQ();
    if (pic_irq >= 0) {
        // check cascade: irq 2 = slave
        if (pic_irq == 2) {
            int slave_irq = slave_pic.GetHighestPriorityIRQ();
            if (slave_irq >= 0) {
                slave_pic.AcknowledgeIRQ(slave_irq);
                master_pic.AcknowledgeIRQ(2);
                return 8 + slave_irq; // slave irq 0-7 → system irq 8-15
            }
        }
        master_pic.AcknowledgeIRQ(pic_irq);
        return master_pic.vector_base + pic_irq;
    }

    // check apic
    int apic_vector = apic.GetPendingVector();
    if (apic_vector >= 0) return apic_vector;

    return -1;
}

VirtualPIC&  VirtualDevices::GetMasterPIC() { return master_pic; }
VirtualPIC&  VirtualDevices::GetSlavePIC()  { return slave_pic; }
VirtualAPIC& VirtualDevices::GetAPIC()      { return apic; }
VirtualPIT&  VirtualDevices::GetPIT()       { return pit; }
VirtualHPET& VirtualDevices::GetHPET()      { return hpet; }

void VirtualDevices::DumpState() {
    SerialLogger::Log("=== Virtual Device State ===\r\n");

    SerialLogger::Log("PIC Master: IRR=");
    SerialLogger::LogHex(master_pic.irr);
    SerialLogger::Log(" ISR=");
    SerialLogger::LogHex(master_pic.isr);
    SerialLogger::Log(" IMR=");
    SerialLogger::LogHex(master_pic.imr);
    SerialLogger::Log(" base=");
    SerialLogger::LogHex(master_pic.vector_base);
    SerialLogger::Log("\r\n");

    SerialLogger::Log("PIC Slave:  IRR=");
    SerialLogger::LogHex(slave_pic.irr);
    SerialLogger::Log(" ISR=");
    SerialLogger::LogHex(slave_pic.isr);
    SerialLogger::Log(" IMR=");
    SerialLogger::LogHex(slave_pic.imr);
    SerialLogger::Log(" base=");
    SerialLogger::LogHex(slave_pic.vector_base);
    SerialLogger::Log("\r\n");

    SerialLogger::Log("APIC: enabled=");
    SerialLogger::Log(apic.enabled ? "yes" : "no");
    SerialLogger::Log(" timer_init=");
    SerialLogger::LogDec(apic.timer_initial);
    SerialLogger::Log(" timer_cur=");
    SerialLogger::LogDec(apic.timer_current);
    SerialLogger::Log(" timer_vec=");
    SerialLogger::LogDec(apic.timer_vector);
    SerialLogger::Log("\r\n");

    SerialLogger::Log("PIT Ch0: reload=");
    SerialLogger::LogDec(pit.channels[0].reload_value);
    SerialLogger::Log(" counter=");
    SerialLogger::LogDec(pit.channels[0].counter);
    SerialLogger::Log(" mode=");
    SerialLogger::LogDec(pit.channels[0].mode);
    SerialLogger::Log(" freq=");
    SerialLogger::LogDec(pit.GetFrequencyHz(0));
    SerialLogger::Log("Hz\r\n");

    SerialLogger::Log("HPET: enabled=");
    SerialLogger::Log(hpet.enabled ? "yes" : "no");
    SerialLogger::Log(" counter=");
    SerialLogger::LogHex((uint32_t)(hpet.counter >> 32));
    SerialLogger::LogHex((uint32_t)hpet.counter);
    SerialLogger::Log(" legacy=");
    SerialLogger::Log(hpet.legacy_route ? "yes" : "no");
    SerialLogger::Log("\r\n");
}
