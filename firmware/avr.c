/*
 * avr.c — AVR ISP driver. See avr.h for wiring and pin assignment.
 *
 * Implements the serial programming instruction set from the ATmega32A
 * datasheet directly. Bit-banged rather than using the SPI peripheral:
 * ISP needs SCK below a quarter of the target's clock, which for a
 * factory ATmega32A on its 1 MHz internal RC means 250 kHz or less.
 * At those speeds the SPI hardware buys nothing, and bit-banging lets
 * avr_isp_enter() retry at several rates without reconfiguring a
 * peripheral each time.
 */

#include <string.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/structs/sio.h"
#include "avr.h"

volatile uint32_t avr_sck_delay_us = 5;   /* ~100 kHz to start */

#define M_SCK   (1u << PIN_SCK)
#define M_MOSI  (1u << PIN_MOSI)
#define M_MISO  (1u << PIN_MISO)
#define M_RESET (1u << PIN_RESET)

const char *avr_result_str(avr_result_t r)
{
    switch (r) {
    case AVR_OK:               return "OK";
    case AVR_ERR_NO_SYNC:      return "NO_SYNC";
    case AVR_ERR_SIGNATURE:    return "SIGNATURE";
    case AVR_ERR_VERIFY:       return "VERIFY";
    case AVR_ERR_TOO_BIG:      return "TOO_BIG";
    case AVR_ERR_FUSE_UNSAFE:  return "FUSE_UNSAFE";
    case AVR_ERR_FUSE_VERIFY:  return "FUSE_VERIFY";
    default:                   return "UNKNOWN";
    }
}

void avr_init(void)
{
    gpio_init(PIN_SCK);
    gpio_init(PIN_MOSI);
    gpio_init(PIN_MISO);
    gpio_init(PIN_RESET);

    gpio_set_dir(PIN_SCK, GPIO_OUT);
    gpio_set_dir(PIN_MOSI, GPIO_OUT);
    gpio_set_dir(PIN_MISO, GPIO_IN);
    gpio_set_dir(PIN_RESET, GPIO_OUT);

    /* Pull-up on MISO so an unconnected target reads as a steady 0xFF
     * rather than noise — that makes "nothing plugged in" a clean,
     * repeatable reading instead of a random one. */
    gpio_pull_up(PIN_MISO);

    /* Leave RESET released so a board plugged in runs normally until we
     * explicitly take it into programming mode. */
    sio_hw->gpio_set = M_RESET;
    sio_hw->gpio_clr = M_SCK | M_MOSI;
}

/* One SPI byte, MSB first, mode 0: MOSI changes while SCK is low, the
 * target samples on the rising edge, and MISO is valid to read there. */
static uint8_t spi_byte(uint8_t out)
{
    uint8_t in = 0;

    for (int i = 7; i >= 0; i--) {
        if ((out >> i) & 1) sio_hw->gpio_set = M_MOSI;
        else                sio_hw->gpio_clr = M_MOSI;

        busy_wait_us(avr_sck_delay_us);

        sio_hw->gpio_set = M_SCK;
        in = (uint8_t)((in << 1) | ((sio_hw->gpio_in & M_MISO) ? 1 : 0));

        busy_wait_us(avr_sck_delay_us);
        sio_hw->gpio_clr = M_SCK;
    }
    return in;
}

/* A serial programming instruction is always four bytes. The reply that
 * matters is usually the last byte, but the third is what carries the
 * 0x53 echo during programming enable, so all four are returned. */
static void isp_cmd(uint8_t a, uint8_t b, uint8_t c, uint8_t d,
                    uint8_t reply[4])
{
    reply[0] = spi_byte(a);
    reply[1] = spi_byte(b);
    reply[2] = spi_byte(c);
    reply[3] = spi_byte(d);
}

static uint8_t isp_cmd4(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    uint8_t r[4];
    isp_cmd(a, b, c, d, r);
    return r[3];
}

/*
 * The busy flag (poll RDY/BSY) tells us a write has finished. Polling is
 * much faster than always waiting the datasheet's worst-case time, but a
 * bounded fallback delay is kept in case a part never clears the flag.
 */
static void wait_ready(uint32_t max_ms)
{
    absolute_time_t deadline = make_timeout_time_ms(max_ms);
    for (;;) {
        if ((isp_cmd4(0xF0, 0x00, 0x00, 0x00) & 1) == 0) return;
        if (absolute_time_diff_us(get_absolute_time(), deadline) < 0) return;
        sleep_ms(1);
    }
}

avr_result_t avr_isp_enter(void)
{
    /*
     * Try progressively slower clocks. A factory ATmega32A runs from its
     * 1 MHz internal RC, capping SCK at 250 kHz; a board with an 8 or
     * 16 MHz crystal will sync at the first, fastest setting. Starting
     * fast and backing off means neither case needs the user to know
     * which they have.
     */
    static const uint32_t delays[] = { 1, 5, 20, 50 };   /* µs per edge */

    for (size_t attempt = 0; attempt < count_of(delays); attempt++) {
        avr_sck_delay_us = delays[attempt];

        /*
         * Datasheet entry sequence: with SCK held low, pulse RESET, wait
         * at least 20 ms, then send Programming Enable. The pulse matters
         * — a target already held in reset may not latch the mode.
         */
        sio_hw->gpio_clr = M_SCK;
        sio_hw->gpio_set = M_RESET;
        sleep_ms(2);
        sio_hw->gpio_clr = M_RESET;
        sleep_ms(30);

        /* Programming Enable: 0xAC 0x53 x x, with 0x53 echoed in byte 3. */
        for (int retry = 0; retry < 3; retry++) {
            uint8_t r[4];
            isp_cmd(0xAC, 0x53, 0x00, 0x00, r);
            if (r[2] == 0x53) {
                return AVR_OK;
            }
            /* Give SCK a single pulse and retry — this re-aligns a target
             * whose bit counter started out of phase with ours. */
            sio_hw->gpio_set = M_SCK;
            busy_wait_us(avr_sck_delay_us);
            sio_hw->gpio_clr = M_SCK;
            sleep_ms(1);
        }
    }
    return AVR_ERR_NO_SYNC;
}

void avr_isp_leave(void)
{
    sio_hw->gpio_clr = M_SCK | M_MOSI;
    sio_hw->gpio_set = M_RESET;    /* release: the target starts running */
    sleep_ms(2);
}

void avr_read_signature(uint8_t sig[3])
{
    for (int i = 0; i < 3; i++) {
        sig[i] = isp_cmd4(0x30, 0x00, (uint8_t)i, 0x00);
    }
}

avr_result_t avr_read_signature_stable(uint8_t sig[3], int *attempts_used)
{
    /* Same ladder avr_isp_enter() uses: fastest first, then progressively
     * more margin. A board that only reads reliably at 50 us per edge is
     * telling you something about its wiring. */
    static const uint32_t delays[] = { 1, 5, 20, 50 };
    uint8_t a[3], b[3];

    for (size_t i = 0; i < count_of(delays); i++) {
        avr_sck_delay_us = delays[i];

        avr_read_signature(a);
        avr_read_signature(b);

        bool agree = (a[0] == b[0] && a[1] == b[1] && a[2] == b[2]);
        bool plausible = !(a[0] == 0xFF && a[1] == 0xFF && a[2] == 0xFF) &&
                         !(a[0] == 0x00 && a[1] == 0x00 && a[2] == 0x00);

        if (agree && plausible) {
            sig[0] = a[0]; sig[1] = a[1]; sig[2] = a[2];
            if (attempts_used) *attempts_used = (int)i + 1;
            return AVR_OK;
        }

        /* Re-enter programming mode before the next try: a failed read can
         * leave the target's bit counter out of phase with ours, and a
         * fresh Programming Enable resynchronises it. */
        avr_isp_leave();
        if (avr_isp_enter() != AVR_OK) {
            if (attempts_used) *attempts_used = (int)i + 1;
            return AVR_ERR_NO_SYNC;
        }
    }

    if (attempts_used) *attempts_used = (int)count_of(delays);
    return AVR_ERR_NO_SYNC;
}

void avr_read_fuses(uint8_t *lfuse, uint8_t *hfuse, uint8_t *lock)
{
    if (lfuse) *lfuse = isp_cmd4(0x50, 0x00, 0x00, 0x00);
    if (hfuse) *hfuse = isp_cmd4(0x58, 0x08, 0x00, 0x00);
    if (lock)  *lock  = isp_cmd4(0x58, 0x00, 0x00, 0x00);
}

fuse_risk_t avr_fuse_risk(uint8_t lfuse, uint8_t hfuse, const char **why)
{
    /*
     * AVR fuses are ACTIVE LOW throughout: 0 means programmed, feature
     * ON. That inversion is the single most common source of bricked
     * parts, which is why both checks below spell out the sense.
     */

    /* SPIEN, hfuse bit 5. A 1 means unprogrammed, switching ISP off for
     * good. This is the real one-way door — no override accepts it. */
    if ((hfuse >> 5) & 1) {
        if (why) *why = "SPIEN unprogrammed switches ISP off permanently";
        return FUSE_FATAL;
    }

    /*
     * CKSEL, lfuse bits 3:0. 0x1..0x4 are the internal RC oscillator and
     * always work. Anything else needs an external clock or crystal —
     * correct on a board that has one, dead on a board that does not.
     *
     * Recoverable either way by feeding a square wave into XTAL1, so this
     * warrants confirmation rather than refusal: blocking it outright
     * would make a legitimate crystal board unprogrammable.
     */
    uint8_t cksel = lfuse & 0x0F;
    if (cksel < 0x1 || cksel > 0x4) {
        if (why) *why = "CKSEL needs an external clock or crystal fitted";
        return FUSE_CONFIRM;
    }

    if (why) *why = NULL;
    return FUSE_FINE;
}

avr_result_t avr_write_fuses(uint8_t lfuse, uint8_t hfuse, bool confirmed)
{
    const char *why = NULL;
    fuse_risk_t risk = avr_fuse_risk(lfuse, hfuse, &why);

    /* FATAL is refused unconditionally; `confirmed` only waives CONFIRM. */
    if (risk == FUSE_FATAL) return AVR_ERR_FUSE_UNSAFE;
    if (risk == FUSE_CONFIRM && !confirmed) return AVR_ERR_FUSE_UNSAFE;

    isp_cmd4(0xAC, 0xA0, 0x00, lfuse);
    wait_ready(20);
    isp_cmd4(0xAC, 0xA8, 0x00, hfuse);
    wait_ready(20);

    /*
     * Read back before reporting success. A fuse write that silently did
     * not take is worse than one that failed loudly — the board would run
     * at an unexpected clock with nothing to show why.
     */
    uint8_t rl = 0, rh = 0;
    avr_read_fuses(&rl, &rh, NULL);
    if (rl != lfuse || rh != hfuse) return AVR_ERR_FUSE_VERIFY;

    return AVR_OK;
}

void avr_chip_erase(void)
{
    isp_cmd4(0xAC, 0x80, 0x00, 0x00);
    wait_ready(50);
    /* The datasheet requires leaving and re-entering programming mode
     * after an erase on some parts; a fresh Programming Enable is
     * cheaper than diagnosing why the first page did not take. */
    isp_cmd4(0xAC, 0x53, 0x00, 0x00);
}

void avr_write_page(uint16_t page_word_addr, const uint8_t *data, uint16_t len)
{
    if (len > ATMEGA32_PAGE) len = ATMEGA32_PAGE;

    /* Fill the page buffer a word at a time. AVR flash is word-organised,
     * so each word needs a low-byte and a high-byte load. */
    for (uint16_t w = 0; w < ATMEGA32_PAGE_WORDS; w++) {
        uint16_t lo_i = (uint16_t)(w * 2);
        uint16_t hi_i = (uint16_t)(w * 2 + 1);

        /* Pad a short final page with 0xFF, which is the erased state —
         * writing anything else would needlessly program cells. */
        uint8_t lo = (lo_i < len) ? data[lo_i] : 0xFF;
        uint8_t hi = (hi_i < len) ? data[hi_i] : 0xFF;

        isp_cmd4(0x40, 0x00, (uint8_t)(w & 0x3F), lo);   /* load low  */
        isp_cmd4(0x48, 0x00, (uint8_t)(w & 0x3F), hi);   /* load high */
    }

    /* Commit the buffer. The address here is the page's word address. */
    isp_cmd4(0x4C,
             (uint8_t)((page_word_addr >> 8) & 0xFF),
             (uint8_t)(page_word_addr & 0xFF),
             0x00);
    wait_ready(20);
}

uint8_t avr_read_flash_byte(uint32_t byte_addr)
{
    uint32_t word = byte_addr >> 1;
    uint8_t op = (byte_addr & 1) ? 0x28 : 0x20;   /* high byte : low byte */

    return isp_cmd4(op,
                    (uint8_t)((word >> 8) & 0xFF),
                    (uint8_t)(word & 0xFF),
                    0x00);
}

avr_result_t avr_verify(const uint8_t *data, uint32_t len, uint32_t *bad_addr)
{
    for (uint32_t a = 0; a < len; a++) {
        if (avr_read_flash_byte(a) != data[a]) {
            if (bad_addr) *bad_addr = a;
            return AVR_ERR_VERIFY;
        }
    }
    return AVR_OK;
}
