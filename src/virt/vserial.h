//  kurono os - virtual serial port (8250/16550a uart) emulation
//  full-featured com port emulation for guest vm serial i/o.
//  supports: tx/rx fifo, interrupt generation, modem control,
//            baud rate divisor, line/modem status, scratch register.
//  reference: pc16550d datasheet, osdev 8250 uart
#pragma once
#include <stdint.h>
#include <stddef.h>

// these offsets are relative to the com port base address.
// com1 = 0x3f8, com2 = 0x2f8, com3 = 0x3e8, com4 = 0x2e8.
constexpr uint16_t UART_RBR = 0; // receive buffer register (read, dlab=0)
constexpr uint16_t UART_THR = 0; // transmit holding register (write, dlab=0)
constexpr uint16_t UART_DLL = 0; // divisor latch low (dlab=1)
constexpr uint16_t UART_IER = 1; // interrupt enable register (dlab=0)
constexpr uint16_t UART_DLH = 1; // divisor latch high (dlab=1)
constexpr uint16_t UART_IIR = 2; // interrupt identification register (read)
constexpr uint16_t UART_FCR = 2; // fifo control register (write)
constexpr uint16_t UART_LCR = 3; // line control register
constexpr uint16_t UART_MCR = 4; // modem control register
constexpr uint16_t UART_LSR = 5; // line status register
constexpr uint16_t UART_MSR = 6; // modem status register
constexpr uint16_t UART_SCR = 7; // scratch register

constexpr uint8_t IER_RX_AVAIL   = 0x01; // received data available
constexpr uint8_t IER_TX_EMPTY   = 0x02; // transmit holding register empty
constexpr uint8_t IER_LINE_STS   = 0x04; // receiver line status change
constexpr uint8_t IER_MODEM_STS  = 0x08; // modem status change

constexpr uint8_t IIR_NO_INT     = 0x01; // no interrupt pending
constexpr uint8_t IIR_ID_MASK    = 0x0E; // interrupt identification mask
constexpr uint8_t IIR_MODEM_STS  = 0x00; // modem status
constexpr uint8_t IIR_TX_EMPTY   = 0x02; // transmitter holding register empty
constexpr uint8_t IIR_RX_AVAIL   = 0x04; // received data available
constexpr uint8_t IIR_LINE_STS   = 0x06; // receiver line status
constexpr uint8_t IIR_CHAR_TMO   = 0x0C; // character timeout (16550)
constexpr uint8_t IIR_FIFO_EN    = 0xC0; // fifo enabled (16550)

constexpr uint8_t FCR_FIFO_EN    = 0x01; // enable fifos
constexpr uint8_t FCR_RX_RESET   = 0x02; // clear receive fifo
constexpr uint8_t FCR_TX_RESET   = 0x04; // clear transmit fifo
constexpr uint8_t FCR_DMA_MODE   = 0x08; // dma mode select
constexpr uint8_t FCR_TRIG_MASK  = 0xC0; // receive fifo trigger level
constexpr uint8_t FCR_TRIG_1     = 0x00; // trigger at 1 byte
constexpr uint8_t FCR_TRIG_4     = 0x40; // trigger at 4 bytes
constexpr uint8_t FCR_TRIG_8     = 0x80; // trigger at 8 bytes
constexpr uint8_t FCR_TRIG_14    = 0xC0; // trigger at 14 bytes

constexpr uint8_t LCR_WORD_5     = 0x00;
constexpr uint8_t LCR_WORD_6     = 0x01;
constexpr uint8_t LCR_WORD_7     = 0x02;
constexpr uint8_t LCR_WORD_8     = 0x03;
constexpr uint8_t LCR_WORD_MASK  = 0x03;
constexpr uint8_t LCR_STOP_2     = 0x04; // 2 stop bits (1.5 for 5-bit word)
constexpr uint8_t LCR_PARITY_EN  = 0x08;
constexpr uint8_t LCR_PARITY_EVEN= 0x10;
constexpr uint8_t LCR_PARITY_STK = 0x20; // stick parity
constexpr uint8_t LCR_BREAK      = 0x40; // set break
constexpr uint8_t LCR_DLAB       = 0x80; // divisor latch access bit

constexpr uint8_t MCR_DTR        = 0x01; // data terminal ready
constexpr uint8_t MCR_RTS        = 0x02; // request to send
constexpr uint8_t MCR_OUT1       = 0x04; // auxiliary output 1
constexpr uint8_t MCR_OUT2       = 0x08; // aux output 2 (enables irq on pc)
constexpr uint8_t MCR_LOOP       = 0x10; // loopback mode

constexpr uint8_t LSR_DR         = 0x01; // data ready (rx data available)
constexpr uint8_t LSR_OE         = 0x02; // overrun error
constexpr uint8_t LSR_PE         = 0x04; // parity error
constexpr uint8_t LSR_FE         = 0x08; // framing error
constexpr uint8_t LSR_BI         = 0x10; // break interrupt
constexpr uint8_t LSR_THRE       = 0x20; // transmit holding register empty
constexpr uint8_t LSR_TEMT       = 0x40; // transmitter empty (shift reg + thr)
constexpr uint8_t LSR_FIFO_ERR   = 0x80; // error in receive fifo

constexpr uint8_t MSR_DCTS       = 0x01; // delta cts
constexpr uint8_t MSR_DDSR       = 0x02; // delta dsr
constexpr uint8_t MSR_TERI       = 0x04; // trailing edge ri
constexpr uint8_t MSR_DDCD       = 0x08; // delta dcd
constexpr uint8_t MSR_CTS        = 0x10; // clear to send
constexpr uint8_t MSR_DSR        = 0x20; // data set ready
constexpr uint8_t MSR_RI         = 0x40; // ring indicator
constexpr uint8_t MSR_DCD        = 0x80; // data carrier detect

constexpr int UART_FIFO_SIZE     = 16;   // 16550a fifo depth

constexpr int UART_OUTPUT_BUF_SIZE = 4096;

//  uart fifo - circular buffer for tx/rx
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
        if (count >= UART_FIFO_SIZE) return false; // overrun
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

//  virtualserial - full 16550a uart emulation
class VirtualSerial {
public:
    void Reset();
    void Init(uint16_t base_port, uint8_t irq_num);

    void     WritePort(uint16_t port, uint8_t value);
    uint8_t  ReadPort(uint16_t port);

    void     InjectRxByte(uint8_t byte);
    void     InjectRxString(const char* str);

    int      ReadOutput(char* buf, int max_len);
    bool     HasOutput() const;
    int      GetOutputCount() const;

    bool     HasPendingIRQ() const;
    void     ClearIRQ();

    void     Tick(uint32_t elapsed_us);

    uint16_t GetBasePort()  const { return base_port; }
    uint8_t  GetIRQ()       const { return irq; }
    uint32_t GetBaudRate()  const;

    void DumpState();

private:
    // port base and irq
    uint16_t base_port;
    uint8_t  irq;

    uint8_t  ier;           // interrupt enable register
    uint8_t  iir;           // interrupt identification register (computed)
    uint8_t  fcr;           // fifo control register
    uint8_t  lcr;           // line control register
    uint8_t  mcr;           // modem control register
    uint8_t  lsr;           // line status register
    uint8_t  msr;           // modem status register
    uint8_t  scr;           // scratch register
    uint16_t divisor;       // baud rate divisor (dll + dlh)
    uint8_t  thr;           // transmit holding register (non-fifo mode)

    UartFifo rx_fifo;
    UartFifo tx_fifo;
    bool     fifo_enabled;

    bool     irq_pending;
    bool     thre_pending;  // thre interrupt asserted
    uint32_t char_timeout_counter;
    bool     char_timeout_pending;

    char     output_buf[UART_OUTPUT_BUF_SIZE];
    int      output_head;
    int      output_tail;
    int      output_count;

    void     UpdateIIR();
    void     UpdateLSR();
    void     UpdateMSR();
    void     TransmitByte(uint8_t byte);
    void     OutputCapture(uint8_t byte);
    void     SetFIFOTrigger(uint8_t fcr_val);
};

//  com port definitions - up to 4 virtual serial ports
constexpr uint16_t COM1_BASE = 0x3F8;
constexpr uint16_t COM2_BASE = 0x2F8;
constexpr uint16_t COM3_BASE = 0x3E8;
constexpr uint16_t COM4_BASE = 0x2E8;
constexpr uint8_t  COM1_IRQ  = 4;
constexpr uint8_t  COM2_IRQ  = 3;
constexpr uint8_t  COM3_IRQ  = 4;
constexpr uint8_t  COM4_IRQ  = 3;

