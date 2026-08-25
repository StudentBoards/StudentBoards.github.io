/*
 * jtag.c — JTAG TAP driver for RP2040. See jtag.h for wiring.
 */

#include <string.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/structs/sio.h"
#include "jtag.h"

volatile uint32_t jtag_edge_delay_us = 0;

/*
 * One of these per JTAG port. The masks are precomputed so the hot loop
 * still does no shifting — moving from #define to a struct field costs a
 * load the compiler keeps in a register, not a shift per edge, so the
 * free-running clock rate is unchanged.
 *
 * `state` lives here rather than in a single global because the two ports
 * drive genuinely separate TAP state machines. A shared variable would
 * mean selecting the other port and then walking a TMS path computed from
 * where the *previous* board was left standing.
 */
typedef struct {
    uint8_t  tck, tms, tdi, tdo;
    uint32_t m_tck, m_tms, m_tdi, m_tdo;
    tap_state_t state;
} jtag_pins_t;

static jtag_pins_t ports[JTAG_PORT_COUNT] = {
    [JTAG_PORT_CPLD] = { .tck = PIN_TCK,     .tms = PIN_TMS,
                         .tdi = PIN_TDI,     .tdo = PIN_TDO },
    [JTAG_PORT_AVR]  = { .tck = PIN_AVR_TCK, .tms = PIN_AVR_TMS,
                         .tdi = PIN_AVR_TDI, .tdo = PIN_AVR_TDO },
};

static jtag_port_t active = JTAG_PORT_CPLD;
static jtag_pins_t *p = &ports[JTAG_PORT_CPLD];

#define M_TCK  (p->m_tck)
#define M_TMS  (p->m_tms)
#define M_TDI  (p->m_tdi)
#define M_TDO  (p->m_tdo)
#define current_state (p->state)

void jtag_select(jtag_port_t port)
{
    if (port >= JTAG_PORT_COUNT) return;
    active = port;
    p = &ports[port];
}

jtag_port_t jtag_active(void)
{
    return active;
}

const char *jtag_port_name(jtag_port_t port)
{
    switch (port) {
    case JTAG_PORT_CPLD: return "CPLD";
    case JTAG_PORT_AVR:  return "AVR";
    default:             return "?";
    }
}

/* Case-sensitive, like jtag_state_from_name() — the caller uppercases. */
bool jtag_port_from_name(const char *name, jtag_port_t *out)
{
    if (strcmp(name, "CPLD") == 0) { *out = JTAG_PORT_CPLD; return true; }
    if (strcmp(name, "AVR")  == 0) { *out = JTAG_PORT_AVR;  return true; }
    return false;
}

/*
 * TAP transition table: [state][tms] -> next state.
 *
 * This is the IEEE 1149.1 state machine verbatim. Everything else in this
 * file is derived from it, so an error here would be invisible and
 * catastrophic — it is worth reading against the standard's diagram.
 */
static const tap_state_t tap_next[TAP_STATE_COUNT][2] = {
    /* TAP_RESET     */ { TAP_IDLE,      TAP_RESET     },
    /* TAP_IDLE      */ { TAP_IDLE,      TAP_DRSELECT  },
    /* TAP_DRSELECT  */ { TAP_DRCAPTURE, TAP_IRSELECT  },
    /* TAP_DRCAPTURE */ { TAP_DRSHIFT,   TAP_DREXIT1   },
    /* TAP_DRSHIFT   */ { TAP_DRSHIFT,   TAP_DREXIT1   },
    /* TAP_DREXIT1   */ { TAP_DRPAUSE,   TAP_DRUPDATE  },
    /* TAP_DRPAUSE   */ { TAP_DRPAUSE,   TAP_DREXIT2   },
    /* TAP_DREXIT2   */ { TAP_DRSHIFT,   TAP_DRUPDATE  },
    /* TAP_DRUPDATE  */ { TAP_IDLE,      TAP_DRSELECT  },
    /* TAP_IRSELECT  */ { TAP_IRCAPTURE, TAP_RESET     },
    /* TAP_IRCAPTURE */ { TAP_IRSHIFT,   TAP_IREXIT1   },
    /* TAP_IRSHIFT   */ { TAP_IRSHIFT,   TAP_IREXIT1   },
    /* TAP_IREXIT1   */ { TAP_IRPAUSE,   TAP_IRUPDATE  },
    /* TAP_IRPAUSE   */ { TAP_IRPAUSE,   TAP_IREXIT2   },
    /* TAP_IREXIT2   */ { TAP_IRSHIFT,   TAP_IRUPDATE  },
    /* TAP_IRUPDATE  */ { TAP_IDLE,      TAP_DRSELECT  },
};

/* SVF state names. Several have aliases (TLR, RTI) that older tools emit. */
static const struct {
    const char *name;
    tap_state_t state;
} tap_names[] = {
    { "RESET",     TAP_RESET     }, { "TLR",       TAP_RESET     },
    { "IDLE",      TAP_IDLE      }, { "RTI",       TAP_IDLE      },
    { "DRSELECT",  TAP_DRSELECT  }, { "DRCAPTURE", TAP_DRCAPTURE },
    { "DRSHIFT",   TAP_DRSHIFT   }, { "DREXIT1",   TAP_DREXIT1   },
    { "DRPAUSE",   TAP_DRPAUSE   }, { "DREXIT2",   TAP_DREXIT2   },
    { "DRUPDATE",  TAP_DRUPDATE  }, { "IRSELECT",  TAP_IRSELECT  },
    { "IRCAPTURE", TAP_IRCAPTURE }, { "IRSHIFT",   TAP_IRSHIFT   },
    { "IREXIT1",   TAP_IREXIT1   }, { "IRPAUSE",   TAP_IRPAUSE   },
    { "IREXIT2",   TAP_IREXIT2   }, { "IRUPDATE",  TAP_IRUPDATE  },
};

bool jtag_state_from_name(const char *name, tap_state_t *out)
{
    for (size_t i = 0; i < count_of(tap_names); i++) {
        if (strcmp(name, tap_names[i].name) == 0) {
            *out = tap_names[i].state;
            return true;
        }
    }
    return false;
}

void jtag_init(void)
{
    /*
     * Both ports are brought up, and both are left in Test-Logic-Reset.
     * A board on the port that is not currently selected therefore sits
     * with its pins driven at defined idle levels rather than floating —
     * which matters here in a way it would not with one port, because the
     * whole point of having two is that the other board stays plugged in
     * while this one is programmed.
     */
    for (int i = 0; i < JTAG_PORT_COUNT; i++) {
        jtag_pins_t *q = &ports[i];

        q->m_tck = 1u << q->tck;
        q->m_tms = 1u << q->tms;
        q->m_tdi = 1u << q->tdi;
        q->m_tdo = 1u << q->tdo;

        gpio_init(q->tck);
        gpio_init(q->tms);
        gpio_init(q->tdi);
        gpio_init(q->tdo);

        gpio_set_dir(q->tck, GPIO_OUT);
        gpio_set_dir(q->tms, GPIO_OUT);
        gpio_set_dir(q->tdi, GPIO_OUT);
        gpio_set_dir(q->tdo, GPIO_IN);

        /*
         * Pull-up on TDO so an unconnected target reads as a steady
         * all-ones rather than floating noise. That turns "nothing
         * plugged in" into a clean, repeatable reading instead of a
         * random one.
         */
        gpio_pull_up(q->tdo);

        /* Maximum drive and slew on the clock: a slow edge on TCK is what
         * turns marginal wiring into intermittent verify failures. */
        gpio_set_drive_strength(q->tck, GPIO_DRIVE_STRENGTH_12MA);
        gpio_set_slew_rate(q->tck, GPIO_SLEW_RATE_FAST);

        sio_hw->gpio_clr = q->m_tck | q->m_tdi;
        sio_hw->gpio_set = q->m_tms;

        jtag_select((jtag_port_t)i);
        jtag_reset();
    }

    jtag_select(JTAG_PORT_CPLD);
}

bool jtag_clock(bool tms, bool tdi, bool read_tdo)
{
    uint32_t set = 0, clr = M_TCK;
    bool tdo = false;

    if (tms) set |= M_TMS; else clr |= M_TMS;
    if (tdi) set |= M_TDI; else clr |= M_TDI;

    /* Set up data while TCK is low. */
    sio_hw->gpio_clr = clr;
    if (set) sio_hw->gpio_set = set;

    if (jtag_edge_delay_us) busy_wait_us(jtag_edge_delay_us);

    /* Sample TDO before the rising edge — the target drove it on the
     * previous falling edge. */
    if (read_tdo) {
        tdo = (sio_hw->gpio_in & M_TDO) != 0;
    }

    /* Rising edge: the target samples TMS/TDI here. */
    sio_hw->gpio_set = M_TCK;
    if (jtag_edge_delay_us) busy_wait_us(jtag_edge_delay_us);

    /* Falling edge. */
    sio_hw->gpio_clr = M_TCK;

    current_state = tap_next[current_state][tms ? 1 : 0];
    return tdo;
}

void jtag_reset(void)
{
    for (int i = 0; i < 5; i++) {
        jtag_clock(true, false, false);
    }
    current_state = TAP_RESET;
}

tap_state_t jtag_state(void)
{
    return current_state;
}

/*
 * Shortest TMS path between any two TAP states, by breadth-first search.
 *
 * Computing this rather than hardcoding paths means adding a state or
 * changing the table above cannot leave a stale path behind. The search
 * runs over 16 states so it costs nothing.
 */
static int tap_path(tap_state_t from, tap_state_t to, uint8_t *tms_out)
{
    if (from == to) return 0;

    tap_state_t queue[TAP_STATE_COUNT];
    int8_t prev[TAP_STATE_COUNT];
    int8_t prev_tms[TAP_STATE_COUNT];
    bool seen[TAP_STATE_COUNT] = { false };
    int head = 0, tail = 0;

    for (int i = 0; i < TAP_STATE_COUNT; i++) {
        prev[i] = -1;
        prev_tms[i] = 0;
    }

    queue[tail++] = from;
    seen[from] = true;

    while (head < tail) {
        tap_state_t s = queue[head++];
        for (int tms = 0; tms < 2; tms++) {
            tap_state_t n = tap_next[s][tms];
            if (seen[n]) continue;
            seen[n] = true;
            prev[n] = (int8_t)s;
            prev_tms[n] = (int8_t)tms;
            if (n == to) {
                /* Walk back, then reverse into tms_out. */
                int len = 0;
                tap_state_t cur = to;
                uint8_t tmp[TAP_STATE_COUNT];
                while (cur != from) {
                    tmp[len++] = (uint8_t)prev_tms[cur];
                    cur = (tap_state_t)prev[cur];
                }
                for (int i = 0; i < len; i++) {
                    tms_out[i] = tmp[len - 1 - i];
                }
                return len;
            }
            queue[tail++] = n;
        }
    }
    return -1;
}

void jtag_goto(tap_state_t target)
{
    uint8_t tms[TAP_STATE_COUNT];
    int len = tap_path(current_state, target, tms);
    for (int i = 0; i < len; i++) {
        jtag_clock(tms[i] != 0, false, false);
    }
}

void jtag_run_test(uint32_t clocks, tap_state_t state)
{
    jtag_goto(state);
    for (uint32_t i = 0; i < clocks; i++) {
        jtag_clock(false, false, false);
    }
}

static inline bool bit_get(const uint8_t *buf, uint32_t bit)
{
    return buf && ((buf[bit >> 3] >> (bit & 7)) & 1);
}

bool jtag_shift(uint32_t nbits,
                const uint8_t *tdi,
                const uint8_t *tdo_expect,
                const uint8_t *mask,
                tap_state_t shift_state,
                tap_state_t end,
                uint32_t *fail_bit,
                uint8_t *tdo_got)
{
    if (nbits == 0) {
        jtag_goto(end);
        return true;
    }

    jtag_goto(shift_state);

    bool checking = (tdo_expect != NULL);
    bool ok = true;

    if (tdo_got) {
        memset(tdo_got, 0, JTAG_TDO_CAPTURE_BYTES);
    }

    for (uint32_t i = 0; i < nbits; i++) {
        bool bit = bit_get(tdi, i);
        /* Assert TMS on the final bit so we leave the shift state on the
         * same clock that shifts that bit — this is what makes the bit
         * count come out right. */
        bool last = (i == nbits - 1);
        bool got = jtag_clock(last, bit, checking);

        /* Keep the leading bits of whatever came back, so a failure can
         * report the actual value rather than only where it differed.
         * Capturing on every shift costs nothing and means the diagnostic
         * is available without re-running. */
        if (tdo_got && got && i < JTAG_TDO_CAPTURE_BYTES * 8) {
            tdo_got[i >> 3] |= (uint8_t)(1u << (i & 7));
        }

        if (checking && ok) {
            /* A NULL mask means check every bit. */
            bool care = mask ? bit_get(mask, i) : true;
            if (care && got != bit_get(tdo_expect, i)) {
                ok = false;
                if (fail_bit) *fail_bit = i;
            }
        }
    }

    /* jtag_clock() already advanced us through Exit1 on the last bit. */
    jtag_goto(end);
    return ok;
}

uint32_t jtag_read_idcode(void)
{
    uint8_t tdi[4] = { 0, 0, 0, 0 };
    uint8_t out = 0;
    uint32_t idcode = 0;

    jtag_reset();
    jtag_goto(TAP_DRSHIFT);

    for (int i = 0; i < 32; i++) {
        bool last = (i == 31);
        if (jtag_clock(last, false, true)) {
            idcode |= (1u << i);
        }
    }
    (void)tdi;
    (void)out;

    jtag_goto(TAP_IDLE);
    jtag_reset();
    return idcode;
}
