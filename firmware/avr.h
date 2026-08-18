/*
 * avr.h — AVR ISP (SPI serial programming) for RP2040/RP2350.
 *
 * Target: ATmega32A on the studentboards AVR dev board.
 *
 * WIRING — a second 6-pin block, immediately below the JTAG one, so one
 * Pico serves both board types with two separate cables and no mode
 * switch to get wrong:
 *
 *     Physical  Pico    AVR board
 *     --------  ------  -------------
 *        8      GND     GND
 *        9      GP6     SCK    (ATmega32A pin PB7)
 *       10      GP7     MOSI   (PB5)
 *       11      GP8     MISO   (PB6)
 *       12      GP9     RESET  (pin 9)
 *       13      GND     GND
 *
 * Physical pin 8 is the GND shared with the JTAG block above, so pins
 * 3..13 form one contiguous strip: JTAG, ground, ISP.
 *
 * Pico GPIO is 3.3 V and not 5 V tolerant. An ATmega32A running at 5 V
 * will drive MISO at 5 V and damage the Pico — only connect a board
 * running at 3.3 V, or add a level shifter.
 *
 * CLOCK SPEED MATTERS HERE. ISP requires SCK below one quarter of the
 * target's clock. An ATmega32A on its default 1 MHz internal RC can only
 * accept SCK up to 250 kHz, so avr_isp_enter() starts slow and only
 * speeds up once it has synchronised — a fixed fast clock is the usual
 * reason ISP "doesn't work" on a factory-fresh part.
 */

#ifndef AVR_H
#define AVR_H

#include <stdbool.h>
#include <stdint.h>

/* ---- Pin assignment ------------------------------------------------- */
#define PIN_SCK    6
#define PIN_MOSI   7
#define PIN_MISO   8
#define PIN_RESET  9

/* ---- ATmega32A geometry --------------------------------------------- */
#define ATMEGA32_SIG_0      0x1E
#define ATMEGA32_SIG_1      0x95
#define ATMEGA32_SIG_2      0x02
#define ATMEGA32_FLASH      32768   /* bytes */
#define ATMEGA32_PAGE       128     /* bytes = 64 words */
#define ATMEGA32_PAGE_WORDS (ATMEGA32_PAGE / 2)

typedef enum {
    AVR_OK = 0,
    AVR_ERR_NO_SYNC,        /* programming enable never echoed back */
    AVR_ERR_SIGNATURE,      /* answered, but not a part we know */
    AVR_ERR_VERIFY,         /* readback differed from what we wrote */
    AVR_ERR_TOO_BIG,        /* image larger than the target's flash */
    AVR_ERR_FUSE_UNSAFE,    /* refused: would disable ISP permanently */
    AVR_ERR_FUSE_VERIFY,    /* fuse readback differed */
} avr_result_t;

const char *avr_result_str(avr_result_t r);

/* Initialise pins. Call once at startup. */
void avr_init(void);

/*
 * Enter serial programming mode.
 *
 * Tries progressively slower SCK rates until the target echoes 0x53, so a
 * part on its 1 MHz internal RC works without the user knowing to slow
 * anything down. Returns AVR_OK on success.
 */
avr_result_t avr_isp_enter(void);

/* Release RESET and let the target run. */
void avr_isp_leave(void);

/* Read the three signature bytes once. */
void avr_read_signature(uint8_t sig[3]);

/*
 * Read the signature repeatedly until two consecutive reads agree, backing
 * the clock off between attempts.
 *
 * A signature that comes back garbage is almost never a dead chip — it is
 * a marginal read: SCK too fast for the target's actual clock, a long
 * jumper, or a weak ground. Retrying at the same speed just repeats the
 * same marginal conditions, so each attempt slows SCK down. Requiring two
 * matching reads rather than trusting one also stops a lucky garbage value
 * being reported as a real part.
 *
 * Returns AVR_OK and fills sig[] on success, or AVR_ERR_NO_SYNC.
 * `attempts_used` is optional and reports how many tries it took, which is
 * worth surfacing: succeeding only on the slowest setting means the wiring
 * is marginal even though the result was good.
 */
avr_result_t avr_read_signature_stable(uint8_t sig[3], int *attempts_used);

/* Read lfuse, hfuse and the lock byte. Any pointer may be NULL. */
void avr_read_fuses(uint8_t *lfuse, uint8_t *hfuse, uint8_t *lock);

/*
 * Write fuses, then read back and confirm.
 *
 * Returns AVR_ERR_FUSE_UNSAFE without writing anything if the values
 * would make the part unreachable over ISP. Pass allow_unsafe only if
 * you genuinely mean it — recovery needs a high-voltage programmer.
 */
/*
 * `confirmed` waives the FUSE_CONFIRM check only — it can never authorise
 * a FUSE_FATAL write. Separating them means a student with a genuine
 * crystal board is not blocked, while the setting that actually ends ISP
 * stays unreachable however the request is phrased.
 */
avr_result_t avr_write_fuses(uint8_t lfuse, uint8_t hfuse, bool confirmed);

/*
 * How risky a fuse pair is. The two failure modes are NOT equivalent, and
 * treating them the same either blocks legitimate boards or hides a real
 * one-way door:
 *
 *   FUSE_FATAL   SPIEN (hfuse bit 5) unprogrammed. ISP is switched off
 *                for good. Recovery needs JTAG (only while JTAGEN is
 *                still programmed) or a high-voltage parallel
 *                programmer. Never allowed, with no override.
 *
 *   FUSE_CONFIRM CKSEL (lfuse bits 3:0) selects an external clock or
 *                crystal. Correct on a board that has one fitted, and
 *                dead on a board that does not — but recoverable either
 *                way by feeding a square wave into XTAL1. Allowed with
 *                explicit confirmation.
 *
 *   FUSE_FINE    Nothing notable.
 */
typedef enum {
    FUSE_FINE = 0,
    FUSE_CONFIRM,
    FUSE_FATAL
} fuse_risk_t;

fuse_risk_t avr_fuse_risk(uint8_t lfuse, uint8_t hfuse, const char **why);

/* Erase the whole chip. Required before writing flash: ISP can only
 * clear bits, so a page must be erased before it is rewritten. */
void avr_chip_erase(void);

/* Write one flash page. `len` is in bytes and must not exceed the page
 * size; a short final page is padded with 0xFF (erased state). */
void avr_write_page(uint16_t page_word_addr, const uint8_t *data, uint16_t len);

/* Read one flash byte by byte address. */
uint8_t avr_read_flash_byte(uint32_t byte_addr);

/* Verify `len` bytes of flash against `data`. On mismatch, *bad_addr is
 * set to the first differing byte address. */
avr_result_t avr_verify(const uint8_t *data, uint32_t len, uint32_t *bad_addr);

/* Per-edge delay in microseconds, set by avr_isp_enter(). */
extern volatile uint32_t avr_sck_delay_us;

#endif /* AVR_H */
