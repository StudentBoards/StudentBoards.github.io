/*
 * jtag.h — JTAG TAP driver for RP2040
 *
 * Target: Altera/Intel MAX V 5M40ZE64 (and the rest of the MAX V family).
 *
 * WIRING — Pico to the CPLD board's 64-pin DIL package:
 *
 *     Pico phys  GPIO          CPLD board
 *     ---------  ----          ----------
 *         4      GP2    ->     pin 14   TMS
 *         5      GP3    ->     pin 15   TDI
 *         6      GP4    ->     pin 16   TCK
 *         7      GP5    <-     pin 17   TDO   (input)
 *         3      GND    --     GND
 *
 * THE SIGNAL ORDER HERE IS NOT ARBITRARY. Pico pins 4,5,6,7 map to board
 * pins 14,15,16,17 in the same order, so the four jumpers run straight
 * across with no crossovers. Students wire this from a photograph; a cable
 * that cannot be plugged in twisted is worth more than any error message.
 * Reordering these defines to something "logical" like TCK first breaks
 * that property — the driver bit-bangs SIO and does not care, but the
 * student does.
 *
 * A side benefit: TDI and TDO used to be neighbours at both ends, which
 * made swapping them the classic failure. They are now two apart on each
 * side, so the easy mistake is harder to make.
 *
 * MAX V JTAG pins sit in bank 1 and run at that bank's VCCIO. On a 3.3 V
 * board that matches RP2040 GPIO directly, so no level shifter is needed.
 * If your bank 1 runs at 1.8 V or 2.5 V you do need one — and note that a
 * TXS0108E is a poor choice here, because MAX V boards commonly fit a 1 k
 * pulldown on TCK which drags an auto-direction shifter's output down.
 *
 * Do not power the target from the Pico's 3V3 pin for anything beyond a
 * bare CPLD; share ground and give the target its own supply.
 */

#ifndef JTAG_H
#define JTAG_H

#include <stdbool.h>
#include <stdint.h>

/* ---- Pin assignment ------------------------------------------------- */
/* Ordered to match the board's pin order — see WIRING above before
 * changing these. */
#define PIN_TMS  2
#define PIN_TDI  3
#define PIN_TCK  4
#define PIN_TDO  5

/* ---- TAP states ----------------------------------------------------- */
typedef enum {
    TAP_RESET = 0,
    TAP_IDLE,
    TAP_DRSELECT,
    TAP_DRCAPTURE,
    TAP_DRSHIFT,
    TAP_DREXIT1,
    TAP_DRPAUSE,
    TAP_DREXIT2,
    TAP_DRUPDATE,
    TAP_IRSELECT,
    TAP_IRCAPTURE,
    TAP_IRSHIFT,
    TAP_IREXIT1,
    TAP_IRPAUSE,
    TAP_IREXIT2,
    TAP_IRUPDATE,
    TAP_STATE_COUNT
} tap_state_t;

/* Initialise pins and enter Test-Logic-Reset. */
void jtag_init(void);

/*
 * One TCK cycle.
 *
 * TMS/TDI are set up while TCK is low; the target samples them on the
 * rising edge. TDO is sampled just before that rising edge, because the
 * target updates TDO on the falling edge.
 */
bool jtag_clock(bool tms, bool tdi, bool read_tdo);

/* Five clocks with TMS high reaches Test-Logic-Reset from any state,
 * which is why no nTRST pin is needed. */
void jtag_reset(void);

/* Walk the TAP to `target` by the shortest TMS path. */
void jtag_goto(tap_state_t target);

/* Current tracked TAP state. */
tap_state_t jtag_state(void);

/* Hold in `state` for `clocks` TCK cycles (SVF RUNTEST). */
void jtag_run_test(uint32_t clocks, tap_state_t state);

/* Look up a TAP state by its SVF name. Returns false if unrecognised. */
bool jtag_state_from_name(const char *name, tap_state_t *out);

/*
 * Shift `nbits` LSB-first through the current shift register.
 *
 * tdi/tdo/mask are little-endian byte arrays; tdo and mask may be NULL to
 * skip the readback check. `end` is the state to finish in.
 *
 * Returns true on success, false if a TDO comparison failed. On failure
 * *fail_bit is set to the index of the first mismatching bit, which is far
 * more useful for diagnosis than a bare pass/fail.
 */
bool jtag_shift(uint32_t nbits,
                const uint8_t *tdi,
                const uint8_t *tdo_expect,
                const uint8_t *mask,
                tap_state_t shift_state,
                tap_state_t end,
                uint32_t *fail_bit,
                uint8_t *tdo_got);

/*
 * Bytes of captured TDO that jtag_shift() will write back through
 * `tdo_got`. Reporting what actually came back is what separates the two
 * causes of a mismatch that otherwise look identical: all-ones or
 * all-zeros means the device stopped driving TDO, while a plausible but
 * different value means the device is fine and the file is for a
 * different part. A bit index alone cannot tell those apart.
 */
#define JTAG_TDO_CAPTURE_BYTES 8

/*
 * Read the 32-bit IDCODE of a single device.
 *
 * After Test-Logic-Reset every TAP loads IDCODE into DR automatically, so
 * this needs no instruction shift. Returns 0 or 0xFFFFFFFF when nothing is
 * connected — a floating TDO with the internal pull-up reads as all ones.
 */
uint32_t jtag_read_idcode(void);

/* Half-period padding per TCK edge, in microseconds. 0 runs as fast as
 * the CPU manages. Raise to 1-2 for long or unshielded leads. */
extern volatile uint32_t jtag_edge_delay_us;

#endif /* JTAG_H */
