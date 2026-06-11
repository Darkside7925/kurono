// ═══════════════════════════════════════════════════════════════════════════
//  Kurono OS — Virtual Serial Port (8250/16550A UART) Emulation
//  Full-featured COM port emulation for guest VM serial I/O.
//  Supports: TX/RX FIFO, interrupt generation, modem control,
//            baud rate divisor, line/modem status, scratch register.
//  Reference: PC16550D datasheet, OSDev 8250 UART
// ═══════════════════════════════════════════════════════════════════════════
#pragma once
#include <stdint.h>
#include <stddef.h>

// ─── I/O Port Offsets (from base address) ────────────────────────────────
// These offsets are relative to the COM port base address.
// COM1 = 0x3F8, COM2 = 0x2F8, COM3 = 0x3E8, COM4 = 0x2E8.
constexpr uint16_t UART_RBR = 0; // Receive Buffer Register (read, DLAB=0)
constexpr uint16_t UART_THR = 0; // Transmit Holding Register (write, DLAB=0)
constexpr uint16_t UART_DLL = 0; // Divisor Latch Low (DLAB=1)
constexpr uint16_t UART_IER = 1; // Interrupt Enable Register (DLAB=0)
constexpr uint16_t UART_DLH = 1; // Divisor Latch High (DLAB=1)
constexpr uint16_t UART_IIR = 2; // Interrupt Identification Register (read)
constexpr uint16_t UART_FCR = 2; // FIFO Control Register (write)
constexpr uint16_t UART_LCR = 3; // Line Control Register
constexpr uint16_t UART_MCR = 4; // Modem Control Register
constexpr uint16_t UART_LSR = 5; // Line Status Register
constexpr uint16_t UART_MSR = 6; // Modem Status Register
constexpr uint16_t UART_SCR = 7; // Scratch Register

// ─── IER bits ────────────────────────────────────────────────────────────
constexpr uint8_t IER_RX_AVAIL   = 0x01; // Received data available
constexpr uint8_t IER_TX_EMPTY   = 0x02; // Transmit holding register empty
constexpr uint8_t IER_LINE_STS   = 0x04; // Receiver line status change
constexpr uint8_t IER_MODEM_STS  = 0x08; // Modem status change

// ─── IIR bits ────────────────────────────────────────────────────────────
constexpr uint8_t IIR_NO_INT     = 0x01; // No interrupt pending
constexpr uint8_t IIR_ID_MASK    = 0x0E; // Interrupt identification mask
constexpr uint8_t IIR_MODEM_STS  = 0x00; // Modem status
constexpr uint8_t IIR_TX_EMPTY   = 0x02; // Transmitter holding register empty
constexpr uint8_t IIR_RX_AVAIL   = 0x04; // Received data available
constexpr uint8_t IIR_LINE_STS   = 0x06; // Receiver line status
constexpr uint8_t IIR_CHAR_TMO   = 0x0C; // Character timeout (16550)
constexpr uint8_t IIR_FIFO_EN    = 0xC0; // FIFO enabled (16550)

// ─── FCR bits ────────────────────────────────────────────────────────────
constexpr uint8_t FCR_FIFO_EN    = 0x01; // Enable FIFOs
constexpr uint8_t FCR_RX_RESET   = 0x02; // Clear receive FIFO
constexpr uint8_t FCR_TX_RESET   = 0x04; // Clear transmit FIFO
constexpr uint8_t FCR_DMA_MODE   = 0x08; // DMA mode select
constexpr uint8_t FCR_TRIG_MASK  = 0xC0; // Receive FIFO trigger level
constexpr uint8_t FCR_TRIG_1     = 0x00; // Trigger at 1 byte
constexpr uint8_t FCR_TRIG_4     = 0x40; // Trigger at 4 bytes
constexpr uint8_t FCR_TRIG_8     = 0x80; // Trigger at 8 bytes
constexpr uint8_t FCR_TRIG_14    = 0xC0; // Trigger at 14 bytes

// ─── LCR bits ────────────────────────────────────────────────────────────
constexpr uint8_t LCR_WORD_5     = 0x00;
constexpr uint8_t LCR_WORD_6     = 0x01;
constexpr uint8_t LCR_WORD_7     = 0x02;
constexpr uint8_t LCR_WORD_8     = 0x03;
constexpr uint8_t LCR_WORD_MASK  = 0x03;
constexpr uint8_t LCR_STOP_2     = 0x04; // 2 stop bits (1.5 for 5-bit word)
constexpr uint8_t LCR_PARITY_EN  = 0x08;
constexpr uint8_t LCR_PARITY_EVEN= 0x10;
constexpr uint8_t LCR_PARITY_STK = 0x20; // Stick parity
constexpr uint8_t LCR_BREAK      = 0x40; // Set Break
constexpr uint8_t LCR_DLAB       = 0x80; // Divisor Latch Access Bit

// ─── MCR bits ────────────────────────────────────────────────────────────
constexpr uint8_t MCR_DTR        = 0x01; // Data Terminal Ready
constexpr uint8_t MCR_RTS        = 0x02; // Request To Send
constexpr uint8_t MCR_OUT1       = 0x04; // Auxiliary output 1
constexpr uint8_t MCR_OUT2       = 0x08; // Aux output 2 (enables IRQ on PC)
constexpr uint8_t MCR_LOOP       = 0x10; // Loopback mode

// ─── LSR bits ────────────────────────────────────────────────────────────
constexpr uint8_t LSR_DR         = 0x01; // Data Ready (RX data available)
constexpr uint8_t LSR_OE         = 0x02; // Overrun Error
constexpr uint8_t LSR_PE         = 0x04; // Parity Error
constexpr uint8_t LSR_FE         = 0x08; // Framing Error
constexpr uint8_t LSR_BI         = 0x10; // Break Interrupt
constexpr uint8_t LSR_THRE       = 0x20; // Transmit Holding Register Empty
constexpr uint8_t LSR_TEMT       = 0x40; // Transmitter Empty (shift reg + THR)
constexpr uint8_t LSR_FIFO_ERR   = 0x80; // Error in receive FIFO

// ─── MSR bits ────────────────────────────────────────────────────────────
constexpr uint8_t MSR_DCTS       = 0x01; // Delta CTS
constexpr uint8_t MSR_DDSR       = 0x02; // Delta DSR
constexpr uint8_t MSR_TERI       = 0x04; // Trailing Edge RI
constexpr uint8_t MSR_DDCD       = 0x08; // Delta DCD
constexpr uint8_t MSR_CTS        = 0x10; // Clear To Send
constexpr uint8_t MSR_DSR        = 0x20; // Data Set Ready
constexpr uint8_t MSR_RI         = 0x40; // Ring Indicator
constexpr uint8_t MSR_DCD        = 0x80; // Data Carrier Detect

// ─── FIFO sizes ──────────────────────────────────────────────────────────
constexpr int UART_FIFO_SIZE     = 16;   // 16550A FIFO depth

// ─── Output buffer for guest-to-host logging ─────────────────────────────
constexpr int UART_OUTPUT_BUF_SIZE = 4096;

// ═══════════════════════════════════════════════════════════════════════════
//  UART FIFO — Circular buffer for TX/RX
// ═══════════════════════════════════════════════════════════════════════════
struct UartFifo {
    uint8_t  data[UART_FIFO_SIZE];
    int      head;
    int      tail;
    int      count;
    int      trigger_level;  // 1, 4, 8, or 14

    void Reset() {
        head = tail = count = 0;
        trigger_level = 1;
    }

    bool IsEmpty() const { return count == 0; }
    bool IsFull()  const { return count >= UART_FIFO_SIZE; }
    int  Count()   const { return count; }

    bool Push(uint8_t byte) {
        if (count >= UART_FIFO_SIZE) return false; // Overrun
        data[tail] = byte;
        tail = (tail + 1) % UART_FIFO_SIZE;
        count++;
        return true;
    }

    uint8_t Pop() {
        if (count == 0) return 0;
        uint8_t byte = data[head];
        head = (head + 1) % UART_FIFO_SIZE;
        count--;
        return byte;
    }

    uint8_t Peek() const {
        if (count == 0) return 0;
        return data[head];
    }

    bool AtTrigger() const {
        return count >= trigger_level;
    }
};

// ═══════════════════════════════════════════════════════════════════════════
//  VirtualSerial — Full 16550A UART emulation
// ═══════════════════════════════════════════════════════════════════════════
class VirtualSerial {
public:
    // ── Initialization ───────────────────────────────────────────────────
    void Reset();
    void Init(uint16_t base_port, uint8_t irq_num);

    // ── Port I/O (called by VM exit handler) ─────────────────────────────
    void     WritePort(uint16_t port, uint8_t value);
    uint8_t  ReadPort(uint16_t port);

    // ── External input (inject data into RX FIFO from host) ─────────────
    void     InjectRxByte(uint8_t byte);
    void     InjectRxString(const char* str);

    // ── Output buffer (guest TX captured here) ───────────────────────────
    int      ReadOutput(char* buf, int max_len);
    bool     HasOutput() const;
    int      GetOutputCount() const;

    // ── Interrupt management ─────────────────────────────────────────────
    bool     HasPendingIRQ() const;
    void     ClearIRQ();

    // ── Tick — advance character timeout, etc. ───────────────────────────
    void     Tick(uint32_t elapsed_us);

    // ── Configuration ────────────────────────────────────────────────────
    uint16_t GetBasePort()  const { return base_port; }
    uint8_t  GetIRQ()       const { return irq; }
    uint32_t GetBaudRate()  const;

    // ── Debug ────────────────────────────────────────────────────────────
    void DumpState();

private:
    // Port base and IRQ
    uint16_t base_port;
    uint8_t  irq;

    // ── Registers ────────────────────────────────────────────────────────
    uint8_t  ier;           // Interrupt Enable Register
    uint8_t  iir;           // Interrupt Identification Register (computed)
    uint8_t  fcr;           // FIFO Control Register
    uint8_t  lcr;           // Line Control Register
    uint8_t  mcr;           // Modem Control Register
    uint8_t  lsr;           // Line Status Register
    uint8_t  msr;           // Modem Status Register
    uint8_t  scr;           // Scratch Register
    uint16_t divisor;       // Baud rate divisor (DLL + DLH)
    uint8_t  thr;           // Transmit Holding Register (non-FIFO mode)

    // ── FIFOs ────────────────────────────────────────────────────────────
    UartFifo rx_fifo;
    UartFifo tx_fifo;
    bool     fifo_enabled;

    // ── Interrupt state ──────────────────────────────────────────────────
    bool     irq_pending;
    bool     thre_pending;  // THRE interrupt asserted
    uint32_t char_timeout_counter;
    bool     char_timeout_pending;

    // ── Guest output capture buffer ──────────────────────────────────────
    char     output_buf[UART_OUTPUT_BUF_SIZE];
    int      output_head;
    int      output_tail;
    int      output_count;

    // ── Internal helpers ─────────────────────────────────────────────────
    void     UpdateIIR();
    void     UpdateLSR();
    void     UpdateMSR();
    void     TransmitByte(uint8_t byte);
    void     OutputCapture(uint8_t byte);
    void     SetFIFOTrigger(uint8_t fcr_val);
};

// ═══════════════════════════════════════════════════════════════════════════
//  COM Port Definitions — up to 4 virtual serial ports
// ═══════════════════════════════════════════════════════════════════════════
constexpr uint16_t COM1_BASE = 0x3F8;
constexpr uint16_t COM2_BASE = 0x2F8;
constexpr uint16_t COM3_BASE = 0x3E8;
constexpr uint16_t COM4_BASE = 0x2E8;
constexpr uint8_t  COM1_IRQ  = 4;
constexpr uint8_t  COM2_IRQ  = 3;
constexpr uint8_t  COM3_IRQ  = 4;
constexpr uint8_t  COM4_IRQ  = 3;


