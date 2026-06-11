// ═══════════════════════════════════════════════════════════════════════════
//  Kurono OS — Virtual Serial Port (8250/16550A UART) Implementation
//  Full register-accurate emulation for guest VM serial console.
//
//  Features implemented:
//    - Full 8250/16450/16550A register set
//    - 16-byte TX/RX FIFOs with configurable trigger levels
//    - Interrupt generation (RX available, TX empty, line status, modem)
//    - DLAB mode for baud rate divisor configuration
//    - Loopback mode (MCR bit 4)
//    - Character timeout indication (16550)
//    - Guest output capture buffer for host-side logging
//    - RX injection from host for input to guest
//
//  Reference: National Semiconductor PC16550D datasheet
//  Reference: OSDev Wiki - Serial Ports
// ═══════════════════════════════════════════════════════════════════════════
#include "vserial.h"
#include "../drivers/serial.h"
#include "../kernel/types.h"

// ═══════════════════════════════════════════════════════════════════════════
//  Reset — set all registers to power-on defaults
// ═══════════════════════════════════════════════════════════════════════════

void VirtualSerial::Reset() {
    base_port    = 0;
    irq          = 0;

    ier          = 0x00;
    iir          = IIR_NO_INT; // No interrupt pending
    fcr          = 0x00;
    lcr          = 0x00;       // 5-bit, 1 stop, no parity
    mcr          = 0x00;
    lsr          = LSR_THRE | LSR_TEMT; // TX empty on reset
    msr          = 0x00;
    scr          = 0x00;
    divisor      = 12;         // 9600 baud (115200 / 12)
    thr          = 0x00;

    rx_fifo.Reset();
    tx_fifo.Reset();
    fifo_enabled = false;

    irq_pending  = false;
    thre_pending = true;       // THR is empty on reset
    char_timeout_counter = 0;
    char_timeout_pending = false;

    output_head  = 0;
    output_tail  = 0;
    output_count = 0;
    memset(output_buf, 0, UART_OUTPUT_BUF_SIZE);
}

void VirtualSerial::Init(uint16_t port_base, uint8_t irq_num) {
    Reset();
    base_port = port_base;
    irq       = irq_num;

    // On init, modem status = CTS + DSR + DCD (connected)
    msr = MSR_CTS | MSR_DSR | MSR_DCD;

    SerialLogger::Log("VSerial: Initialized COM at 0x");
    SerialLogger::LogHex(base_port);
    SerialLogger::Log(" IRQ ");
    SerialLogger::LogDec(irq);
    SerialLogger::Log("\r\n");
}

// ═══════════════════════════════════════════════════════════════════════════
//  WritePort — guest writes to a UART register
// ═══════════════════════════════════════════════════════════════════════════

void VirtualSerial::WritePort(uint16_t port, uint8_t value) {
    uint16_t offset = port - base_port;

    switch (offset) {
        case UART_THR: // Also UART_DLL when DLAB=1
            if (lcr & LCR_DLAB) {
                // Divisor Latch Low
                divisor = (divisor & 0xFF00) | value;
            } else {
                // Transmit Holding Register
                if (mcr & MCR_LOOP) {
                    // Loopback mode: TX data goes directly to RX
                    if (fifo_enabled) {
                        if (!rx_fifo.IsFull()) {
                            rx_fifo.Push(value);
                        } else {
                            lsr |= LSR_OE; // Overrun
                        }
                    }
                    lsr |= LSR_DR; // Data ready
                } else {
                    TransmitByte(value);
                }
                // THR is now empty again (instant transmit in emulation)
                lsr |= LSR_THRE | LSR_TEMT;
                thre_pending = true;
            }
            break;

        case UART_IER: // Also UART_DLH when DLAB=1
            if (lcr & LCR_DLAB) {
                // Divisor Latch High
                divisor = (divisor & 0x00FF) | ((uint16_t)value << 8);
            } else {
                // Interrupt Enable Register
                ier = value & 0x0F; // Only lower 4 bits
                // Re-evaluate pending interrupts
                if (ier & IER_TX_EMPTY) {
                    // If THR is empty, THRE interrupt fires
                    if (lsr & LSR_THRE) {
                        thre_pending = true;
                    }
                }
            }
            break;

        case UART_FCR:
            fcr = value;
            fifo_enabled = (value & FCR_FIFO_EN) != 0;

            if (value & FCR_RX_RESET) {
                rx_fifo.Reset();
                char_timeout_counter = 0;
                char_timeout_pending = false;
            }
            if (value & FCR_TX_RESET) {
                tx_fifo.Reset();
            }
            SetFIFOTrigger(value);
            break;

        case UART_LCR:
            lcr = value;
            break;

        case UART_MCR:
            mcr = value & 0x1F; // Only lower 5 bits
            // Update MSR based on MCR in loopback mode
            if (mcr & MCR_LOOP) {
                UpdateMSR();
            }
            break;

        case UART_LSR:
            // LSR is read-only, ignore writes (some software writes here)
            break;

        case UART_MSR:
            // MSR is read-only, ignore writes
            break;

        case UART_SCR:
            scr = value;
            break;

        default:
            break;
    }

    UpdateIIR();
}

// ═══════════════════════════════════════════════════════════════════════════
//  ReadPort — guest reads a UART register
// ═══════════════════════════════════════════════════════════════════════════

uint8_t VirtualSerial::ReadPort(uint16_t port) {
    uint16_t offset = port - base_port;

    switch (offset) {
        case UART_RBR: // Also UART_DLL when DLAB=1
            if (lcr & LCR_DLAB) {
                return (uint8_t)(divisor & 0xFF);
            } else {
                // Read from receive buffer / FIFO
                uint8_t data = 0;
                if (fifo_enabled) {
                    data = rx_fifo.Pop();
                    if (rx_fifo.IsEmpty()) {
                        lsr &= ~LSR_DR;
                    }
                    char_timeout_counter = 0;
                    char_timeout_pending = false;
                } else {
                    data = rx_fifo.Pop();
                    lsr &= ~LSR_DR;
                }
                UpdateIIR();
                return data;
            }

        case UART_IER: // Also UART_DLH when DLAB=1
            if (lcr & LCR_DLAB) {
                return (uint8_t)((divisor >> 8) & 0xFF);
            }
            return ier;

        case UART_IIR:
        {
            uint8_t val = iir;
            if (fifo_enabled) {
                val |= IIR_FIFO_EN; // Show FIFOs enabled
            }
            // Reading IIR with THRE clears THRE interrupt
            if ((iir & IIR_ID_MASK) == IIR_TX_EMPTY) {
                thre_pending = false;
                UpdateIIR();
            }
            return val;
        }

        case UART_LCR:
            return lcr;

        case UART_MCR:
            return mcr;

        case UART_LSR:
        {
            uint8_t val = lsr;
            // Reading LSR clears error bits (OE, PE, FE, BI)
            lsr &= ~(LSR_OE | LSR_PE | LSR_FE | LSR_BI | LSR_FIFO_ERR);
            UpdateIIR();
            return val;
        }

        case UART_MSR:
        {
            uint8_t val = msr;
            // Reading MSR clears delta bits
            msr &= ~(MSR_DCTS | MSR_DDSR | MSR_TERI | MSR_DDCD);
            UpdateIIR();
            return val;
        }

        case UART_SCR:
            return scr;

        default:
            return 0xFF;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  TransmitByte — handle guest TX output
// ═══════════════════════════════════════════════════════════════════════════

void VirtualSerial::TransmitByte(uint8_t byte) {
    // Capture to output buffer for host-side reading
    OutputCapture(byte);

    // Also log to host serial for real-time monitoring
    // (This goes to the real host COM port / QEMU serial)
    char c[2] = { (char)byte, 0 };
    SerialLogger::Log(c);
}

void VirtualSerial::OutputCapture(uint8_t byte) {
    if (output_count >= UART_OUTPUT_BUF_SIZE) {
        // Buffer full — drop oldest byte
        output_head = (output_head + 1) % UART_OUTPUT_BUF_SIZE;
        output_count--;
    }
    output_buf[output_tail] = (char)byte;
    output_tail = (output_tail + 1) % UART_OUTPUT_BUF_SIZE;
    output_count++;
}

// ═══════════════════════════════════════════════════════════════════════════
//  External RX Injection — push data from host into guest
// ═══════════════════════════════════════════════════════════════════════════

void VirtualSerial::InjectRxByte(uint8_t byte) {
    if (fifo_enabled) {
        if (!rx_fifo.IsFull()) {
            rx_fifo.Push(byte);
            lsr |= LSR_DR;
            char_timeout_counter = 0;
        } else {
            lsr |= LSR_OE; // Overrun error
        }
    } else {
        // Non-FIFO: only 1-byte buffer
        if (lsr & LSR_DR) {
            lsr |= LSR_OE; // Previous byte not read → overrun
        }
        rx_fifo.Reset();
        rx_fifo.Push(byte);
        lsr |= LSR_DR;
    }
    UpdateIIR();
}

void VirtualSerial::InjectRxString(const char* str) {
    while (*str) {
        InjectRxByte((uint8_t)*str);
        str++;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Output Buffer Read — host reads guest TX output
// ═══════════════════════════════════════════════════════════════════════════

int VirtualSerial::ReadOutput(char* buf, int max_len) {
    int read = 0;
    while (read < max_len && output_count > 0) {
        buf[read++] = output_buf[output_head];
        output_head = (output_head + 1) % UART_OUTPUT_BUF_SIZE;
        output_count--;
    }
    return read;
}

bool VirtualSerial::HasOutput() const {
    return output_count > 0;
}

int VirtualSerial::GetOutputCount() const {
    return output_count;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Interrupt Management
// ═══════════════════════════════════════════════════════════════════════════

void VirtualSerial::UpdateIIR() {
    // Priority order (highest first):
    //   1. Receiver line status (OE/PE/FE/BI)
    //   2. Received data available / character timeout
    //   3. Transmitter holding register empty
    //   4. Modem status

    // Check receiver line status
    if ((ier & IER_LINE_STS) && (lsr & (LSR_OE | LSR_PE | LSR_FE | LSR_BI))) {
        iir = IIR_LINE_STS;
        irq_pending = true;
        return;
    }

    // Check received data available
    if (ier & IER_RX_AVAIL) {
        if (fifo_enabled) {
            if (rx_fifo.AtTrigger()) {
                iir = IIR_RX_AVAIL;
                irq_pending = true;
                return;
            }
            if (char_timeout_pending) {
                iir = IIR_CHAR_TMO;
                irq_pending = true;
                return;
            }
        } else {
            if (lsr & LSR_DR) {
                iir = IIR_RX_AVAIL;
                irq_pending = true;
                return;
            }
        }
    }

    // Check transmitter empty
    if ((ier & IER_TX_EMPTY) && thre_pending) {
        iir = IIR_TX_EMPTY;
        irq_pending = true;
        return;
    }

    // Check modem status
    if ((ier & IER_MODEM_STS) && (msr & 0x0F)) {
        iir = IIR_MODEM_STS;
        irq_pending = true;
        return;
    }

    // No interrupt
    iir = IIR_NO_INT;
    irq_pending = false;
}

void VirtualSerial::UpdateLSR() {
    // DR bit: data ready in RX buffer/FIFO
    if (fifo_enabled) {
        if (!rx_fifo.IsEmpty()) lsr |= LSR_DR;
        else lsr &= ~LSR_DR;
    }
    // THRE and TEMT are managed during TX
}

void VirtualSerial::UpdateMSR() {
    if (mcr & MCR_LOOP) {
        // In loopback, MCR outputs are reflected to MSR inputs
        uint8_t old_msr = msr & 0xF0;
        uint8_t new_inputs = 0;
        if (mcr & MCR_RTS)  new_inputs |= MSR_CTS;
        if (mcr & MCR_DTR)  new_inputs |= MSR_DSR;
        if (mcr & MCR_OUT1) new_inputs |= MSR_RI;
        if (mcr & MCR_OUT2) new_inputs |= MSR_DCD;

        // Compute delta bits
        uint8_t changes = old_msr ^ new_inputs;
        msr = new_inputs; // Set input states
        if (changes & MSR_CTS)  msr |= MSR_DCTS;
        if (changes & MSR_DSR)  msr |= MSR_DDSR;
        if (changes & MSR_RI)   msr |= MSR_TERI;
        if (changes & MSR_DCD)  msr |= MSR_DDCD;
    } else {
        // Not loopback: always show connected
        msr = MSR_CTS | MSR_DSR | MSR_DCD;
    }
}

bool VirtualSerial::HasPendingIRQ() const {
    // IRQ only fires if MCR.OUT2 is set (PC standard: OUT2 gates IRQ)
    return irq_pending && (mcr & MCR_OUT2);
}

void VirtualSerial::ClearIRQ() {
    irq_pending = false;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Tick — advance character timeout timer
// ═══════════════════════════════════════════════════════════════════════════

void VirtualSerial::Tick(uint32_t elapsed_us) {
    if (!fifo_enabled || rx_fifo.IsEmpty()) {
        char_timeout_counter = 0;
        char_timeout_pending = false;
        return;
    }

    // Character timeout: triggers when FIFO has data but level < trigger,
    // and no new data received for 4 character times.
    // At 115200 baud, 8N1: 1 char ~= 87us → 4 chars ~= 348us
    uint32_t char_time_us = 1;
    if (divisor > 0) {
        // Baud rate = 115200 / divisor
        // Character time = (bits_per_char / baud_rate) * 1e6 us
        uint32_t bits = 1 + ((lcr & LCR_WORD_MASK) + 5) + // start + data
                        ((lcr & LCR_STOP_2) ? 2 : 1) +     // stop bits
                        ((lcr & LCR_PARITY_EN) ? 1 : 0);    // parity
        uint32_t baud = 115200 / divisor;
        if (baud > 0) char_time_us = (bits * 1000000) / baud;
        if (char_time_us == 0) char_time_us = 1;
    }

    uint32_t timeout_threshold = char_time_us * 4;
    char_timeout_counter += elapsed_us;

    if (char_timeout_counter >= timeout_threshold && !rx_fifo.AtTrigger()) {
        char_timeout_pending = true;
        UpdateIIR();
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  FIFO trigger level configuration
// ═══════════════════════════════════════════════════════════════════════════

void VirtualSerial::SetFIFOTrigger(uint8_t fcr_val) {
    switch (fcr_val & FCR_TRIG_MASK) {
        case FCR_TRIG_1:  rx_fifo.trigger_level = 1;  break;
        case FCR_TRIG_4:  rx_fifo.trigger_level = 4;  break;
        case FCR_TRIG_8:  rx_fifo.trigger_level = 8;  break;
        case FCR_TRIG_14: rx_fifo.trigger_level = 14; break;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Configuration query
// ═══════════════════════════════════════════════════════════════════════════

uint32_t VirtualSerial::GetBaudRate() const {
    if (divisor == 0) return 115200; // Divisor 0 treated as 1
    return 115200 / divisor;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Debug dump
// ═══════════════════════════════════════════════════════════════════════════

void VirtualSerial::DumpState() {
    SerialLogger::Log("=== VSerial COM 0x");
    SerialLogger::LogHex(base_port);
    SerialLogger::Log(" ===\r\n");

    SerialLogger::Log("  IER=");    SerialLogger::LogHex(ier);
    SerialLogger::Log(" IIR=");     SerialLogger::LogHex(iir);
    SerialLogger::Log(" LCR=");     SerialLogger::LogHex(lcr);
    SerialLogger::Log(" MCR=");     SerialLogger::LogHex(mcr);
    SerialLogger::Log("\r\n");

    SerialLogger::Log("  LSR=");    SerialLogger::LogHex(lsr);
    SerialLogger::Log(" MSR=");     SerialLogger::LogHex(msr);
    SerialLogger::Log(" SCR=");     SerialLogger::LogHex(scr);
    SerialLogger::Log("\r\n");

    SerialLogger::Log("  Divisor=");    SerialLogger::LogDec(divisor);
    SerialLogger::Log(" Baud=");    SerialLogger::LogDec(GetBaudRate());
    SerialLogger::Log("\r\n");

    SerialLogger::Log("  FIFO: ");
    SerialLogger::Log(fifo_enabled ? "enabled" : "disabled");
    SerialLogger::Log(" RX=");
    SerialLogger::LogDec(rx_fifo.Count());
    SerialLogger::Log("/");
    SerialLogger::LogDec(UART_FIFO_SIZE);
    SerialLogger::Log(" trigger=");
    SerialLogger::LogDec(rx_fifo.trigger_level);
    SerialLogger::Log("\r\n");

    SerialLogger::Log("  IRQ pending: ");
    SerialLogger::Log(HasPendingIRQ() ? "yes" : "no");
    SerialLogger::Log(" output_buf=");
    SerialLogger::LogDec(output_count);
    SerialLogger::Log("\r\n");
}
