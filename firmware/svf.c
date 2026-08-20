/*
 * svf.c — streaming SVF parser and executor. See svf.h.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "jtag.h"
#include "svf.h"

/*
 * Shift buffers. Static rather than on the stack: three of these at
 * SVF_MAX_BYTES each would blow the default RP2040 stack, and making the
 * ceiling a link-time fact is better than a runtime surprise.
 */
static uint8_t buf_tdi[SVF_MAX_BYTES];
static uint8_t buf_tdo[SVF_MAX_BYTES];
static uint8_t buf_mask[SVF_MAX_BYTES];

const char *svf_result_str(svf_result_t r)
{
    switch (r) {
    case SVF_OK:              return "OK";
    case SVF_ERR_SYNTAX:      return "SYNTAX";
    case SVF_ERR_UNSUPPORTED: return "UNSUPPORTED";
    case SVF_ERR_TOO_LONG:    return "TOO_LONG";
    case SVF_ERR_TDO_MISMATCH:return "TDO_MISMATCH";
    case SVF_ERR_CHAIN:       return "CHAIN";
    default:                  return "UNKNOWN";
    }
}

void svf_init(svf_ctx_t *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->end_ir = TAP_IDLE;
    ctx->end_dr = TAP_IDLE;
}

/* ------------------------------------------------------------------ */
/* Hex parsing                                                        */
/* ------------------------------------------------------------------ */

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/*
 * Parse a parenthesised hex value into a little-endian bit buffer.
 *
 * SVF writes these MSB-first, and whitespace inside the parentheses is
 * legal and common in Quartus output (long vectors get wrapped across
 * lines), so it is skipped rather than treated as a terminator.
 *
 * Returns the number of hex digits consumed, or -1 on error.
 */
static int parse_hex_into(const char *p, const char *end,
                          uint8_t *out, uint32_t nbits)
{
    uint32_t nbytes = (nbits + 7) / 8;
    memset(out, 0, nbytes);

    /* First pass: count digits, so we know where the LSB lands. */
    int digits = 0;
    for (const char *q = p; q < end; q++) {
        if (isspace((unsigned char)*q)) continue;
        if (hexval(*q) < 0) return -1;
        digits++;
    }

    /* Second pass: fill from the least significant digit backwards. */
    int nibble = 0;
    for (const char *q = end - 1; q >= p; q--) {
        if (isspace((unsigned char)*q)) continue;
        int v = hexval(*q);
        if (v < 0) return -1;
        uint32_t bitpos = (uint32_t)nibble * 4;
        if (bitpos < nbits) {
            out[bitpos >> 3] |= (uint8_t)(v << (bitpos & 7));
        }
        nibble++;
    }
    return digits;
}

/*
 * Find `key (...)` in a statement body and parse its contents.
 *
 * Returns 1 if found and parsed, 0 if absent, -1 on a parse error. The
 * three outcomes are distinct because "absent" is normal (TDO and MASK
 * are optional) while a malformed value is a hard error.
 */
static int find_param(const char *body, const char *key,
                      uint8_t *out, uint32_t nbits)
{
    size_t keylen = strlen(key);
    const char *p = body;

    while ((p = strstr(p, key)) != NULL) {
        /* Must be a whole word: TDI must not match inside SMASK etc. */
        bool left_ok = (p == body) || !isalnum((unsigned char)p[-1]);
        const char *q = p + keylen;
        while (*q == ' ' || *q == '\t') q++;
        if (left_ok && *q == '(') {
            const char *open = q + 1;
            const char *close = strchr(open, ')');
            if (!close) return -1;
            if (parse_hex_into(open, close, out, nbits) < 0) return -1;
            return 1;
        }
        p += keylen;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Statement execution                                                */
/* ------------------------------------------------------------------ */

static svf_result_t do_shift(svf_ctx_t *ctx, const char *body,
                             tap_state_t shift_state, tap_state_t end)
{
    char *endp = NULL;
    unsigned long nbits = strtoul(body, &endp, 10);

    if (endp == body) return SVF_ERR_SYNTAX;
    if (nbits > SVF_MAX_BITS) {
        snprintf(ctx->error_detail, sizeof(ctx->error_detail),
                 "shift of %lu bits exceeds SVF_MAX_BITS (%d)",
                 nbits, SVF_MAX_BITS);
        return SVF_ERR_TOO_LONG;
    }

    int has_tdi  = find_param(endp, "TDI",  buf_tdi,  (uint32_t)nbits);
    int has_tdo  = find_param(endp, "TDO",  buf_tdo,  (uint32_t)nbits);
    int has_mask = find_param(endp, "MASK", buf_mask, (uint32_t)nbits);

    if (has_tdi < 0 || has_tdo < 0 || has_mask < 0) return SVF_ERR_SYNTAX;
    if (!has_tdi) memset(buf_tdi, 0, (nbits + 7) / 8);

    uint32_t fail_bit = 0;
    uint8_t got[JTAG_TDO_CAPTURE_BYTES];

    bool ok = jtag_shift((uint32_t)nbits,
                         buf_tdi,
                         has_tdo ? buf_tdo : NULL,
                         has_mask ? buf_mask : NULL,
                         shift_state, end, &fail_bit, got);

    ctx->total_bits += (uint32_t)nbits;

    if (!ok) {
        ctx->error_bit = fail_bit;

        /*
         * Report the value, not just where it differed. A bit index cannot
         * distinguish "the device stopped answering" (all ones or all
         * zeros) from "the device answered, with data for a different
         * part" — and those need completely different fixes.
         *
         * Only the low 32 bits are shown; that covers the short status
         * reads where this matters, and a longer vector's leading bits are
         * enough to tell the two cases apart.
         */
        unsigned long shown = (nbits < 32) ? (unsigned long)nbits : 32;
        uint32_t g = 0, e = 0;
        for (unsigned long i = 0; i < shown; i++) {
            if (got[i >> 3] & (1u << (i & 7)))     g |= (1u << i);
            if (buf_tdo[i >> 3] & (1u << (i & 7))) e |= (1u << i);
        }

        snprintf(ctx->error_detail, sizeof(ctx->error_detail),
                 "TDO bit %lu of %lu: got 0x%0*lX expected 0x%0*lX",
                 (unsigned long)fail_bit, nbits,
                 (int)((shown + 3) / 4), (unsigned long)g,
                 (int)((shown + 3) / 4), (unsigned long)e);
        return SVF_ERR_TDO_MISMATCH;
    }
    return SVF_OK;
}

/*
 * RUNTEST [state] N TCK [M SEC] [ENDSTATE s]
 *
 * The TCK count and any SEC minimum are both honoured. On MAX V these are
 * flash erase and program waits, so running short of them corrupts the
 * cycle — overshooting is harmless, undershooting is not.
 */
static svf_result_t do_runtest(svf_ctx_t *ctx, const char *body)
{
    char tokens[16][24];
    int ntok = 0;
    const char *p = body;

    while (*p && ntok < 16) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        int n = 0;
        while (*p && *p != ' ' && *p != '\t' && n < 23) {
            tokens[ntok][n++] = (char)toupper((unsigned char)*p++);
        }
        tokens[ntok][n] = '\0';
        ntok++;
        while (*p && *p != ' ' && *p != '\t') p++;
    }

    tap_state_t state = TAP_IDLE;
    tap_state_t end_state = TAP_IDLE;
    bool have_end = false;
    uint32_t clocks = 0;
    double min_sec = 0.0;
    int i = 0;

    if (ntok > 0) {
        tap_state_t s;
        if (jtag_state_from_name(tokens[0], &s)) {
            state = s;
            end_state = s;      /* default: finish where we ran */
            i = 1;
        }
    }

    for (; i < ntok; i++) {
        if (strcmp(tokens[i], "TCK") == 0 && i > 0) {
            clocks = (uint32_t)strtoul(tokens[i - 1], NULL, 10);
        } else if (strcmp(tokens[i], "SEC") == 0 && i > 0) {
            double v = strtod(tokens[i - 1], NULL);
            if (v > min_sec) min_sec = v;
        } else if (strcmp(tokens[i], "ENDSTATE") == 0 && i + 1 < ntok) {
            /* ENDSTATE is independent of the run state in SVF. Quartus
             * usually emits both as IDLE, where ignoring this clause
             * happens to work — but a file that set them differently
             * would leave the TAP where the next statement does not
             * expect it, so honour it properly. */
            tap_state_t s;
            if (!jtag_state_from_name(tokens[i + 1], &s)) {
                snprintf(ctx->error_detail, sizeof(ctx->error_detail),
                         "unknown ENDSTATE '%s'", tokens[i + 1]);
                return SVF_ERR_SYNTAX;
            }
            end_state = s;
            have_end = true;
            i++;                /* consume the state name */
        }
    }

    absolute_time_t start = get_absolute_time();
    jtag_run_test(clocks, state);

    if (min_sec > 0.0) {
        int64_t want_us = (int64_t)(min_sec * 1e6);
        int64_t spent = absolute_time_diff_us(start, get_absolute_time());
        if (spent < want_us) {
            busy_wait_us((uint64_t)(want_us - spent));
        }
    }

    if (have_end && end_state != state) {
        jtag_goto(end_state);
    }
    return SVF_OK;
}

static svf_result_t do_state(svf_ctx_t *ctx, const char *body)
{
    char name[24];
    const char *p = body;

    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        int n = 0;
        while (*p && *p != ' ' && *p != '\t' && n < 23) {
            name[n++] = (char)toupper((unsigned char)*p++);
        }
        name[n] = '\0';

        tap_state_t s;
        if (!jtag_state_from_name(name, &s)) {
            snprintf(ctx->error_detail, sizeof(ctx->error_detail),
                     "unknown STATE '%s'", name);
            return SVF_ERR_SYNTAX;
        }
        jtag_goto(s);
    }
    return SVF_OK;
}

static svf_result_t execute(svf_ctx_t *ctx, char *stmt)
{
    /* Split the leading keyword from the rest. */
    char *p = stmt;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) return SVF_OK;

    char cmd[16];
    int n = 0;
    while (*p && *p != ' ' && *p != '\t' && n < 15) {
        cmd[n++] = (char)toupper((unsigned char)*p++);
    }
    cmd[n] = '\0';
    while (*p == ' ' || *p == '\t') p++;
    char *body = p;

    ctx->statements++;

    if (strcmp(cmd, "SDR") == 0) {
        return do_shift(ctx, body, TAP_DRSHIFT, (tap_state_t)ctx->end_dr);
    }
    if (strcmp(cmd, "SIR") == 0) {
        return do_shift(ctx, body, TAP_IRSHIFT, (tap_state_t)ctx->end_ir);
    }
    if (strcmp(cmd, "RUNTEST") == 0) {
        return do_runtest(ctx, body);
    }
    if (strcmp(cmd, "STATE") == 0) {
        return do_state(ctx, body);
    }
    if (strcmp(cmd, "ENDIR") == 0 || strcmp(cmd, "ENDDR") == 0) {
        char name[24];
        int i = 0;
        while (body[i] && body[i] != ' ' && body[i] != '\t' && i < 23) {
            name[i] = (char)toupper((unsigned char)body[i]);
            i++;
        }
        name[i] = '\0';
        tap_state_t s;
        if (!jtag_state_from_name(name, &s)) return SVF_ERR_SYNTAX;

        /*
         * Index 3, not 2: "ENDIR" and "ENDDR" share E-N-D at 0..2 and only
         * differ at index 3. Getting this wrong sends ENDIR's state into
         * end_dr, so every SDR ends in Pause-IR — and the next SDR's route
         * back to Shift-DR then passes through Update-IR, latching the
         * Capture-IR pattern over the real instruction. The first shift
         * after any SIR still works, which makes it look like a hardware
         * fault rather than a parser bug.
         */
        if (strcmp(cmd, "ENDIR") == 0) ctx->end_ir = (uint8_t)s;
        else                           ctx->end_dr = (uint8_t)s;
        return SVF_OK;
    }
    if (strcmp(cmd, "TRST") == 0) {
        /* No nTRST pin is wired; five TMS-high clocks reach reset from
         * anywhere, which is equivalent for our purposes. */
        return SVF_OK;
    }
    if (strcmp(cmd, "FREQUENCY") == 0) {
        /*
         * RUNTEST waits on MAX V are flash erase/program delays expressed
         * as a TCK count, computed by Quartus from this declared frequency
         * — in a real file they are over 99% of all clocks. Running slower
         * than declared just stretches those waits, which is harmless;
         * running faster shortens them below what the silicon needs and
         * truncates a flash write, which fails intermittently and looks
         * exactly like flaky hardware. So this only ever slows the clock
         * down from its free-running rate, never speeds it up — a file
         * declaring a frequency above what we already run at changes
         * nothing, and rounding always favours "too slow" over "too fast".
         */
        double hz = strtod(body, NULL);   /* "1.80E+07 HZ": stops at the space */
        if (hz > 0.0) {
            const double FREE_RUNNING_HZ = 2.8e6;
            if (hz >= FREE_RUNNING_HZ) {
                jtag_edge_delay_us = 0;
            } else {
                double target_half_us = 500000.0 / hz;
                double baseline_half_us = 500000.0 / FREE_RUNNING_HZ;
                double extra = target_half_us - baseline_half_us;

                /*
                 * Clamp before the cast, not after. A frequency low enough
                 * to make `extra` exceed uint32_t makes the conversion
                 * undefined, and it was observed wrapping to 0 — full
                 * speed, the exact opposite of what a slow declared
                 * frequency must produce. Saturating in double keeps the
                 * error on the safe side for any value a file can contain.
                 */
                if (extra > 1000.0) extra = 1000.0;
                if (!(extra > 0.0))  extra = 0.0;   /* also catches NaN */

                jtag_edge_delay_us = (uint32_t)(extra + 0.999);
            }
        }
        return SVF_OK;
    }
    if (strcmp(cmd, "HIR") == 0 || strcmp(cmd, "HDR") == 0 ||
        strcmp(cmd, "TIR") == 0 || strcmp(cmd, "TDR") == 0) {
        /*
         * Header/trailer padding for multi-device chains. Quartus emits
         * these as zero-length for a single device. A non-zero length
         * means this SVF targets a chain we would silently misprogram, so
         * it is an error rather than something to ignore.
         */
        unsigned long len = strtoul(body, NULL, 10);
        if (len != 0) {
            snprintf(ctx->error_detail, sizeof(ctx->error_detail),
                     "%s %lu: SVF targets a multi-device chain", cmd, len);
            return SVF_ERR_CHAIN;
        }
        return SVF_OK;
    }

    snprintf(ctx->error_detail, sizeof(ctx->error_detail),
             "unsupported command '%s'", cmd);
    return SVF_ERR_UNSUPPORTED;
}

/* ------------------------------------------------------------------ */
/* Streaming feed                                                     */
/* ------------------------------------------------------------------ */

svf_result_t svf_feed(svf_ctx_t *ctx, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        char c = (char)data[i];

        /* Comments run to end of line: '!' or '//'. */
        if (ctx->in_comment) {
            if (c == '\n' || c == '\r') ctx->in_comment = false;
            continue;
        }
        if (c == '!') {
            ctx->in_comment = true;
            continue;
        }
        if (c == '/' && ctx->stmt_len > 0 &&
            ctx->stmt[ctx->stmt_len - 1] == '/') {
            ctx->stmt_len--;              /* drop the first slash */
            ctx->in_comment = true;
            continue;
        }

        if (c == ';') {
            ctx->stmt[ctx->stmt_len] = '\0';
            svf_result_t r = execute(ctx, ctx->stmt);
            ctx->stmt_len = 0;
            if (r != SVF_OK) {
                ctx->error_statement = ctx->statements;
                return r;
            }
            svf_progress_hook(ctx->statements, ctx->total_bits);
            continue;
        }

        /* Collapse newlines and tabs to spaces: statements legally span
         * multiple lines, so line structure carries no meaning. */
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';

        /* Squeeze runs of spaces to keep long vectors inside stmt[]. */
        if (c == ' ' && (ctx->stmt_len == 0 ||
                         ctx->stmt[ctx->stmt_len - 1] == ' ')) {
            continue;
        }

        if (ctx->stmt_len >= sizeof(ctx->stmt) - 1) {
            snprintf(ctx->error_detail, sizeof(ctx->error_detail),
                     "statement longer than %u bytes",
                     (unsigned)sizeof(ctx->stmt));
            ctx->error_statement = ctx->statements;
            return SVF_ERR_TOO_LONG;
        }
        ctx->stmt[ctx->stmt_len++] = c;
    }
    return SVF_OK;
}

svf_result_t svf_finish(svf_ctx_t *ctx)
{
    if (ctx->stmt_len > 0) {
        ctx->stmt[ctx->stmt_len] = '\0';
        svf_result_t r = execute(ctx, ctx->stmt);
        ctx->stmt_len = 0;
        if (r != SVF_OK) {
            ctx->error_statement = ctx->statements;
            return r;
        }
    }
    return SVF_OK;
}

