#include "WProgram.h"
#include "pins_arduino.h"
#include "dmaLed.h"
#include "lpd8806.h"
#include "blinkytile.h"


// Send 3 zeros at the end of the data stream
#define OUTPUT_BYTES        (LED_COUNT*BYTES_PER_PIXEL+3)*8


// Check that the output buffer size is sufficient
#if DMA_BUFFER_SIZE < OUTPUT_BYTES*2
#error DMA Buffer too small, cannot use lpd8806 output.
#endif

#define DATA_PIN_OFFSET              4       // Offset of our data pin in port C
#define CLOCK_PIN_OFFSET             3      // Offset of our clock pin in port C

LPD8806Controller lpd8806;

namespace LPD8806 {

uint8_t* frontBuffer;
uint8_t* backBuffer;
bool swapBuffers;

uint8_t ONE = _BV(CLOCK_PIN_OFFSET);

// FTM0 drives our whole operation!
void setupFTM0(){
  FTM0_MODE = FTM_MODE_WPDIS;    // Disable Write Protect

  FTM0_SC = 0;                   // Turn off the clock so we can update CNTIN and MODULO?
  FTM0_MOD = 0x0040;             // Period register
  FTM0_SC |= FTM_SC_CLKS(1) | FTM_SC_PS(0);

  FTM0_MODE |= FTM_MODE_INIT;         // Enable FTM0

  FTM0_C0SC = 0x40          // Enable interrupt
  | 0x20                    // Mode select: Edge-aligned PWM 
  | 0x04                    // Low-true pulses (inverted)
  | 0x01;                   // Enable DMA out
  FTM0_C0V = 0x0020;        // Duty cycle of PWM signal

  FTM0_C2SC = 0x40          // Enable interrupt
  | 0x20                    // Mode select: Edge-aligned PWM 
  | 0x04                    // Low-true pulses (inverted)
  | 0x01;                   // Enable DMA out
  FTM0_C2V = 0x0001;        // Duty cycle of PWM signal

  FTM0_SYNC |= 0x80;        // set PWM value update
}


// TCD0 clears the clock output
void setupTCD0(uint8_t* source, int minorLoopSize, int majorLoops) {
  DMA_TCD0_SADDR = source;                                        // Address to read from
  DMA_TCD0_SOFF = 0;                                              // Bytes to increment source register between writes 
  DMA_TCD0_ATTR = DMA_TCD_ATTR_SSIZE(0) | DMA_TCD_ATTR_DSIZE(0);  // 8-bit input and output
  DMA_TCD0_NBYTES_MLNO = minorLoopSize;                           // Number of bytes to transfer in the minor loop
  DMA_TCD0_SLAST = 0;                                             // Bytes to add after a major iteration count (N/A)
  DMA_TCD0_DADDR = &GPIOC_PSOR;                                   // Address to write to
  DMA_TCD0_DOFF = 0;                                              // Bytes to increment destination register between write
  DMA_TCD0_CITER_ELINKNO = majorLoops;                            // Number of major loops to complete
  DMA_TCD0_BITER_ELINKNO = majorLoops;                            // Reset value for CITER (must be equal to CITER)
  DMA_TCD0_DLASTSGA = 0;                                          // Address of next TCD (N/A)
}

// TCD2 sets the data and clock outputs
void setupTCD2(uint8_t* source, int minorLoopSize, int majorLoops) {
  DMA_TCD2_SADDR = source;                                        // Address to read from
  DMA_TCD2_SOFF = 1;                                              // Bytes to increment source register between writes 
  DMA_TCD2_ATTR = DMA_TCD_ATTR_SSIZE(0) | DMA_TCD_ATTR_DSIZE(0);  // 8-bit input and output
  DMA_TCD2_NBYTES_MLNO = minorLoopSize;                           // Number of bytes to transfer in the minor loop
  DMA_TCD2_SLAST = 0;                                             // Bytes to add after a major iteration count (N/A)
  DMA_TCD2_DADDR = &GPIOC_PDOR;                                   // Address to write to
  DMA_TCD2_DOFF = 0;                                              // Bytes to increment destination register between write
  DMA_TCD2_CITER_ELINKNO = majorLoops;                            // Number of major loops to complete
  DMA_TCD2_BITER_ELINKNO = majorLoops;                            // Reset value for CITER (must be equal to CITER)
  DMA_TCD2_DLASTSGA = 0;                                          // Address of next TCD (N/A)
}


void setupTCDs() {
    setupTCD0(&ONE, 1, OUTPUT_BYTES);
    setupTCD2(frontBuffer, 1, OUTPUT_BYTES);
}

// Send a DMX frame with new data
void lpd8806Transmit() {
    setupTCDs();

//    DMA_SSRT = DMA_SSRT_SAST;

    FTM0_SC |= FTM_SC_CLKS(1);
}

} // namespace LPD8806

// At the end of the DMX frame, start it again
// TODO: This is on channel 2, because channel 0 and 1 are used. Can we stack these somehow?
void dma_ch2_isr(void) {
    DMA_CINT = DMA_CINT_CINT(2);

    // Wait until DMA2 is triggered, then stop the counter
    while(FTM0_CNT < 0x20) {}

    // TODO: Turning off the timer this late into the cycle causes a small glitch :-/
    FTM0_SC = 0;                   // Turn off the clock so we can update CNTIN and MODULO?

    if(LPD8806::swapBuffers) {
        uint8_t* lastBuffer = LPD8806::frontBuffer;
        LPD8806::frontBuffer = LPD8806::backBuffer;
        LPD8806::backBuffer = lastBuffer;
        LPD8806::swapBuffers = false;
    }

    // TODO: Re-start this chain here  
//    lpd8806Transmit();
}

void LPD8806Controller::start() {
    PORTC_PCR4 = PORT_PCR_DSE | PORT_PCR_SRE | PORT_PCR_MUX(1);	// Configure TX Pin for digital
    PORTC_PCR3 = PORT_PCR_DSE | PORT_PCR_SRE | PORT_PCR_MUX(1);	// Configure RX Pin for digital
    GPIOC_PDDR |= _BV(DATA_PIN_OFFSET) | _BV(CLOCK_PIN_OFFSET);

    // DMA
    // Configure DMA
    SIM_SCGC7 |= SIM_SCGC7_DMA;  // Enable DMA clock
    DMA_CR = 0;  // Use default configuration
 
    // Configure the DMA request input for DMA0
    DMA_SERQ = DMA_SERQ_SERQ(0);        // Configure DMA0 to enable the request signal
    DMA_SERQ = DMA_SERQ_SERQ(2);        // Configure DMA2 to enable the request signal
 
    // Enable interrupt on major completion for DMA channel 1
    DMA_TCD2_CSR = DMA_TCD_CSR_INTMAJOR;  // Enable interrupt on major complete
    NVIC_ENABLE_IRQ(IRQ_DMA_CH2);         // Enable interrupt request
 
    // DMAMUX
    SIM_SCGC6 |= SIM_SCGC6_DMAMUX; // Enable DMAMUX clock

    // Timer DMA channel:
    // Configure DMAMUX to trigger DMA0 from FTM0_CH0
    DMAMUX0_CHCFG0 = DMAMUX_DISABLE;
    DMAMUX0_CHCFG0 = DMAMUX_SOURCE_FTM0_CH0 | DMAMUX_ENABLE;

    // Configure DMAMUX to trigger DMA2 from FTM0_CH2
    DMAMUX0_CHCFG2 = DMAMUX_DISABLE;
    DMAMUX0_CHCFG2 = DMAMUX_SOURCE_FTM0_CH2 | DMAMUX_ENABLE;
 
    // Load this frame of data into the DMA engine
    LPD8806::setupTCDs();
 
    // FTM
    SIM_SCGC6 |= SIM_SCGC6_FTM0;  // Enable FTM0 clock
    LPD8806::setupFTM0();

    LPD8806::frontBuffer = dmaBuffer;
    LPD8806::backBuffer =  dmaBuffer + OUTPUT_BYTES;
    LPD8806::swapBuffers = false;

    // Clear the display
    memset(LPD8806::frontBuffer, 0x00, OUTPUT_BYTES);

//    // The last 24 bits transmitted should always be data zeros
//    for(int offset = 0; offset < 24; offset++) {
//        LPD8806::frontBuffer[LED_COUNT*3*8+offset] = _BV(CLOCK_PIN_OFFSET);
//        LPD8806::backBuffer[LED_COUNT*3*8+offset]  = _BV(CLOCK_PIN_OFFSET);
//    }

    LPD8806::lpd8806Transmit();
}


void LPD8806Controller::stop() {
    FTM0_SC = 0;                        // Disable FTM0 clock

    DMA_TCD2_CSR = 0;                   // Disable interrupt on major complete
    NVIC_DISABLE_IRQ(IRQ_DMA_CH2);      // Disable interrupt request

    DMAMUX0_CHCFG0 = DMAMUX_DISABLE;
    DMAMUX0_CHCFG2 = DMAMUX_DISABLE;

    SIM_SCGC6 &= ~SIM_SCGC6_FTM0;       // Disable FTM0 clock
    SIM_SCGC6 &= ~SIM_SCGC6_DMAMUX;     // turn off DMA MUX clock
    SIM_SCGC7 &= ~SIM_SCGC7_DMA;        // turn off DMA clock
}


bool LPD8806Controller::waiting() {
    return LPD8806::swapBuffers;
}

void LPD8806Controller::show() {
    LPD8806::lpd8806Transmit();

    // If a draw is already pending, skip the new frame
    if(LPD8806::swapBuffers == true) {
        return;
    }

    // Copy the data, but scale it based on the system brightness
    for(int led = 0; led < LED_COUNT; led++) {

        // TODO: Make the pattern order RGB
        uint8_t red =   (drawBuffer[led*3+0]*brightness)/255;
        uint8_t green = (drawBuffer[led*3+1]*brightness)/255;
        uint8_t blue =  (drawBuffer[led*3+2]*brightness)/255;

        // MSB is low when clocking data
//        LPD8806::backBuffer[led*3*8 +  0] = _BV(CLOCK_PIN_OFFSET) | _BV(DATA_PIN_OFFSET);
//        LPD8806::backBuffer[led*3*8 +  8] = _BV(CLOCK_PIN_OFFSET) | _BV(DATA_PIN_OFFSET);
//        LPD8806::backBuffer[led*3*8 + 16] = _BV(CLOCK_PIN_OFFSET) | _BV(DATA_PIN_OFFSET);
        LPD8806::backBuffer[led*3*8 +  0] = _BV(DATA_PIN_OFFSET);
        LPD8806::backBuffer[led*3*8 +  8] = _BV(DATA_PIN_OFFSET);
        LPD8806::backBuffer[led*3*8 + 16] = _BV(DATA_PIN_OFFSET);

        // Shift the remaining bits by 1, since this is a 7 bit LED controller
        for(int bit = 1; bit < 8; bit++) {
//            LPD8806::backBuffer[led*3*8 +  0 + bit] = _BV(CLOCK_PIN_OFFSET) | ((( green >> (8-bit))&0x1) << DATA_PIN_OFFSET);
//            LPD8806::backBuffer[led*3*8 +  8 + bit] = _BV(CLOCK_PIN_OFFSET) | ((( red >>   (8-bit))&0x1) << DATA_PIN_OFFSET);
//            LPD8806::backBuffer[led*3*8 + 16 + bit] = _BV(CLOCK_PIN_OFFSET) | ((( blue >>  (8-bit))&0x1) << DATA_PIN_OFFSET);
            LPD8806::backBuffer[led*3*8 +  0 + bit] = ((( green >> (8-bit))&0x1) << DATA_PIN_OFFSET);
            LPD8806::backBuffer[led*3*8 +  8 + bit] = ((( red >>   (8-bit))&0x1) << DATA_PIN_OFFSET);
            LPD8806::backBuffer[led*3*8 + 16 + bit] = ((( blue >>  (8-bit))&0x1) << DATA_PIN_OFFSET);
        }
    }
    LPD8806::swapBuffers = true;
}

