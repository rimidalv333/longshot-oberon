"""(2026-11-26, after Longshot #2235 by the Longshot contributors; adapted to this tree's lift) RX_FP_NATIVE: the lifted code's
float ops as plain host ops, with no FPSR bookkeeping.

The lift keeps Arm's float results bit for bit, FPSR's cumulative flags included: every FPX1 guard computes the native result
and keeps it only when it provably raised no flag FPSR lacks; the fast float regions (nx2/fastfp.py) run their ops in an MXCSR
window as asm volatile and fall back to an exact copy of the region; the vector routes send a lane that is not finite (a NaN
lane, the Havok cloth's unused fourth lane: Longshot #2355's case) to the per-lane exact helpers. The game reads FPSR in no
function play runs (its MRS FPSR are in libc's fenv routines). Its FPCR is round to nearest with no DN; FZ is set and cleared
by the game itself, and this tree's host already makes the thread's MXCSR follow it (host/hos_thread.c hos_fpcr_sync: FTZ and
DAZ, at every lifted MSR FPCR and at a thread's and a fiber's start; ReLink 0a05c24). Then SSE's add, sub, mul, div, sqrt and
FMA give Arm's value but for a NaN's encoding (and, under FZ, a result rounded up to exactly 2^-126, where x86 detects
tininess after rounding and Arm before).

What this does: copies of the lift's two float headers with every keep test true, the vector routes' bad-lane tests empty, the
fast regions' ops plain intrinsics and their MXCSR window and guards gone, put ahead of the lift's on the include path
(ps5/build.py). Conversions, min/max, compares and the FRECPE/FRSQRTE estimates are left exact. The hot functions' own native
helpers (a64_hot_fp.inc, A64_NATIVE_FP) are not changed: they are native already.

A ceiling, not a proof: FPSR's flags and NaN encodings are no longer Arm's, and nothing checks that the game never reads them.
Upstream measured +12 % fps in the Market and +7 % at Lake Hylia on a PC whose lift had no native path at all; here the 680 hot
functions are native already, so the gain is the rest of the game's code. RX_FP_NATIVE unset or 0 (the default): the lift's
headers as they are, the build as before."""
import os
import re

HEADERS = ("a64_suyu.h", "a64_fpx.h")


def on(environ=os.environ):
    return environ.get("RX_FP_NATIVE", "0").strip().lower() not in ("0", "off", "false", "no", "")


ONE = r"\1 1"
# (upstream's, less its FPCR-write hook: this tree's lifted MSR FPCR calls hos_fpcr_sync already)
SUYU = [
    (r"^(#define RECOMP_FPX_OPEN\(c\)).*$", r"\1 ((void)(c), 1)", 1),
    (r"^(#define RECOMP_FPX_FIN(?:32|64)\(t\)).*$", ONE, 2),
    (r"^(#define RECOMP_FPX_BIG(?:32|64)\(t\))(?:.*\\\n)*.*$", ONE, 2),
    (r"^(#define RECOMP_FPX_KEEP(?:32|64)\(t, a, b\))(?:.*\\\n)*.*$", ONE, 2),
    (r"^(#define RECOMP_FPX_KEEPDIV(?:32|64)\(t, a\)).*$", ONE, 2),
    (r"^(#define RECOMP_FPX_FZOK(?:32|64)\(a, b\)).*$", ONE, 2),
    (r"^(#define RECOMP_FPX_FZSUM(?:32|64)\(t, a, b\))(?:.*\\\n)*.*$", ONE, 2),
    (r"^(#define RECOMP_FPX_MIDPOINT\(s\)).*$", r"\1 0", 1),
]
OP = ("#define A64_FF_OP(ins, d, a, b) ((d) = (ins)[0] == 'a' ? _mm_add_ps((a), (b)) : (ins)[0] == 's' ? "
      "_mm_sub_ps((a), (b)) : (ins)[0] == 'm' ? _mm_mul_ps((a), (b)) : _mm_div_ps((a), (b)))")
FN = r"static inline __attribute__\(\(always_inline\)\) "
FPX = [
    # the vector routes (A64_VFPX_SUM's add and sub, and mul): no lane is bad, and FZ's own lane tests go (MXCSR has FTZ/DAZ)
    (r"__m128i bad = a64_v_nonfin\(t\), r = t; \\$", r"__m128i bad = _mm_setzero_si128(), r = t; \\", 1),
    (r"^(        )if \(RECOMP_FPX_FZ\(c\)\) \{ \\$", r"\1if (0) { \\", 1),
    (r"__m128i bad = _mm_andnot_si128\(keep, _mm_set1_epi32\(-1\)\), r = t;$",
     r"__m128i bad = _mm_setzero_si128(), r = t; (void)keep;", 1),
    (r"^(        )if \(RECOMP_FPX_FZ\(c\)\) \{\n(            bad = _mm_or_si128\(bad, _mm_or_si128\(a64_v_subn\(a\), a64_v_subn\(b\)\)\);\n"
     r"            if \(c->fpsr & 8u\) \{   /\* \(flags raised: a tiny product)", r"\1if (0) {\n\2", 1),
    # the fast regions
    (r'^#define A64_FF_OP\(ins, d, a, b\) __asm__ volatile\("v".*$', OP, 1),
    (r'^#define A64_FF_SQRT\(d, a\) __asm__ volatile\("vsqrtss.*$', "#define A64_FF_SQRT(d, a) ((d) = _mm_sqrt_ps(a))", 1),
    # (the fast region's multiply-add stays the kit's asm: the console's flags do not enable FMA for the compiler's intrinsics,
    # and the asm needs no feature flag; its MXCSR window is gone all the same)
    ("(" + FN + r"unsigned a64_ff_begin\(const Ctx \*c\) \{)", r"\1 (void)c; return 0;", 1),
    ("(" + FN + r"int a64_ff_end\(Ctx \*c, unsigned m\) \{)", r"\1 (void)c; (void)m; return 1;", 1),
    ("(" + FN + r"void a64_ff_spoil_\(void\) \{)", r"\1 return;", 1),
    ("(" + FN + r"void a64_ff_guard_\(uint64_t v, int tiny\) \{)", r"\1 (void)v; (void)tiny; return;", 1),
    ("(" + FN + r"void a64_ff_vguard_\(__m128 r, int k, int tiny\) \{)", r"\1 (void)r; (void)k; (void)tiny; return;", 1),
]


def patched(name, text):
    for pat, rep, want in SUYU if name == "a64_suyu.h" else FPX:
        text, n = re.subn(pat, lambda m, rep=rep: m.expand(rep), text, flags=re.M)
        if n != want:
            raise SystemExit("RX_FP_NATIVE: %s: %r matched %d times, not %d (the lift's header changed)" % (name, pat, n, want))
    return "/* RX_FP_NATIVE (nx2/fp_native.py): a copy with the FPSR bookkeeping taken out */\n" + text


def write(gen, out):
    """The native copies of gen's float headers in out; returns out"""
    os.makedirs(out, exist_ok=True)
    for name in HEADERS:
        with open(os.path.join(gen, name), encoding="utf-8") as f:
            text = patched(name, f.read())
        dst = os.path.join(out, name)
        if not os.path.isfile(dst) or open(dst, encoding="utf-8").read() != text:
            with open(dst, "w", encoding="utf-8", newline="\n") as f:
                f.write(text)
    return out
