/*
 * Clip take recording. Original 1010music Blackbox, firmware 3.1.9.
 *
 * Before 3.0 you could record clip launches: REC+PLAY on a PADS sequence, tap Toggle clips on the PADS screen to
 * start and stop them, and the sequence replayed them. 3.x records each tap as its own note, press to release, and
 * plays every note as a gate. A Toggle clip tapped on and off comes back as two blips.
 *
 * This records a Toggle clip the way it played: one note from where the clip started to where it stopped, both on
 * the pad's own Quant Size grid (where the clip really starts and stops). Stock gate playback then starts the clip at
 * the note and stops it at the note's end. Other pads, KEYS and MIDI sequences, playback and editing are all stock.
 *
 * Recording again over a sequence only adds (overdub, like MIDI overdub in Ableton or live recording on an MPC or
 * Elektron): a take that overlaps or touches an older note of the same pad joins it into one note. Recording never
 * shortens or removes a note; that is done in the sequence editor. A tap on a clip the sequence is playing stops it
 * live and leaves the pattern alone; the next tap starts a take as usual.
 *
 * Stock recording: while a sequence records, it sends the app timed events from its input (0x30 bytes each; type at
 * +0, pad id 0x01nn at +2 with nn = pad index, pitch at +0x10, engine time at +0x28, 960 ticks per beat). The app's
 * pump hands every one to the recorder (FUN_080a02c0, bl @0x080a2946); this wraps that call. For a Toggle pad in a
 * PADS sequence, a press:
 *   - closes the take if one is open for the pad: the note is written from the take's start to here;
 *   - stops the clip if the sequence is playing it here (a note of the pad covers this point and it was not already
 *     stopped live): nothing is written;
 *   - otherwise opens a take: the clip starts here. Until it closes, the sequence's own note on / off for the pad are
 *     ignored (cliprec_thunk.S), so older notes under the take cannot cut or restart it.
 * Releases on Toggle pads are dropped. When recording stops (seen by wrapping the pump's queue read, bl @0x080a2936),
 * takes still open are written up to that point.
 *
 * Where a time falls in the pattern is worked out the way the stock recorder does it: the pass started at
 * now - current step * step length, snapped to the sequence's launch grid (Quant Size of the sequence, param 0x46),
 * and the position is the time since then, wrapped around the loop and converted to pattern ticks (960 per step; engine
 * time is 960 per beat). A note is made by the stock recorder (a press on
 * a pad that does not exist, so it never meets a real note), then given its pad, place and length and sent to the
 * engine.
 */
#include <stdint.h>

#define FN(addr) ((addr) | 1u)

struct note {                    /* pattern event, 0x18 bytes */
    uint8_t type, b1;            /* type 1 = note */
    uint16_t pad;                /* 0x01nn, nn = pad index (PADS sequences) */
    uint32_t start, len, id;     /* position and length in pattern ticks: 960 per step */
    uint16_t pitch, vel;
    uint32_t x;
};

typedef void (*record_fn)(uint8_t *app, uint8_t *ev);
typedef int (*pop_fn)(void *queue, uint8_t *ev);
typedef uint8_t *(*pad_obj_fn)(uint8_t *app, const void *ctx);
typedef uint8_t *(*cell_obj_fn)(uint8_t *app, const void *ctx, unsigned layer);
typedef int (*param_fn)(const uint8_t *obj, unsigned param);
typedef int (*list_get_fn)(void *list, unsigned k, struct note *out);
typedef void (*list_set_fn)(void *list, const struct note *n);
typedef void (*list_del_fn)(void *list, uint32_t id);
typedef void (*eng_set_fn)(uint32_t handle, uint32_t seq, const struct note *n, uint32_t zero, uint32_t layer);
typedef void (*eng_del_fn)(uint32_t handle, uint32_t seq, uint32_t id, uint32_t layer);
typedef int64_t *(*now_fn)(int64_t *out, uint32_t handle);
typedef int32_t *(*seq_pos_fn)(int32_t out[3], uint8_t *app, const void *ctx);

#define fw_record   ((record_fn)FN(0x080a02c0))      /* the app's live recorder */
#define fw_pop      ((pop_fn)FN(0x0804c754))         /* next timed event from the engine, 0 when empty */
#define fw_pad_obj  ((pad_obj_fn)FN(0x08098d0c))     /* pad object for ctx {u32, u16 (row << 4) | col} */
#define fw_cell_obj ((cell_obj_fn)FN(0x08097ce8))    /* sequence cell object for ctx, layer */
#define fw_param    ((param_fn)FN(0x08093e9c))       /* read a parameter of a pad or cell object */
#define fw_list_get ((list_get_fn)FN(0x08063d08))    /* k-th event of a pattern, 0 past the end */
#define fw_list_set ((list_set_fn)FN(0x08063b38))    /* update an event (by id), kept sorted */
#define fw_list_del ((list_del_fn)FN(0x08063c08))    /* remove an event by id */
#define fw_eng_set  ((eng_set_fn)FN(0x0804c798))     /* tell the engine: add / update this note */
#define fw_eng_del  ((eng_del_fn)FN(0x0804c7e8))     /* tell the engine: remove note id */
#define fw_now      ((now_fn)FN(0x0804ca68))         /* engine time now */
#define fw_seq_pos  ((seq_pos_fn)FN(0x08097860))     /* sequence's current step in this pass (first word) */
#define HANDLE      0x2400a9c0u                      /* the app's engine handle, as the recorder passes it */

#define APP_CELL_CTX  0x18       /* ctx of the sequence being recorded; its u16 id at +0x1c */
#define APP_RECORDING 0x8cc1     /* u8: 1 while a sequence records (the recorder's own check) */
#define APP_NEXT_ID   0xe808     /* u32: id the recorder gives its next note */

#define PARAM_LAUNCH    0x93     /* pad Launch Mode: 0 Trigger, 1 Gate, 2 Toggle */
#define LAUNCH_TOGGLE   2
#define PARAM_MODE      0x8b     /* pad mode: 1 Clip */
#define MODE_CLIP       1
#define PARAM_QUANT     0x47     /* Clip pad Quant Size: 8 bars, 4 bars, 2 bars, 1 bar, 1/2, 1/4, 1/8, 1/16, None */
#define QUANTS          8
#define QUANT_BEATS     ((const float *)0x080ecae4u)  /* beats per entry (32, 16, 8, 4, 2, 1, .5, .25): the clip
                                                          engine's launch / stop grid (FUN_08067648, param 0x47) */
#define TICKS_PER_BEAT  960
#define PARAM_CELL_TYPE 0xb7     /* sequence cell: 0 PADS (the recorder keeps only pitch-0 notes there) */
#define CELL_PADS       0
#define PARAM_LAYER     0xc0     /* sequence cell: active layer A..D */
#define PARAM_STEP      0x85     /* layer: step length, index into the step table */
#define PARAM_STEPS     0x86     /* layer: number of steps */
#define STEP_BEATS      ((const float *)0x080eca6cu)  /* beats per step-length entry (15) */
#define STEP_LENGTHS    15
#define PARAM_SEQ_QUANT 0x46     /* sequence cell: its own launch grid, index into the table below */
#define SEQ_QUANT_BEATS ((const float *)0x080ecc70u)  /* beats per entry (13; the recorder uses at least one step) */
#define SEQ_QUANTS      13

#define EV_SIZE  0x30
#define EV_TYPE  0x00
#define EV_PAD   0x02
#define EV_PITCH 0x10
#define EV_TIME  0x28
#define EV_PRESS   5
#define EV_RELEASE 6
#define PROBE_PAD  0x01ff        /* no such pad: a probe note can never collide with or play a real one */

#define PADS 16

void bkp_enable(void);

/* Backup SRAM (0x38800000..0x38801000; the stock firmware leaves it alone), from 0x38800c00: 0x3a0 bytes. */
struct cliprec_state {
    uint32_t magic;
    uint8_t *app;
    uint16_t held;               /* bit per pad index: a take is open */
    uint16_t _r;
    uint8_t press[PADS][EV_SIZE];/* the press that opened it */
    int32_t quiet[PADS];         /* engine time until which a pad the sequence plays was stopped live */
};
#define S ((volatile struct cliprec_state *)0x38800c00u)
#define MAGIC 0x43525334u        /* "CRS4" */

/* Patch RAM: the alignment gap between the stock heap end (0x2405ff54) and the MPU regions at 0x24060000, unused by
   the stock firmware. Ordinary RAM, so the engine can read it at any time. Pads whose sequencer notes are ignored while a take is open: */
struct cliprec_mute {
    uint32_t magic;
    uint16_t pads;               /* bit = pad index */
    uint16_t _r;
};
#define MUTE ((volatile struct cliprec_mute *)0x2405ff54u)
#define MUTE_MAGIC 0x434d5554u   /* "CMUT"; cliprec_thunk.S checks it */

static void ensure(void)
{
    bkp_enable();
    if (S->magic != MAGIC) {
        S->app = 0;
        S->held = 0;
        for (int i = 0; i < PADS; i++)
            S->quiet[i] = 0;
        S->magic = MAGIC;
    }
    if (MUTE->magic != MUTE_MAGIC || !S->held) {
        MUTE->pads = 0;
        MUTE->magic = MUTE_MAGIC;
    }
}

static unsigned pad_index(const uint8_t *ev)
{
    return *(const uint16_t *)(ev + EV_PAD) & 0xff;
}

static int32_t ev_time(const uint8_t *ev)     /* engine time; 2^31 ticks is about 13 days at 120 BPM */
{
    return *(const int32_t *)(ev + EV_TIME);
}

static uint32_t ticks(float beats)
{
    return beats > 0 ? (uint32_t)(beats * TICKS_PER_BEAT + 0.5f) : 0;
}

static const uint8_t *pad_obj(uint8_t *app, unsigned index)
{
    struct { uint32_t tag; uint16_t id, _r; } ctx = {0, (uint16_t)((index / 4) << 4 | index % 4), 0};
    return fw_pad_obj(app, &ctx);
}

/* Launch Mode of the pad an event names (index 0..15 in the low byte of the id), or -1. */
static int launch_of(uint8_t *app, const uint8_t *ev)
{
    unsigned index = pad_index(ev);
    return index < PADS ? fw_param(pad_obj(app, index), PARAM_LAUNCH) : -1;
}

/* Is this a press or release on a Toggle pad, recorded into a PADS sequence? */
static int is_toggle(uint8_t *app, const uint8_t *ev)
{
    if (*(const uint16_t *)(ev + EV_PITCH) != 0)
        return 0;
    if (fw_param(fw_cell_obj(app, app + APP_CELL_CTX, 0), PARAM_CELL_TYPE) != CELL_PADS)
        return 0;
    return launch_of(app, ev) == LAUNCH_TOGGLE;
}

/* A clip starts and stops on its Quant Size grid, not at the tap: the next grid line at or after t. Only Clip pads
   launch on a grid. */
static int32_t on_grid(uint8_t *app, unsigned index, int32_t t)
{
    const uint8_t *obj = pad_obj(app, index);
    if (fw_param(obj, PARAM_MODE) != MODE_CLIP)
        return t;
    int q = fw_param(obj, PARAM_QUANT);
    if (q < 0 || q >= QUANTS || t < 0)
        return t;
    int32_t grid = (int32_t)ticks(QUANT_BEATS[q]);
    if (grid <= 0 || t > INT32_MAX - grid)
        return t;
    return (t + grid - 1) / grid * grid;
}

/* The pattern being recorded into. Two time units meet here: engine time (the events, the clip's launch grid) runs
   at 960 ticks per beat, while a pattern counts 960 ticks per STEP whatever the step length (the stock recorder
   stores step * 960; the editor and the player read it that way). With 1/16 steps, one step = 240 engine ticks. */
struct pattern {
    void *list;
    uint32_t seq, layer;         /* engine id of the sequence, layer */
    int32_t step;                /* engine ticks per step */
    int32_t len;                 /* loop length in pattern ticks (steps * 960) */
    int32_t len_e;               /* loop length in engine ticks (steps * step) */
    int32_t origin;              /* engine time at which this pass started */
};

#define PATTERN_TICKS_PER_STEP 960

static int32_t nearest(int32_t t, int32_t grid)                 /* nearest multiple of grid (> 0) */
{
    int32_t lo = t / grid * grid;
    if (lo > t)
        lo -= grid;
    return (t - lo) * 2 >= grid ? lo + grid : lo;
}

static int32_t to_pattern(const struct pattern *p, int32_t e)   /* engine ticks (>= 0) -> pattern ticks */
{
    return e / p->step * PATTERN_TICKS_PER_STEP + (e % p->step) * PATTERN_TICKS_PER_STEP / p->step;
}

static int32_t to_engine(const struct pattern *p, int32_t t)    /* pattern ticks (>= 0) -> engine ticks */
{
    return t / PATTERN_TICKS_PER_STEP * p->step + (t % PATTERN_TICKS_PER_STEP) * p->step / PATTERN_TICKS_PER_STEP;
}

static void pattern_of(uint8_t *app, struct pattern *p)
{
    const uint8_t *cell = fw_cell_obj(app, app + APP_CELL_CTX, 0);
    p->layer = (uint32_t)fw_param(cell, PARAM_LAYER) & 0xff;
    uint8_t *layer = fw_cell_obj(app, app + APP_CELL_CTX, p->layer);
    p->list = layer + 0x18;
    unsigned id = *(const uint16_t *)(app + APP_CELL_CTX + 4);
    p->seq = ((id >> 8) & 31) << 16 | ((id >> 4) & 15) << 8 | (id & 15);
    int si = fw_param(layer, PARAM_STEP);
    int32_t step = (int32_t)(si >= 0 && si < STEP_LENGTHS ? ticks(STEP_BEATS[si]) : TICKS_PER_BEAT / 4);
    if (step <= 0)
        step = TICKS_PER_BEAT / 4;
    int steps = fw_param(layer, PARAM_STEPS);
    if (steps < 0)
        steps = 0;
    p->step = step;
    p->len = steps * PATTERN_TICKS_PER_STEP;
    p->len_e = steps * step;

    /* As the recorder: the pass began `current step` steps ago, snapped to the sequence's launch grid. The step the
       sequencer reports can be a step or two behind the engine clock, so when that leaves us within four steps of
       a bar line, the pass began on the bar line (REC+PLAY starts sequences on one; so does a launch Quant of a bar or
       more). */
    int qi = fw_param(cell, PARAM_SEQ_QUANT);
    int32_t q = (int32_t)(qi >= 0 && qi < SEQ_QUANTS ? ticks(SEQ_QUANT_BEATS[qi]) : 0);
    if (q < step)
        q = step;
    int32_t pos[3];
    fw_seq_pos(pos, app, app + APP_CELL_CTX);
    int64_t now;
    fw_now(&now, HANDLE);
    int32_t base = (int32_t)now - pos[0] * step;
    int32_t bar = 4 * TICKS_PER_BEAT;
    if (q < bar) {
        int32_t b = nearest(base, bar);
        if (base - b <= 4 * step && b - base <= 4 * step)
            q = bar;
    }
    p->origin = nearest(base, q);
}

/* Where engine time t falls in the pattern, in pattern ticks. */
static uint32_t place(const struct pattern *p, int32_t t)
{
    if (p->len_e <= 0)
        return 0;
    int32_t r = (t - p->origin) % p->len_e;
    return (uint32_t)to_pattern(p, r < 0 ? r + p->len_e : r);
}

static uint32_t ahead(uint32_t from, uint32_t to, uint32_t len)  /* distance forward around the loop */
{
    return len ? (to + len - from % len) % len : to - from;
}

static int find_note(struct pattern *p, uint32_t id, struct note *out)
{
    for (unsigned k = 0; fw_list_get(p->list, k, out); k++)
        if (out->type == 1 && out->id == id)
            return 1;
    return 0;
}

/* A note of `pad` sounding at pattern position `at` (begun before it, not ended): its distance into the note. */
static int covering(struct pattern *p, uint16_t pad, uint32_t at, struct note *n, uint32_t *into)
{
    for (unsigned k = 0; fw_list_get(p->list, k, n); k++) {
        if (n->type != 1 || n->pad != pad || n->pitch != 0)
            continue;
        uint32_t d = ahead(n->start, at, (uint32_t)p->len);
        if (d > 0 && d < n->len) {
            *into = d;
            return 1;
        }
    }
    return 0;
}

/* The note `id` has just been written: fold in every other note of the same pad it overlaps or touches. */
static void merge(struct pattern *p, uint32_t id)
{
    struct note n, o;
    uint32_t len = (uint32_t)p->len;
    if (!find_note(p, id, &n) || !len)
        return;
    int changed = 0;
    for (int again = 1; again;) {
        again = 0;
        for (unsigned k = 0; fw_list_get(p->list, k, &o); k++) {
            if (o.type != 1 || o.id == n.id || o.pad != n.pad || o.pitch != n.pitch)
                continue;
            uint32_t d = ahead(n.start, o.start, len), e = ahead(o.start, n.start, len);
            if (d <= n.len) {                                    /* o starts inside or right at the end of n */
                if (d + o.len > n.len)
                    n.len = d + o.len;
            } else if (e <= o.len) {                             /* n starts inside or right at the end of o */
                n.start = o.start;
                n.len = (e + n.len > o.len) ? e + n.len : o.len;
            } else {
                continue;
            }
            if (n.len > len)
                n.len = len;
            if (o.id == 0) {                                     /* (n.id is not 0 then: o.id == n.id is skipped) */
                /* id 0 cannot be removed (to the pattern list it means "everything"): o stays, n goes */
                uint32_t gone = n.id;
                uint32_t start = n.start, span = n.len;
                n = o;
                n.start = start;
                n.len = span;
                fw_list_del(p->list, gone);
                fw_eng_del(HANDLE, p->seq, gone, p->layer);
            } else {
                fw_list_del(p->list, o.id);
                fw_eng_del(HANDLE, p->seq, o.id, p->layer);
            }
            changed = again = 1;
            break;
        }
    }
    if (changed) {
        fw_list_set(p->list, &n);
        fw_eng_set(HANDLE, p->seq, &n, 0, p->layer);
    }
}

/* Write a note for the pad of `tmpl` from engine time `from` to `to`, then join it with what it overlaps. */
static void add_note(uint8_t *app, struct pattern *p, const uint8_t *tmpl, int32_t from, int32_t to, int32_t at)
{
    if (to <= from || p->len <= 0 || p->len_e <= 0)
        return;
    uint8_t probe[EV_SIZE];
    for (int i = 0; i < EV_SIZE; i++)
        probe[i] = tmpl[i];
    probe[EV_TYPE] = EV_PRESS;
    *(uint16_t *)(probe + EV_PAD) = PROBE_PAD;
    *(int64_t *)(probe + EV_TIME) = at;                  /* a time the recorder can place (not ahead of now) */
    volatile uint32_t *next = (volatile uint32_t *)(app + APP_NEXT_ID);
    if (*next == 0)
        *next = 1;                                       /* id 0 means "no id" to the firmware: never hand it out */
    uint32_t id = *next;
    fw_record(app, probe);                               /* app copy only: the engine hears of a note when set */
    struct note n;
    if (!find_note(p, id, &n))
        return;
    n.pad = *(const uint16_t *)(tmpl + EV_PAD);
    n.pitch = 0;
    n.start = place(p, from);
    n.len = (uint32_t)to_pattern(p, to - from < p->len_e ? to - from : p->len_e);
    fw_list_set(p->list, &n);
    fw_eng_set(HANDLE, p->seq, &n, 0, p->layer);
    merge(p, id);
}


/* Close the open take of pad `index` at engine time `stop` (already on the grid). */
static void close_take(uint8_t *app, struct pattern *p, unsigned index, int32_t stop, int32_t at)
{
    const uint8_t *press = (const uint8_t *)S->press[index];
    uint8_t tmpl[EV_SIZE];
    for (int i = 0; i < EV_SIZE; i++)
        tmpl[i] = press[i];
    S->held &= (uint16_t)~(1u << index);
    add_note(app, p, tmpl, on_grid(app, index, ev_time(tmpl)), stop, at);
    MUTE->pads &= (uint16_t)~(1u << index);              /* the sequence may play this pad again */
}

/* Replaces the pump's call to the recorder (bl @0x080a2946). */
void cliprec_record(uint8_t *app, uint8_t *ev)
{
    ensure();
    S->app = app;
    unsigned type = ev[EV_TYPE];
    int toggle = app[APP_RECORDING] == 1 && (type == EV_PRESS || type == EV_RELEASE) && is_toggle(app, ev);
    if (!toggle) {
        fw_record(app, ev);
        return;
    }
    if (type == EV_RELEASE) {
        return;                                          /* a tap's release says nothing on a Toggle pad */
    }
    unsigned index = pad_index(ev);
    int32_t t = ev_time(ev), at = on_grid(app, index, t);
    struct pattern p;
    pattern_of(app, &p);
    if (S->held & (1u << index)) {
        close_take(app, &p, index, at, t);               /* the clip stops here: the take is written */
        return;
    }
    struct note n;
    uint32_t into;
    if (covering(&p, *(const uint16_t *)(ev + EV_PAD), place(&p, at), &n, &into) && S->quiet[index] - at <= 0) {
        S->quiet[index] = at + to_engine(&p, (int32_t)(n.len - into));  /* the sequence is playing it: stopped
                                                            live until that note ends; the pattern is left alone */
        return;
    }
    for (int i = 0; i < EV_SIZE; i++)
        S->press[index][i] = ev[i];                      /* the clip starts here: wait for the tap that stops it */
    S->quiet[index] = at;
    S->held |= (uint16_t)(1u << index);
    MUTE->pads |= (uint16_t)(1u << index);               /* older notes under the take must not cut or restart it */
}

/* Replaces the pump's timed-event read (bl @0x080a2936): once recording has stopped, write the takes still open,
   ending at the clip's next grid line from now. The recorder only works while recording, so it is let in for that. */
int cliprec_pop(void *queue, uint8_t *ev)
{
    int got = fw_pop(queue, ev);
    if (got)
        return got;
    ensure();
    uint8_t *app = S->app;
    if (!S->held || !app || app[APP_RECORDING] == 1)
        return got;
    int64_t now64;
    fw_now(&now64, HANDLE);
    int32_t now = (int32_t)now64;
    uint8_t saved = app[APP_RECORDING];
    app[APP_RECORDING] = 1;
    struct pattern p;
    pattern_of(app, &p);
    for (unsigned index = 0; index < PADS; index++) {
        if (!(S->held & (1u << index)))
            continue;
        close_take(app, &p, index, on_grid(app, index, now), now);
    }
    app[APP_RECORDING] = saved;
    S->held = 0;
    MUTE->pads = 0;
    return got;
}
