/*
 * main.c — standalone MAX V programmer on a Raspberry Pi Pico.
 *
 * Target: Altera/Intel MAX V 5M40ZE64 (works for any single MAX V device).
 *
 * The Pico appears as a USB serial port (CDC). No drivers on Windows 10+,
 * macOS or Linux, and no Quartus programmer configuration — the student
 * runs one Python script and the board programs.
 *
 * WHY NOT EMULATE A USB-BLASTER?
 * Emulating the Blaster gives you native Quartus integration, and the
 * pico-usb-blaster project already does that well. This firmware exists
 * for the other case: a self-contained tool with a one-line command, a
 * clear pass/fail, and no driver install. Pick whichever fits — they run
 * on the same hardware, so you can reflash between them.
 *
 * WIRING (see jtag.h):
 *     GP2 -> TCK,  GP3 -> TMS,  GP4 -> TDI,  GP5 <- TDO,  GND -- GND
 *
 * PROTOCOL (line-based, over USB CDC):
 *     Host -> Pico             Pico -> Host
 *     ---------------------    -------------------------------------
 *     ID                       IDCODE 0x020A50DD  |  ERR NO_TARGET
 *     INFO                     INFO <version/pins/limits>
 *     SPEED <us>               OK
 *     SVF <bytes>              READY, then progress, then DONE / ERR
 *     PING                     PONG
 *
 * After SVF <bytes> the host sends exactly that many raw bytes. The Pico
 * plays them as they arrive rather than buffering, so file size is not
 * limited by the 264 KB of SRAM and programming overlaps the transfer.
 *
 * LEDs (onboard LED only, so the language is timing-based):
 *     slow breathe   idle
 *     fast blink     receiving / programming
 *     solid on       last run passed
 *     3 short + gap  last run failed
 */

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "jtag.h"
#include "svf.h"
#include "avr.h"

#define FW_VERSION "1.0"

/*
 * MAX V IDCODEs.
 *
 * IMPORTANT: MAX V does NOT encode density in the IDCODE. All three parts
 * used on these boards — 5M40ZE64, 5M80ZE64 and 5M160ZE64 — report the
 * SAME value, 0x020A50DD, and being the same E64 package they also share
 * a boundary-scan register length. There is no way to tell them apart
 * over JTAG.
 *
 * So this check confirms "a MAX V of the small-density group is fitted
 * and the JTAG path works" — which is exactly what catches an unpowered
 * board, swapped TDI/TDO, or a missing ground. Identifying WHICH of the
 * three is the SVF's job: the file names its target device in its header,
 * and the host page reads that.
 */
#define IDCODE_MAXV_E64    0x020A50DDu  /* 5M40Z / 5M80Z / 5M160Z / 5M240Z */
#define IDCODE_MAXV_MID    0x020A60DDu  /* 5M240Z(T144) / 5M570Z */
#define IDCODE_MAXV_LARGE  0x020A30DDu  /* 5M1270Z / 5M2210Z */

#define LED_PIN PICO_DEFAULT_LED_PIN

typedef enum { LED_IDLE, LED_BUSY, LED_PASS, LED_FAIL } led_mode_t;
static led_mode_t led_mode = LED_IDLE;

/* ------------------------------------------------------------------ */
/* LED                                                                */
/* ------------------------------------------------------------------ */

static void led_service(void)
{
    uint32_t t = to_ms_since_boot(get_absolute_time());
    bool on = false;

    switch (led_mode) {
    case LED_IDLE:
        /* Slow triangle so idle looks alive but not attention-seeking. */
        on = ((t % 2000) < 1000) ? ((t % 200) < 20) : false;
        break;
    case LED_BUSY:
        on = (t % 120) < 60;
        break;
    case LED_PASS:
        on = true;
        break;
    case LED_FAIL: {
        uint32_t p = t % 1600;
        on = (p < 120) || (p >= 240 && p < 360) || (p >= 480 && p < 600);
        break;
    }
    }
    gpio_put(LED_PIN, on);
}

/* Called by svf.c once per statement. */
void svf_progress_hook(uint32_t statements, uint32_t bits)
{
    (void)bits;
    /* Report sparsely: a line per statement would swamp the link and
     * slow programming more than it informs. */
    if ((statements % 500) == 0) {
        printf("PROGRESS %lu\n", (unsigned long)statements);
    }
    led_service();
}

/* ------------------------------------------------------------------ */
/* Input helpers                                                      */
/* ------------------------------------------------------------------ */

/* Read one line, or return false on timeout. Non-blocking on the LED so
 * the device never looks frozen while waiting. */
static bool read_line(char *buf, size_t maxlen, uint32_t timeout_ms)
{
    size_t n = 0;
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);

    while (true) {
        int c = getchar_timeout_us(1000);
        if (c == PICO_ERROR_TIMEOUT) {
            led_service();
            if (timeout_ms && absolute_time_diff_us(get_absolute_time(),
                                                    deadline) < 0) {
                return false;
            }
            continue;
        }
        if (c == '\r') continue;
        if (c == '\n') {
            buf[n] = '\0';
            return true;
        }
        if (n < maxlen - 1) buf[n++] = (char)c;
    }
}

/* ------------------------------------------------------------------ */
/* Commands                                                           */
/* ------------------------------------------------------------------ */

static const char *idcode_name(uint32_t id)
{
    switch (id) {
    /* This is the group the student boards use. The reply deliberately
     * names all three candidates rather than guessing one, because the
     * silicon genuinely does not distinguish them. */
    case IDCODE_MAXV_E64:   return "MAX V 5M40Z/5M80Z/5M160Z/5M240Z";
    case IDCODE_MAXV_MID:   return "MAX V 5M240Z(T144)/5M570Z";
    case IDCODE_MAXV_LARGE: return "MAX V 5M1270Z/5M2210Z";
    default:                return "unrecognised";
    }
}

static void cmd_id(void)
{
    uint32_t id = jtag_read_idcode();

    /*
     * A floating TDO with the internal pull-up reads as all ones, and a
     * shorted or unpowered one reads as all zeros. Treating both as "no
     * target" rather than as a device with a strange ID is what makes
     * "you forgot to plug it in" a clear message instead of a puzzle.
     */
    if (id == 0xFFFFFFFFu || id == 0x00000000u) {
        printf("ERR NO_TARGET\n");
        return;
    }
    printf("IDCODE 0x%08lX %s\n", (unsigned long)id, idcode_name(id));
}

static void cmd_info(void)
{
    printf("INFO version=%s tck=%d tms=%d tdi=%d tdo=%d "
           "max_shift_bits=%d edge_delay_us=%lu\n",
           FW_VERSION, PIN_TCK, PIN_TMS, PIN_TDI, PIN_TDO,
           SVF_MAX_BITS, (unsigned long)jtag_edge_delay_us);
}

static void cmd_speed(const char *arg)
{
    unsigned long us = strtoul(arg, NULL, 10);
    if (us > 1000) us = 1000;
    jtag_edge_delay_us = (uint32_t)us;
    printf("OK edge_delay_us=%lu\n", us);
}

/*
 * Receive and play an SVF stream of exactly `total` bytes.
 *
 * Every byte is consumed even after an error, so the host's byte count
 * and ours stay in step and the next command lands on a clean boundary.
 * Bailing out early here would leave the tail of the file being parsed as
 * commands, which is a confusing failure to debug.
 */
static void cmd_svf(uint32_t total)
{
    svf_ctx_t ctx;
    svf_init(&ctx);

    led_mode = LED_BUSY;
    printf("READY %lu\n", (unsigned long)total);

    uint8_t chunk[64];
    uint32_t received = 0;
    svf_result_t result = SVF_OK;
    absolute_time_t start = get_absolute_time();

    /*
     * FNV-1a over every byte received, reported back with the result.
     * The host computes the same hash over what it sent, so any mismatch
     * proves bytes were lost or reordered in transit — which otherwise
     * looks exactly like a programming bug and is impossible to tell apart
     * from one by staring at the failure.
     */
    uint32_t rx_hash = 2166136261u;

    while (received < total) {
        size_t want = total - received;
        if (want > sizeof(chunk)) want = sizeof(chunk);

        size_t got = 0;
        absolute_time_t deadline = make_timeout_time_ms(10000);
        while (got < want) {
            int c = getchar_timeout_us(1000);
            if (c == PICO_ERROR_TIMEOUT) {
                led_service();
                if (absolute_time_diff_us(get_absolute_time(), deadline) < 0) {
                    printf("ERR TIMEOUT after %lu of %lu bytes\n",
                           (unsigned long)received, (unsigned long)total);
                    led_mode = LED_FAIL;
                    return;
                }
                continue;
            }
            chunk[got++] = (uint8_t)c;
        }
        for (size_t k = 0; k < got; k++) {
            rx_hash = (rx_hash ^ chunk[k]) * 16777619u;
        }
        received += got;

        if (result == SVF_OK) {
            result = svf_feed(&ctx, chunk, got);
        }
        /* If result is already an error we keep draining, but stop
         * executing — see the note above. */
    }

    if (result == SVF_OK) {
        result = svf_finish(&ctx);
    }

    int64_t ms = absolute_time_diff_us(start, get_absolute_time()) / 1000;

    if (result == SVF_OK) {
        printf("DONE statements=%lu bits=%lu ms=%lld rx=%lu hash=%08lX\n",
               (unsigned long)ctx.statements,
               (unsigned long)ctx.total_bits,
               (long long)ms,
               (unsigned long)received, (unsigned long)rx_hash);
        led_mode = LED_PASS;
    } else {
        printf("ERR %s statement=%lu detail=%s rx=%lu hash=%08lX\n",
               svf_result_str(result),
               (unsigned long)ctx.error_statement,
               ctx.error_detail,
               (unsigned long)received, (unsigned long)rx_hash);
        led_mode = LED_FAIL;
    }
}


/* ------------------------------------------------------------------ */
/* AVR ISP commands                                                   */
/* ------------------------------------------------------------------ */

/*
 * Flash image buffer.
 *
 * The whole ATmega32A image is only 32 KB, so unlike the SVF path there
 * is no reason to stream it: buffering lets us erase once, program every
 * page, and then verify the entire image against the same copy. The
 * RP2350 has 520 KB of SRAM, so this costs nothing worth saving.
 */
static uint8_t avr_image[ATMEGA32_FLASH];

static const char *avr_part_name(const uint8_t sig[3])
{
    if (sig[0] == ATMEGA32_SIG_0 && sig[1] == ATMEGA32_SIG_1 &&
        sig[2] == ATMEGA32_SIG_2) {
        return "ATmega32/ATmega32A";
    }
    return NULL;
}

/*
 * Detect the AVR, retrying rather than reporting the first bad read.
 *
 * An intermittent "unknown" signature is a marginal link, not a dead
 * chip, so a single attempt gives a misleading answer roughly as often as
 * a correct one. This tries the whole enter-and-read cycle several times,
 * and avr_read_signature_stable() slows SCK down within each try.
 *
 * The reply carries the attempt count so a board that only reads at the
 * slowest setting can be flagged as marginal even when the result is
 * correct — that is the board whose programming will fail later.
 */
static void cmd_avr_id(void)
{
    const int MAX_CYCLES = 3;
    uint8_t sig[3];
    int used = 0;

    for (int cycle = 1; cycle <= MAX_CYCLES; cycle++) {
        if (avr_isp_enter() != AVR_OK) {
            avr_isp_leave();
            sleep_ms(50);
            continue;
        }

        avr_result_t r = avr_read_signature_stable(sig, &used);
        avr_isp_leave();

        if (r == AVR_OK) {
            const char *name = avr_part_name(sig);
            /* "retries" counts extra effort, so 0 means it read cleanly
             * first time at full speed. */
            printf("SIG 0x%02X%02X%02X %s retries=%d\n",
                   sig[0], sig[1], sig[2],
                   name ? name : "unknown AVR",
                   (cycle - 1) * 4 + (used - 1));
            return;
        }
        sleep_ms(50);
    }

    printf("ERR NO_TARGET after %d attempts\n", MAX_CYCLES);
}

static void cmd_avr_fuses(void)
{
    if (avr_isp_enter() != AVR_OK) {
        printf("ERR NO_TARGET\n");
        return;
    }
    uint8_t l = 0, h = 0, lock = 0;
    avr_read_fuses(&l, &h, &lock);
    avr_isp_leave();

    const char *why = NULL;
    fuse_risk_t risk = avr_fuse_risk(l, h, &why);
    const char *level = (risk == FUSE_FATAL) ? "fatal"
                      : (risk == FUSE_CONFIRM) ? "confirm" : "fine";

    printf("FUSES lfuse=0x%02X hfuse=0x%02X lock=0x%02X risk=%s%s%s\n",
           l, h, lock, level,
           why ? " why=" : "", why ? why : "");
}

/*
 * AVRFUSEW <lfuse> <hfuse> [CONFIRM]
 *
 * CONFIRM waives only the external-clock check, and never authorises a
 * write that would switch ISP off. Callers must ask for it explicitly, so
 * a student cannot reach that state by clicking through a dialog.
 */
static void cmd_avr_fusew(const char *arg)
{
    unsigned int l = 0, h = 0;
    char extra[16] = { 0 };
    int n = sscanf(arg, "%x %x %15s", &l, &h, extra);

    if (n < 2) {
        printf("ERR BAD_ARGS expected: AVRFUSEW <lfuse> <hfuse> [CONFIRM]\n");
        return;
    }
    bool confirmed = (n >= 3 && strcmp(extra, "CONFIRM") == 0);

    const char *why = NULL;
    fuse_risk_t risk = avr_fuse_risk((uint8_t)l, (uint8_t)h, &why);

    if (risk == FUSE_FATAL) {
        printf("ERR FUSE_FATAL %s\n", why ? why : "would disable ISP");
        return;
    }
    if (risk == FUSE_CONFIRM && !confirmed) {
        printf("ERR FUSE_CONFIRM %s\n", why ? why : "needs confirmation");
        return;
    }

    if (avr_isp_enter() != AVR_OK) {
        printf("ERR NO_TARGET\n");
        return;
    }

    /* Read the old values first so the reply can report what changed —
     * useful for undoing a change that turns out to be wrong. */
    uint8_t ol = 0, oh = 0;
    avr_read_fuses(&ol, &oh, NULL);

    avr_result_t r = avr_write_fuses((uint8_t)l, (uint8_t)h, confirmed);
    avr_isp_leave();

    if (r != AVR_OK) {
        printf("ERR %s\n", avr_result_str(r));
        return;
    }
    printf("FUSEOK lfuse=0x%02X hfuse=0x%02X was lfuse=0x%02X hfuse=0x%02X\n",
           l, h, ol, oh);
}

/*
 * Receive a raw flash image and program it.
 *
 * The host sends a flat binary — Intel HEX parsing happens in the
 * browser, where memory is plentiful and the parsing is trivial. That
 * keeps address-record and checksum handling out of the firmware
 * entirely.
 */
static void cmd_avr_flash(uint32_t total)
{
    if (total > ATMEGA32_FLASH) {
        printf("ERR TOO_BIG %lu bytes exceeds %d\n",
               (unsigned long)total, ATMEGA32_FLASH);
        return;
    }

    led_mode = LED_BUSY;
    printf("READY %lu\n", (unsigned long)total);

    uint32_t received = 0;
    while (received < total) {
        int c = getchar_timeout_us(1000);
        if (c == PICO_ERROR_TIMEOUT) {
            led_service();
            continue;
        }
        avr_image[received++] = (uint8_t)c;
    }

    absolute_time_t start = get_absolute_time();

    if (avr_isp_enter() != AVR_OK) {
        printf("ERR NO_TARGET\n");
        led_mode = LED_FAIL;
        return;
    }

    /* Confirm the part before erasing. An erase against the wrong or
     * absent device is the one irreversible step in this sequence. */
    uint8_t sig[3];
    int sig_tries = 0;
    if (avr_read_signature_stable(sig, &sig_tries) != AVR_OK ||
        !avr_part_name(sig)) {
        avr_isp_leave();
        printf("ERR SIGNATURE 0x%02X%02X%02X is not an ATmega32A\n",
               sig[0], sig[1], sig[2]);
        led_mode = LED_FAIL;
        return;
    }

    avr_chip_erase();

    uint32_t pages = (total + ATMEGA32_PAGE - 1) / ATMEGA32_PAGE;
    for (uint32_t p = 0; p < pages; p++) {
        uint32_t off = p * ATMEGA32_PAGE;
        uint32_t len = total - off;
        if (len > ATMEGA32_PAGE) len = ATMEGA32_PAGE;

        avr_write_page((uint16_t)(off / 2), avr_image + off, (uint16_t)len);

        if ((p % 16) == 0) {
            printf("PROGRESS %lu\n", (unsigned long)(p * ATMEGA32_PAGE));
            led_service();
        }
    }

    uint32_t bad = 0;
    avr_result_t v = avr_verify(avr_image, total, &bad);
    avr_isp_leave();

    int64_t ms = absolute_time_diff_us(start, get_absolute_time()) / 1000;

    if (v != AVR_OK) {
        printf("ERR VERIFY mismatch at byte %lu\n", (unsigned long)bad);
        led_mode = LED_FAIL;
        return;
    }

    printf("DONE bytes=%lu pages=%lu ms=%lld\n",
           (unsigned long)total, (unsigned long)pages, (long long)ms);
    led_mode = LED_PASS;
}

/*
 * AVRVERIFY <bytes> — compare flash against a hex image WITHOUT writing.
 *
 * Distinct from the verify that follows programming: this one never
 * erases, so it answers "is this board already running the right build?"
 * on a board you do not want to disturb. Useful for checking a batch, or
 * for confirming a suspect board before reflashing it.
 */
static void cmd_avr_verify(uint32_t total)
{
    if (total > ATMEGA32_FLASH) {
        printf("ERR TOO_BIG %lu bytes exceeds %d\n",
               (unsigned long)total, ATMEGA32_FLASH);
        return;
    }

    led_mode = LED_BUSY;
    printf("READY %lu\n", (unsigned long)total);

    uint32_t received = 0;
    while (received < total) {
        int c = getchar_timeout_us(1000);
        if (c == PICO_ERROR_TIMEOUT) { led_service(); continue; }
        avr_image[received++] = (uint8_t)c;
    }

    absolute_time_t start = get_absolute_time();

    if (avr_isp_enter() != AVR_OK) {
        printf("ERR NO_TARGET\n");
        led_mode = LED_FAIL;
        return;
    }

    uint8_t sig[3];
    int used = 0;
    if (avr_read_signature_stable(sig, &used) != AVR_OK || !avr_part_name(sig)) {
        avr_isp_leave();
        printf("ERR SIGNATURE could not identify the chip\n");
        led_mode = LED_FAIL;
        return;
    }

    /*
     * Compare byte by byte and count every difference rather than stopping
     * at the first. One mismatch usually means a bad read; thousands means
     * the board is running a different program entirely — and the caller
     * can only tell those apart if we keep counting.
     */
    uint32_t diffs = 0, first_bad = 0;
    for (uint32_t a = 0; a < total; a++) {
        if (avr_read_flash_byte(a) != avr_image[a]) {
            if (diffs == 0) first_bad = a;
            diffs++;
        }
        if ((a % 2048) == 0) {
            printf("PROGRESS %lu\n", (unsigned long)a);
            led_service();
        }
    }
    avr_isp_leave();

    int64_t ms = absolute_time_diff_us(start, get_absolute_time()) / 1000;

    if (diffs == 0) {
        printf("DONE match bytes=%lu ms=%lld\n",
               (unsigned long)total, (long long)ms);
        led_mode = LED_PASS;
        return;
    }

    printf("ERR MISMATCH %lu of %lu bytes differ, first at 0x%04lX\n",
           (unsigned long)diffs, (unsigned long)total,
           (unsigned long)first_bad);
    led_mode = LED_FAIL;
}

/*
 * DIAG — exercise the JTAG primitives independently of any SVF.
 *
 * When an SVF fails partway through, the useful question is whether the
 * basic TAP machinery works at all. These four checks use different
 * registers and different instructions, so agreement between them is real
 * evidence the wiring and state machine are sound — which narrows a
 * failure down to the programming sequence rather than the plumbing.
 */
static void cmd_diag(void)
{
    printf("DIAG start\n");

    /* 1. IDCODE, loaded automatically by Test-Logic-Reset. */
    uint32_t id = jtag_read_idcode();
    printf("DIAG idcode 0x%08lX %s\n", (unsigned long)id, idcode_name(id));

    /*
     * 2. Capture-IR must load a value whose two least significant bits are
     * '01' — IEEE 1149.1 mandates it. Reading that back proves we can
     * navigate to Shift-IR and sample TDO correctly, with no dependence on
     * any vendor instruction.
     */
    jtag_reset();
    jtag_goto(TAP_IRSHIFT);
    uint32_t cap = 0;
    for (int i = 0; i < 10; i++) {
        if (jtag_clock(i == 9, true, true)) cap |= (1u << i);
    }
    jtag_goto(TAP_IDLE);
    printf("DIAG ircapture 0x%03lX %s\n", (unsigned long)(cap & 0x3FF),
           ((cap & 3) == 1) ? "ok" : "BAD (expected low bits 01)");

    /* 3. Total IR length: 10 for a single MAX V. */
    int irlen = -1;
    {
        jtag_reset();
        jtag_goto(TAP_IRSHIFT);
        for (int i = 0; i < 64; i++) jtag_clock(false, true, false);
        for (int i = 1; i <= 65; i++) {
            if (!jtag_clock(false, false, true)) { irlen = i - 1; break; }
        }
        jtag_goto(TAP_IDLE);
        jtag_reset();
    }
    printf("DIAG irlen %d %s\n", irlen, (irlen == 10) ? "ok" : "unexpected");

    /*
     * 4. BYPASS shift delay. All-ones in IR selects BYPASS on every device
     * (IEEE mandates it), giving a one-bit register per device. Pushing a
     * marker through and counting clocks gives the device count from a
     * completely different path than the IDCODE scan above.
     */
    int devices = -1;
    {
        jtag_reset();
        jtag_goto(TAP_IRSHIFT);
        for (int i = 0; i < 64; i++) jtag_clock(false, true, false);
        jtag_goto(TAP_DRSHIFT);
        for (int i = 0; i < 40; i++) jtag_clock(false, false, false);
        for (int i = 1; i <= 40; i++) {
            if (jtag_clock(false, i == 1, true)) { devices = i - 1; break; }
        }
        jtag_goto(TAP_IDLE);
        jtag_reset();
    }
    printf("DIAG bypass %d %s\n", devices,
           (devices == 1) ? "ok (single device)" : "unexpected");

    printf("DIAG done\n");
}

/* ------------------------------------------------------------------ */
/* Main                                                               */
/* ------------------------------------------------------------------ */

int main(void)
{
    stdio_init_all();

    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);

    jtag_init();
    avr_init();

    char line[64];

    while (true) {
        led_service();

        if (!read_line(line, sizeof(line), 0)) {
            continue;
        }

        /* Uppercase the verb so the protocol is case-insensitive. */
        char verb[16];
        size_t i = 0;
        while (line[i] && line[i] != ' ' && i < sizeof(verb) - 1) {
            verb[i] = (char)toupper((unsigned char)line[i]);
            i++;
        }
        verb[i] = '\0';
        const char *arg = line[i] ? line + i + 1 : "";

        if (strcmp(verb, "PING") == 0) {
            printf("PONG %s\n", FW_VERSION);
        } else if (strcmp(verb, "ID") == 0) {
            cmd_id();
        } else if (strcmp(verb, "INFO") == 0) {
            cmd_info();
        } else if (strcmp(verb, "SPEED") == 0) {
            cmd_speed(arg);
        } else if (strcmp(verb, "SVF") == 0) {
            unsigned long n = strtoul(arg, NULL, 10);
            if (n == 0) {
                printf("ERR BAD_LENGTH\n");
            } else {
                cmd_svf((uint32_t)n);
            }
        } else if (strcmp(verb, "DIAG") == 0) {
            cmd_diag();
        } else if (strcmp(verb, "AVRID") == 0) {
            cmd_avr_id();
        } else if (strcmp(verb, "AVRFUSES") == 0) {
            cmd_avr_fuses();
        } else if (strcmp(verb, "AVRFUSEW") == 0) {
            cmd_avr_fusew(arg);
        } else if (strcmp(verb, "AVRVERIFY") == 0) {
            unsigned long n = strtoul(arg, NULL, 10);
            if (n == 0) printf("ERR BAD_LENGTH\n");
            else cmd_avr_verify((uint32_t)n);
        } else if (strcmp(verb, "AVRFLASH") == 0) {
            unsigned long n = strtoul(arg, NULL, 10);
            if (n == 0) printf("ERR BAD_LENGTH\n");
            else cmd_avr_flash((uint32_t)n);
        } else if (strcmp(verb, "BOOTSEL") == 0) {
            /* Drop to the mass-storage bootloader for reflashing without
             * unplugging and holding the button. */
            printf("OK entering bootloader\n");
            sleep_ms(100);
            reset_usb_boot(0, 0);
        } else if (verb[0] != '\0') {
            printf("ERR UNKNOWN_COMMAND %s\n", verb);
        }
    }
}
