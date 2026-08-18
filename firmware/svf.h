/*
 * svf.h — streaming SVF parser.
 *
 * SVF files for a MAX V are hundreds of KB of ASCII, far more than the
 * RP2040's 264 KB of SRAM once you allow for USB buffers. So the parser
 * is fed bytes as they arrive over USB and executes each statement as it
 * completes, never holding more than one statement at a time.
 *
 * That also means programming starts as soon as the first bytes land,
 * rather than after a full upload — the transfer and the programming
 * overlap instead of running back to back.
 */

#ifndef SVF_H
#define SVF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Largest shift, in bits, that a single SIR/SDR statement may carry.
 *
 * Quartus SVFs for MAX V use modest shifts (the CFM is written in pages,
 * not one giant vector), so 4096 bits is comfortable. If you hit
 * SVF_ERR_TOO_LONG on a bigger device, raise this — three buffers of
 * SVF_MAX_BITS/8 bytes each are allocated statically.
 */
#define SVF_MAX_BITS   4096
#define SVF_MAX_BYTES  (SVF_MAX_BITS / 8)

typedef enum {
    SVF_OK = 0,
    SVF_ERR_SYNTAX,
    SVF_ERR_UNSUPPORTED,
    SVF_ERR_TOO_LONG,
    SVF_ERR_TDO_MISMATCH,
    SVF_ERR_CHAIN,        /* HIR/HDR/TIR/TDR non-zero: multi-device chain */
} svf_result_t;

typedef struct {
    /* Statement accumulator. */
    char     stmt[512];
    uint32_t stmt_len;
    bool     in_comment;       /* inside a ! or // comment, to end of line */

    /* Sticky parser state set by ENDIR/ENDDR. */
    uint8_t  end_ir;           /* tap_state_t */
    uint8_t  end_dr;

    /* Statistics, reported back to the host at the end. */
    uint32_t statements;
    uint32_t total_bits;

    /* Failure detail, valid when the result is not SVF_OK. */
    uint32_t error_statement;
    uint32_t error_bit;
    char     error_detail[96];
} svf_ctx_t;

/* Reset the parser. Call before each file. */
void svf_init(svf_ctx_t *ctx);

/*
 * Feed `len` bytes. Executes any statements that complete within this
 * chunk. Returns SVF_OK, or the first error encountered — after an error
 * the caller should stop feeding, since continuing would run the rest of
 * a programming sequence against a device in an unknown state.
 */
svf_result_t svf_feed(svf_ctx_t *ctx, const uint8_t *data, size_t len);

/*
 * Call after the last chunk. Executes any trailing statement that was not
 * terminated by a semicolon and returns the final result.
 */
svf_result_t svf_finish(svf_ctx_t *ctx);

/* Human-readable name for a result code. */
const char *svf_result_str(svf_result_t r);

/* Called by the parser roughly once per statement so the caller can blink
 * an LED or report progress. Defined in main.c. */
extern void svf_progress_hook(uint32_t statements, uint32_t bits);

#endif /* SVF_H */
