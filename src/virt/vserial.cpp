//  kurono os  -  virtual serial port (8250/16550a uart) implementation
//  full register-accurate emulation for guest vm serial console.
//
//  features implemented:
//    - full 8250/16450/16550a register set
//    - 16-byte tx/rx fifos with configurable trigger levels
//    - interrupt generation (rx available, tx empty, line status, modem)
//    - dlab mode for baud rate divisor configuration
//    - loopback mode (mcr bit 4)
//    - character timeout indication (16550)
//    - guest output capture buffer for host-side logging
//    - rx injection from host for input to guest
//
//  reference: national semiconductor pc16550d datasheet
//  reference: osdev wiki - serial ports
#include "vserial.h"
#include "../drivers/serial.h"
#include "../kernel/types.h"

//  reset  -  set all registers to power-on defaults

void VirtualSerial::Reset() {
    base_port    = 0;
    irq          = 0;

    ier          = 0x00;
    iir          = IIR_NO_INT; // no interrupt pending
    fcr          = 0x00;
    lcr          = 0x00;       // 5-bit, 1 stop, no parity
    mcr          = 0x00;
    lsr          = LSR_THRE | LSR_TEMT; // tx empty on reset
    msr          = 0x00;
    scr          = 0x00;
    divisor      = 12;         // 9600 baud (115200 / 12)
    thr          = 0x00;

    rx_fifo.Reset();
    tx_fifo.Reset();
    fifo_enabled = false;

    irq_pending  = false;
    thre_pending = true;       // thr is empty on reset
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

    // on init, modem status = cts + dsr + dcd (connected)
    msr = MSR_CTS | MSR_DSR | MSR_DCD;

    SerialLogger::Log("VSerial: Initialized COM at 0x");
    SerialLogger::LogHex(base_port);
    SerialLogger::Log(" IRQ ");
    SerialLogger::LogDec(irq);
    SerialLogger::Log("\r\n");
}

//  writeport  -  guest writes to a uart register

void VirtualSerial::WritePort(uint16_t port, uint8_t value) {
    uint16_t offset = port - base_port;

    switch (offset) {
        case UART_THR: // also uart_dll when dlab=1
            if (lcr & LCR_DLAB) {
                // divisor latch low
                divisor = (divisor & 0xFF00) | value;
            } else {
                // transmit holding register
                if (mcr & MCR_LOOP) {
                    // loopback mode: tx data goes directly to rx
                    if (fifo_enabled) {
                        if (!rx_fifo.IsFull()) {
                            rx_fifo.Push(value);
                        } else {
                            lsr |= LSR_OE; // overrun
                        }
                    }
                    lsr |= LSR_DR; // data ready
                } else {
                    TransmitByte(value);
                }
                // thr is now empty again (instant transmit in emulation)
                lsr |= LSR_THRE | LSR_TEMT;
                thre_pending = true;
            }
            break;

        case UART_IER: // also uart_dlh when dlab=1
            if (lcr & LCR_DLAB) {
                // divisor latch high
                divisor = (divisor & 0x00FF) | ((uint16_t)value << 8);
            } else {
                // interrupt enable register
                ier = value & 0x0F; // only lower 4 bits
                // re-evaluate pending interrupts
                if (ier & IER_TX_EMPTY) {
                    // if thr is empty, thre interrupt fires
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
            mcr = value & 0x1F; // only lower 5 bits
            // update msr based on mcr in loopback mode
            if (mcr & MCR_LOOP) {
                UpdateMSR();
            }
            break;

        case UART_LSR:
            // lsr is read-only, ignore writes (some software writes here)
            break;

        case UART_MSR:
            // msr is read-only, ignore writes
            break;

        case UART_SCR:
            scr = value;
            break;

        default:
            break;
    }

    UpdateIIR();
}

//  readport  -  guest reads a uart register

uint8_t VirtualSerial::ReadPort(uint16_t port) {
    uint16_t offset = port - base_port;

    switch (offset) {
        case UART_RBR: // also uart_dll when dlab=1
            if (lcr & LCR_DLAB) {
                return (uint8_t)(divisor & 0xFF);
            } else {
                // read from receive buffer / fifo
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

        case UART_IER: // also uart_dlh when dlab=1
            if (lcr & LCR_DLAB) {
                return (uint8_t)((divisor >> 8) & 0xFF);
            }
            return ier;

        case UART_IIR:
        {
            uint8_t val = iir;
            if (fifo_enabled) {
                val |= IIR_FIFO_EN; // show fifos enabled
            }
            // reading iir with thre clears thre interrupt
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
            // reading lsr clears error bits (oe, pe, fe, bi)
            lsr &= ~(LSR_OE | LSR_PE | LSR_FE | LSR_BI | LSR_FIFO_ERR);
            UpdateIIR();
            return val;
        }

        case UART_MSR:
        {
            uint8_t val = msr;
            // reading msr clears delta bits
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

//  transmitbyte  -  handle guest tx output

void VirtualSerial::TransmitByte(uint8_t byte) {
    // capture to output buffer for host-side reading
    OutputCapture(byte);

    // also log to host serial for real-time monitoring
    // (this goes to the real host com port / qemu serial)
    char c[2] = { (char)byte, 0 };
    SerialLogger::Log(c);
}

void VirtualSerial::OutputCapture(uint8_t byte) {
    if (output_count >= UART_OUTPUT_BUF_SIZE) {
        // buffer full  -  drop oldest byte
        output_head = (output_head + 1) % UART_OUTPUT_BUF_SIZE;
        output_count--;
    }
    output_buf[output_tail] = (char)byte;
    output_tail = (output_tail + 1) % UART_OUTPUT_BUF_SIZE;
    output_count++;
}

//  external rx injection  -  push data from host into guest

void VirtualSerial::InjectRxByte(uint8_t byte) {
    if (fifo_enabled) {
        if (!rx_fifo.IsFull()) {
            rx_fifo.Push(byte);
            lsr |= LSR_DR;
            char_timeout_counter = 0;
        } else {
            lsr |= LSR_OE; // overrun error
        }
    } else {
        // non-fifo: only 1-byte buffer
        if (lsr & LSR_DR) {
            lsr |= LSR_OE; // previous byte not read → overrun
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

//  output buffer read  -  host reads guest tx output

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

//  interrupt management

void VirtualSerial::UpdateIIR() {
    // priority order (highest first):
    //   1. receiver line status (oe/pe/fe/bi)
    //   2. received data available / character timeout
    //   3. transmitter holding register empty
    //   4. modem status

    // check receiver line status
    if ((ier & IER_LINE_STS) && (lsr & (LSR_OE | LSR_PE | LSR_FE | LSR_BI))) {
        iir = IIR_LINE_STS;
        irq_pending = true;
        return;
    }

    // check received data available
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

    // check transmitter empty
    if ((ier & IER_TX_EMPTY) && thre_pending) {
        iir = IIR_TX_EMPTY;
        irq_pending = true;
        return;
    }

    // check modem status
    if ((ier & IER_MODEM_STS) && (msr & 0x0F)) {
        iir = IIR_MODEM_STS;
        irq_pending = true;
        return;
    }

    // no interrupt
    iir = IIR_NO_INT;
    irq_pending = false;
}

void VirtualSerial::UpdateLSR() {
    // dr bit: data ready in rx buffer/fifo
    if (fifo_enabled) {
        if (!rx_fifo.IsEmpty()) lsr |= LSR_DR;
        else lsr &= ~LSR_DR;
    }
    // thre and temt are managed during tx
}

void VirtualSerial::UpdateMSR() {
    if (mcr & MCR_LOOP) {
        // in loopback, mcr outputs are reflected to msr inputs
        uint8_t old_msr = msr & 0xF0;
        uint8_t new_inputs = 0;
        if (mcr & MCR_RTS)  new_inputs |= MSR_CTS;
        if (mcr & MCR_DTR)  new_inputs |= MSR_DSR;
        if (mcr & MCR_OUT1) new_inputs |= MSR_RI;
        if (mcr & MCR_OUT2) new_inputs |= MSR_DCD;

        // compute delta bits
        uint8_t changes = old_msr ^ new_inputs;
        msr = new_inputs; // set input states
        if (changes & MSR_CTS)  msr |= MSR_DCTS;
        if (changes & MSR_DSR)  msr |= MSR_DDSR;
        if (changes & MSR_RI)   msr |= MSR_TERI;
        if (changes & MSR_DCD)  msr |= MSR_DDCD;
    } else {
        // not loopback: always show connected
        msr = MSR_CTS | MSR_DSR | MSR_DCD;
    }
}

bool VirtualSerial::HasPendingIRQ() const {
    // irq only fires if mcr.out2 is set (pc standard: out2 gates irq)
    return irq_pending && (mcr & MCR_OUT2);
}

void VirtualSerial::ClearIRQ() {
    irq_pending = false;
}

//  tick  -  advance character timeout timer

void VirtualSerial::Tick(uint32_t elapsed_us) {
    if (!fifo_enabled || rx_fifo.IsEmpty()) {
        char_timeout_counter = 0;
        char_timeout_pending = false;
        return;
    }

    // character timeout: triggers when fifo has data but level < trigger,
    // and no new data received for 4 character times.
    // at 115200 baud, 8n1: 1 char ~= 87us → 4 chars ~= 348us
    uint32_t char_time_us = 1;
    if (divisor > 0) {
        // baud rate = 115200 / divisor
        // character time = (bits_per_char / baud_rate) * 1e6 us
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

//  fifo trigger level configuration

void VirtualSerial::SetFIFOTrigger(uint8_t fcr_val) {
    switch (fcr_val & FCR_TRIG_MASK) {
        case FCR_TRIG_1:  rx_fifo.trigger_level = 1;  break;
        case FCR_TRIG_4:  rx_fifo.trigger_level = 4;  break;
        case FCR_TRIG_8:  rx_fifo.trigger_level = 8;  break;
        case FCR_TRIG_14: rx_fifo.trigger_level = 14; break;
    }
}

//  configuration query

uint32_t VirtualSerial::GetBaudRate() const {
    if (divisor == 0) return 115200; // divisor 0 treated as 1
    return 115200 / divisor;
}

//  debug dump

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
