/*
 * VGA leaf logo fade-in, September 2010.
 *
 * Adapted from VGA Pong by Marti Bolivar
 *
 * This code is released into the public domain.
 */

#include "wirish.h"

/* Pinouts -- you also must change the GPIO macros below if you change
 * these.  Increasing number means more significant bit. */

#define R0_PIN 34               // STM32: B15
#define R1_PIN 33               // STM32: B14
#define R2_PIN 32               // STM32: B13

#define G0_PIN 31               // STM32: B12
#define G1_PIN 30               // STM32: B11
#define G2_PIN 29               // STM32: B10

#define B0_PIN 28               // STM32: B1
#define B1_PIN 27               // STM32: B0
#define B2_PIN 24               // STM32: B9

#define VS_PIN 21               // STM32: C13
#define HS_PIN 22               // STM32: C14

/* color GPIO (general purpose input/output) macros
 *
 * (All of the low-level macros that follow are necessary for speed;
 * digitalWrite just isn't fast enough for VGA's 25 MHz pixel clock.
 * But hey, at least we didn't have to write any assembly!) */

// A pointer to the STM32F103 GPIO port B bit set/reset register.  See
// ST's reference manual, especially sections 2.3 and 8.2.5:
// http://www.st.com/stonline/products/literature/rm/13902.pdf
//
// Writing to this lets us deterministically update all of the color
// values we send to the monitor in a single cycle, which is crucial
// for getting decent quality VGA output. (Note that it still takes
// multiple cycles when you include loading the value from memory,
// etc.)
#define BBSRR ((volatile uint32*)0x40010C10)

// What bits to write in BBSRR to update the corresponding pins.
#define R0_BIT 15
#define R1_BIT 14
#define R2_BIT 13

#define G0_BIT 12
#define G1_BIT 11
#define G2_BIT 10

#define B0_BIT 1
#define B1_BIT 0
#define B2_BIT 9

// convenience macros for color values.  BIT(n) produces an integer
// with the nth bit set.
//
// black
#define C_BLACK (0)
// primary colors at various strengths
#define C_R0 (BIT(R0_BIT))
#define C_R1 (BIT(R1_BIT))
#define C_R2 (BIT(R2_BIT))
#define C_G0 (BIT(G0_BIT))
#define C_G1 (BIT(G1_BIT))
#define C_G2 (BIT(G2_BIT))
#define C_B0 (BIT(B0_BIT))
#define C_B1 (BIT(B1_BIT))
#define C_B2 (BIT(B2_BIT))
// primary colors
#define C_RED   (C_R0 | C_R1 | C_R2)
#define C_GREEN (C_G0 | C_G1 | C_G2)
#define C_BLUE  (C_B0 | C_B1 | C_B2)
// secondary colors
#define C_MAGENTA (C_R2 | C_B2)
#define C_CYAN    (C_G2 | C_B2)
#define C_YELLOW  (C_G2 | C_R2)
// white
#define C_WHITE (C_RED | C_GREEN | C_BLUE)

// given an color (which is an OR of BIT applied to the [RGB][012]_BIT
// macros, above), convert it into a 32-bit integer which, if written
// to the BSRR, will set the corresponding [RGB][012]_PIN pins.
#define COLOR_TO_BSRR(c) \
    ((c) |                                                              \
     BIT(R0_BIT+16) | BIT(R1_BIT+16) | BIT(R2_BIT+16) |                 \
     BIT(G0_BIT+16) | BIT(G1_BIT+16) | BIT(G2_BIT+16) |                 \
     BIT(B0_BIT+16) | BIT(B1_BIT+16) | BIT(B2_BIT+16))

// convenience macros for BSRR color values.
#define BSRR_BLACK   COLOR_TO_BSRR(C_BLACK)
#define BSRR_R0      COLOR_TO_BSRR(C_R0)
#define BSRR_R1      COLOR_TO_BSRR(C_R1)
#define BSRR_R2      COLOR_TO_BSRR(C_R2)
#define BSRR_G0      COLOR_TO_BSRR(C_G0)
#define BSRR_G1      COLOR_TO_BSRR(C_G1)
#define BSRR_G2      COLOR_TO_BSRR(C_G2)
#define BSRR_B0      COLOR_TO_BSRR(C_B0)
#define BSRR_B1      COLOR_TO_BSRR(C_B1)
#define BSRR_B2      COLOR_TO_BSRR(C_B2)
#define BSRR_RED     COLOR_TO_BSRR(C_RED)
#define BSRR_GREEN   COLOR_TO_BSRR(C_GREEN)
#define BSRR_BLUE    COLOR_TO_BSRR(C_BLUE)
#define BSRR_MAGENTA COLOR_TO_BSRR(C_MAGENTA)
#define BSRR_CYAN    COLOR_TO_BSRR(C_CYAN)
#define BSRR_YELLOW  COLOR_TO_BSRR(C_YELLOW)
#define BSRR_WHITE   COLOR_TO_BSRR(C_WHITE)

// Assumes the argument is an appropriate value for sending to the
// BSRR.  see COLOR_TO_BSRR for a convenient way to get such a value.
#define VGA_SET_BSRR(bsrr) (*BBSRR = (bsrr))

/* Hsync/vsync GPIO macros */

// STM32 port C BSRR and bit reset register BRR.  See the STM32
// manual, referenced above.
#define CBSRR ((volatile uint32*)0x40011010)
#define CBRR  ((volatile uint32*)0x40011014)

// Bits to write in CBSRR/CBRR to set/reset the vsync/hsync pins.
#define VS_BIT 13
#define HS_BIT 14

// Macros for sending hsync/vsync to the monitor.
#define VGA_VS_HIGH (*CBSRR = BIT(VS_BIT))
#define VGA_VS_LOW  (*CBRR  = BIT(VS_BIT))
#define VGA_HS_HIGH (*CBSRR = BIT(HS_BIT))
#define VGA_HS_LOW  (*CBRR  = BIT(HS_BIT))

/* VGA state */

// We take 2287 cycles to draw a full horizontal line (based on a 72
// MHz clock).  This is the cycle when we start drawing the visible
// part of the screen.
#define START_IMAGE 300

// # bits of color
#define N_COLOR_BITS 9

// Controls the dimensions of the buffer we use as a frame.  Note that
// we don't/can't support full resolution -- at 640 x 480, 9-bit color
// would require ~340KB RAM, and we only have 20KB total, of which
// 17KB is currently usable by a sketch.
#define FRAME_WIDTH 64
#define FRAME_HEIGHT 48        // you want this to be a divisor of 480
#define FRAME_N_PIXELS (FRAME_WIDTH * FRAME_HEIGHT)

// this is (hackishly) used to force delays
volatile uint8 volatile_int = 0;

// vga state
uint16 x = 0;       // X coordinate
uint16 y = 0;       // Y coordinate
uint8 v_active = 1; // Are we in the image?

// The frame buffer.  Each cell is an OR of BIT applied to the color
// bits R0_BIT,R1_BIT,...,B1_BIT,B2_BIT, so we get 9-bit color with 3
// bits for each of R, G, and B.
//
// access like frame[y][x], where frame[0][0] is upper left, and
// frame[FRAME_HEIGHT-1][FRAME_WIDTH-1] is bottom right
uint32 frame[FRAME_HEIGHT][FRAME_WIDTH];
uint32 *frame_row;    // cache our current row at hsync time, for speed

// convenience
uint8 color_pins[] = {R0_PIN, R1_PIN, R2_PIN,
                      G0_PIN, G1_PIN, G2_PIN,
                      B0_PIN, B1_PIN, B2_PIN};

/* Color wheel state */

// 12-color color wheel
#define N_COLOR_WHEEL_COLORS 12
uint32 bsrr_color_wheel[] = \
    {BSRR_RED,
     COLOR_TO_BSRR(C_R2 | C_R1 | C_B0), // rose
     BSRR_MAGENTA,
     COLOR_TO_BSRR(C_R0 | C_B1 | C_B2), // violet
     BSRR_BLUE,
     COLOR_TO_BSRR(C_B2 | C_B1 | C_G0), // azure
     BSRR_CYAN,
     COLOR_TO_BSRR(C_B0 | C_G1 | C_G2), // spring green
     BSRR_GREEN,
     COLOR_TO_BSRR(C_G2 | C_G1 | C_R0), // chartreuse green
     BSRR_YELLOW,
     COLOR_TO_BSRR(C_R2 | C_R1 | C_G0) // orange
    };
int current_color_idx = 0;          // start red
uint32 current_color = 0;
int color_count = 0;
#define COLOR_OVERFLOW 20

// number of framebuffer pixels to update per scan line in update_frame()
#define CHUNK_SIZE 70

// These interrupt service routines control hsync, vsync, and sending
// the visible part of each scan line to the monitor.
void isr_start_hsync(void);
void isr_back_porch(void);
void isr_draw_line(void);
void isr_front_porch(void);

// This function is called during scan lines 480--489; these lines
// aren't visible in the display, and so it's a good time to compute
// what the next frame should be.
void update_frame(void);

//------------------------------ setup()/loop() -------------------------------

void setup() {
    // Set up our color output pins
    for (int i = 0; i < N_COLOR_BITS; i++) {
        pinMode(color_pins[i], OUTPUT);
        // we can use digitalWrite here, because we aren't driving the
        // display yet, and this only gets run once.  We use the (much
        // faster) VGA_SET_BSRR from within the inner loop of each
        // horizontal scan line.
        digitalWrite(color_pins[i], LOW);
    }

    // Set up hsync/vsync pins.
    pinMode(HS_PIN, OUTPUT);
    pinMode(VS_PIN, OUTPUT);
    digitalWrite(HS_PIN, HIGH);
    digitalWrite(VS_PIN, HIGH);

    // USB and SysTick each fire interrupts, the handling of which
    // causes jitter and other interrupt artifacts in our display.
    // Turning them off lets us get cleaner video.
    SerialUSB.end();
    systick_disable();

    // Fill the frame with black.
    for (int y = 0; y < FRAME_HEIGHT; y++) {
        for (int x = 0; x < FRAME_WIDTH; x++) {
            frame[y][x] = VGA_SET_BSRR(BSRR_BLACK);
        }
    }

    // Configure the interrupts which handle a scan line.
    Timer4.pause(); // while we configure
    Timer4.setPrescaleFactor(1);     // Full speed
    Timer4.setChannel1Mode(TIMER_OUTPUTCOMPARE);
    Timer4.setChannel2Mode(TIMER_OUTPUTCOMPARE);
    Timer4.setChannel3Mode(TIMER_OUTPUTCOMPARE);
    Timer4.setChannel4Mode(TIMER_OUTPUTCOMPARE);
    Timer4.setOverflow(2287);   // Total line time

    // we do this in the following order: hsync, back porch, video,
    // front porch.  vsync is handled with a hack in isr_back_porch.
    Timer4.setCompare1(1);      // 0 was causing jitter
    Timer4.attachCompare1Interrupt(isr_start_hsync);
    Timer4.setCompare2(276);
    Timer4.attachCompare2Interrupt(isr_back_porch);
    Timer4.setCompare3(450); // delayed 40ish cycles; improves quality
    Timer4.attachCompare3Interrupt(isr_draw_line);
    Timer4.setCompare4(2266);   // beginning of the right border
    Timer4.attachCompare4Interrupt(isr_front_porch);

    Timer4.setCount(0);         // Ready...
    Timer4.resume();            // Go!
}

void loop() {
    // Everything happens in the interrupts!
}

//--------------------------------- VGA ISRs ----------------------------------

// Start horizonal sync
void isr_start_hsync(void) {
    VGA_HS_LOW;
}

// This ISR:
// - ends horizontal sync for most of the image,
// - sets up the vertical sync for higher line counts, and
// - passes out computation time to update the framebuffer when we're
//   past the visible lines.
void isr_back_porch(void) {
    // end hsync
    VGA_HS_HIGH;

    y++;
    frame_row = frame[y / (480/FRAME_HEIGHT)];

    // if we're about to draw a frame, stop now
    if (y < 480) return;

    switch (y) {
    case 480:
        v_active = 0;           // stop drawing
        break;
    case 490:
        VGA_VS_LOW;             // start vsync
        break;
    case 492:
        VGA_VS_HIGH;            // end vsync
        break;
    case 523:                   // Back to the top
        y = 0;
        v_active = 1;
        frame_row = frame[0];
        return;                 // (we're about to draw)
    default:
        break;
    }

    // give lines 479--522 to advancing the frame, one line at a time.
    // probably there's a smarter way to do this so that hsync will
    // interrupt us while we're thinking, but this works okay so long
    // as the frame update computation can be split into line-sized
    // chunks.
    update_frame();
}

// This is the main horizontal sweep
void isr_draw_line(void) {
    // Skip if we're not in the image at all
    if(!v_active) { return; }

    // draw a white border.  for loop setup makes this appear as a band.
    VGA_SET_BSRR(BSRR_WHITE);

    if (5 < y && y < 475) {
        // delay here as it's the likely branch
        volatile_int++;
        volatile_int++;
        volatile_int++;
        volatile_int++;

        // main loop: draw the contents of the frame as fast as we can.
        // use VGA_BIT so per-iteration time is deterministic.
        for(x = 0; x < FRAME_WIDTH; x++) {
            VGA_SET_BSRR(frame_row[x]);
        }
    } else {
        for(x = 0; x < FRAME_WIDTH; x++) {
            VGA_SET_BSRR(BSRR_WHITE);
        }
    }

    // delay a bit so the last pixel in the row doesn't look so narrow
    volatile_int++;

    // then draw another white border
    VGA_SET_BSRR(BSRR_WHITE);
    volatile_int++;
    volatile_int++;
    volatile_int++;
    volatile_int++;

    // and black out the rest
    VGA_SET_BSRR(BSRR_BLACK);
}

void isr_front_porch(void) {
    // VGA standard says we should black out what's left
    VGA_SET_BSRR(BSRR_BLACK);
}

//------------------------------- frame update --------------------------------

void update_frame_chunk(int chunk) {
    int start = chunk * CHUNK_SIZE;
    int end = start + CHUNK_SIZE;
    uint32* flat_frame = (uint32*)frame;

    if (end > FRAME_N_PIXELS) end = FRAME_N_PIXELS;

    for (int i = start; i < end; i++) {
        flat_frame[i] = current_color;
    }
}

void update_frame(void) {
    static int chunk = 0;
    // we have lines 480--522 to do our thinking.
    switch (y) {
    case 480:
        // put this here so we only do it once per frame
        if (++color_count < COLOR_OVERFLOW) {
            return;
        } else {
            color_count = 0;
            chunk = 0;
            if (++current_color_idx >= N_COLOR_WHEEL_COLORS)
                current_color_idx = 0;
            current_color = bsrr_color_wheel[current_color_idx];
            update_frame_chunk(chunk++);
        }
        break;
    default:
        if (color_count == 0) update_frame_chunk(chunk++);
        break;
    }
}

//---------------------------------- main() -----------------------------------

// Force init to be called *first*, i.e. before static object allocation.
// Otherwise, statically allocated object that need libmaple may fail.
 __attribute__(( constructor )) void premain() {
    init();
}

int main(void) {
    setup();

    while (1) {
        loop();
    }

    return 0;
}
