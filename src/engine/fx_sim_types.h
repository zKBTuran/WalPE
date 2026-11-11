/* fx_sim_types.h - simulation chains: state and entry points (see fx_sim.h)
 *
 * Included by scenenode_effects.c right after its EffectChain type; the
 * functions are defined in fx_sim.h, further down in the same file. */

#define FX_SIM_MAX_FN 4

/* how a run of a simulation chain goes (FxSim.mode) */
enum {
    FXSIM_RUN_STEP = 0,   /* one round of the pass list per step that is due */
    FXSIM_RUN_PRESENT,    /* no step is due: only the passes that draw the picture */
    FXSIM_RUN_REUSE,      /* ... and the picture drawn last still holds: no pass */
};

struct FxSim {
    /* ---- what the pass list is (build time) */
    guint8 step_pass[FX_MAX_PASSES];   /* 1 = belongs to a step: it writes a named
                                        * target, copies or swaps. 0 = it presents:
                                        * it draws the chain's output from the fields */
    int n_step, n_present;
    gboolean present_vol;      /* a presenting pass reads time, the pointer, audio,
                                * album art or the frame below: its picture is
                                * new in every frame also without a step */
    gboolean reuse_ok;         /* one presenting pass and it is the last draw:
                                * the output may be shown again as it is */
    gboolean broken;           /* a field declared as float cannot be one here:
                                * the chain is left out (as with the switch off) */
    /* "functions" of the effect file: named clears of some of its targets */
    struct { char name[48]; guint targets; } fn[FX_SIM_MAX_FN];
    int n_fn;
    /* ---- the simulation's clock */
    gboolean started;          /* a run has taken the clock */
    double clock_last;         /* the thread's simulation clock at the last frame
                                * this chain was scheduled in */
    double acc;                /* scene time not simulated yet (fluidsim.h) */
    guint frame;               /* that frame (the pool's frame count + 1; 0 = none) */
    double sim_time;           /* seconds simulated since the last reset */
    guint64 steps;             /* steps run since the chain was built */
    guint serial;              /* bumped by every step and every reset */
    gboolean swapped;          /* the pass list ran to its end at least once since
                                * the last reset: the swaps have been applied */
    WalpeFluidPointer ptr;
    /* ---- the run in progress */
    int mode;                  /* FXSIM_RUN_* */
    int round, rounds;
    double run_h;              /* the step's length in seconds */
    gboolean sched;            /* this run scheduled the frame (the first of it) */
    float u_dt, u_time;        /* g_Frametime / g_Time of the round's step passes */
    float u_cur[2], u_last[2]; /* the pointer for them */
    float u_pcur[2];           /* the pointer for presenting passes */
    float map[16];             /* g_EffectTextureProjectionMatrixInverse */
    /* ---- the picture drawn last, and what it was drawn from */
    struct {
        gboolean valid;
        GLuint input, out;
        guint gen, set_gen, serial;
        int w, h, fullw, fullh, sw, sh;
        float k;
    } shown;
    int field_div;             /* the divisor the picture-sized fields were made with */
    guint resets;              /* diagnostics */
    gboolean limit_said;       /* the one line "targets smaller than the effect
                                * file asks for" was written for this chain */
};

/* render thread: the values of the frame being drawn that only simulation
 * chains look at */
static __thread struct {
    double clock;              /* seconds: the frame times added up */
    gboolean ptr_absent;       /* fx_set_pointer_state: no pointer in this frame */
    gboolean ptr_down;
    gboolean map_set;          /* fx_set_pointer_map, for the next run ... */
    gboolean map_live;         /* ... and taken by the run in progress */
    float map[9];
    guint set_gen;             /* the generation of the set whose chains run */
    gboolean input_static;     /* the next chain run reads a texture whose pixels
                                * only change with its name */
    guint64 steps;             /* diagnostics: steps run / runs that drew no pass */
    guint reused, presented;
} fxsim_frame;

/* TRUE while the passes of a simulation chain are being built */
static __thread gboolean fxsim_building;

/* render thread (one GL context each): GL_MAX_TEXTURE_SIZE, asked once;
 * 0 = not asked yet (fxsim_tex_max) */
static __thread int fxsim_tex_max_v;

static void fxsim_new_frame (float frametime);
static gboolean fxsim_scan (JsonArray *passes);
static gboolean fxsim_condition (JsonObject *def, JsonArray *instpasses);
static void fxsim_target_parse (EffTarget *t, JsonObject *f);
static gboolean fxsim_uniform (EffPass *pass, const char *name, GLint loc, GLenum type);
static void fxsim_chain_built (EffectChain *ch, JsonObject *eo);
static void fxsim_chain_free (EffectChain *ch);
static gboolean fxsim_off (const EffectChain *ch);
static void fxsim_ensure_targets (EffectChain *ch, int w, int h, float k);
static int fxsim_begin (EffectChain *ch, GLuint input, int w, int h, int fullw, int fullh, float k);
static gboolean fxsim_skips (const EffectChain *ch, int pass);
static gboolean fxsim_next_round (EffectChain *ch);
static void fxsim_end (EffectChain *ch, GLuint input, GLuint output, int w, int h, int fullw,
                       int fullh, float k);
static void fxsim_pointer (const EffectChain *ch, int pass, float cur[2], float last[2]);
static guint64 fxsim_chain_px (EffectChain *ch, int rw, int rh, guint *passes);
