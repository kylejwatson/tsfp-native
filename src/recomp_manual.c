/**
 * Manual function overrides and ICALL diagnostics
 *
 * This file provides:
 *   - recomp_lookup_manual()  : intercept specific Xbox VAs with hand-written code
 *   - recomp_icall_fail_log() : log when an indirect call target can't be resolved
 *   - ICALL trace ring buffer  : globals used by the RECOMP_ICALL macro
 *
 * The recomp pipeline generates an auto-dispatch table (recomp_lookup) that
 * resolves most function addresses. recomp_lookup_manual() is called FIRST,
 * giving you a chance to override any function with a custom implementation.
 *
 * Common reasons to add manual overrides:
 *   - Trace a function to understand call flow (wrap the generated version)
 *   - Fix a function the lifter translated incorrectly
 *   - Stub out a function that crashes (return early, set eax to a safe value)
 *   - Redirect a function to a native implementation (e.g., skip CRT init)
 *   - Intercept D3D/audio calls for custom rendering or sound
 */

#include <stdio.h>
#include <stddef.h>   /* ptrdiff_t */
#include <stdint.h>
#include <stdlib.h>   /* getenv, exit: the spin verdict below */
#include <string.h>   /* strcmp: RECOMP_ICALL_FATAL */
#include <math.h>     /* floorf, fabsf: mounted-gun angle limits */
#include <time.h>     /* timespec_get: menus with the mouse */

/* One diagnostic report, one piece.
 *
 * Each report below is a dozen fprintf calls and the CRT takes its lock per
 * call, so with more than one guest thread running the lines interleave in
 * the middle of a report. Dino Crisis 3 runs six workers once it is past its
 * menus, and its log came out like this:
 *
 *   callers: 0x004DB424 <- 0x002705B4 <- 0x002705B4[ICALL] unresolved jump
 *   target 0x41B0B870 -- 1 time(s) (total calls: 6930990)
 *
 * -- two threads' reports spliced together, with a caller list that stops
 * mid-sentence and an address that belongs to neither line as read. These
 * diagnostics exist to be read during exactly the multi-threaded bring-up
 * that breaks them.
 *
 * The CRT exposes the lock these calls were already taking one at a time, so
 * hold it across the whole report instead. */
#if defined(_MSC_VER)
#  define RECOMP_DIAG_LOCK()   _lock_file(stderr)
#  define RECOMP_DIAG_UNLOCK() _unlock_file(stderr)
#elif defined(__unix__) || defined(__APPLE__)
#  define RECOMP_DIAG_LOCK()   flockfile(stderr)
#  define RECOMP_DIAG_UNLOCK() funlockfile(stderr)
#else
#  define RECOMP_DIAG_LOCK()   ((void)0)
#  define RECOMP_DIAG_UNLOCK() ((void)0)
#endif

/* ── ICALL trace ring buffer ───────────────────────────────── */

/*
 * These globals are written by the RECOMP_ICALL macro (defined in
 * recomp_types.h) every time an indirect call is dispatched. When a
 * crash occurs, the VEH handler or recomp_icall_fail_log() can dump
 * the last 16 call targets to help you trace what happened.
 *
 * The runtime owns them: xbox_kernel defines all three in
 * src/kernel/xbox_memory_layout.c, and recomp_types.h declares them extern.
 * Declare, do not define -- a definition here as well is a duplicate symbol,
 * and a project copied from this template failed to link on all three:
 *
 *   xbox_memory_layout.obj : error LNK2005: g_icall_count already defined
 *                            in recomp_manual.obj
 */
/* Runtime data the game module reads. A Windows DLL reaches data in the
 * executable only through __declspec(dllimport) (RECOMP_MODULE_DLL is set
 * for the module's sources); everywhere else this is nothing. */
#ifndef RECOMP_RUNTIME_DATA
#  if defined(_WIN32) && defined(RECOMP_MODULE_DLL)
#    define RECOMP_RUNTIME_DATA __declspec(dllimport)
#  else
#    define RECOMP_RUNTIME_DATA
#  endif
#endif
extern RECOMP_RUNTIME_DATA volatile uint32_t g_icall_trace[16];
extern RECOMP_RUNTIME_DATA volatile uint32_t g_icall_trace_idx;
extern RECOMP_RUNTIME_DATA volatile uint64_t g_icall_count;

typedef void (*recomp_func_t)(void);

/* ── Register state (defined in xbox_memory_layout.c) ──────── */

/* g_eax, g_esp and the rest are fields of the per-thread register block that
 * xbox_memory_layout.h declares; the header is the one way to reach them (a
 * declaration of our own once silently read other storage, every value 0).
 * RECOMP_MANUAL_TLS is for this file's own thread-locals. */
#include "xbox_memory_layout.h"
#if defined(_MSC_VER)
#  define RECOMP_MANUAL_TLS __declspec(thread)
#elif defined(__GNUC__) || defined(__clang__)
#  define RECOMP_MANUAL_TLS __thread
#else
#  define RECOMP_MANUAL_TLS _Thread_local
#endif

/* The guest stack pointer. At the moment an indirect call is refused, the
 * caller has already pushed its guest return address, so the top of the
 * guest stack is the call site -- the one thing the old log did not say. */
extern RECOMP_RUNTIME_DATA ptrdiff_t g_xbox_mem_offset;
/* The lifted code sections, so a value on the guest stack can be told
 * apart from data when naming the callers of a refused call. */
extern RECOMP_RUNTIME_DATA uint32_t g_xbox_code_lo;
extern RECOMP_RUNTIME_DATA uint32_t g_xbox_code_hi;
/* The esp the dispatch macro captured. Not g_esp, which is stale by the time
 * a refused call is reported. Zero when the title's generated header predates
 * this, and the log then says it has no callers rather than inventing them. */
/* Which dispatch form was refused: 0 unknown, 1 call, 2 jump. Unknown
 * means this title was lifted before the macros published it. */

/* ── TimeSplitters: Future Perfect overrides ───────────────── */

/* sub_000245C0: the MSVC CRT memcpy (cdecl: dst, src, n; returns dst).
 *
 * Replaced because it enters its unrolled `movsb` tail with a computed
 * `jmp ecx` to a point in the middle of the function (0x24604..0x2460C), which
 * the lifter cannot translate: the load stopped at "[ICALL] unresolved jump
 * target 0x0002460C". A native copy is also much faster than the lifted
 * string/MMX loops. memmove, since a caller may rely on the overlap-safe
 * behaviour the original had for forward-overlapping blocks. */
void sub_000245C0(void)
{
    uint32_t dst = *(uint32_t *)((uint8_t *)g_xbox_mem_offset + g_esp + 4);
    uint32_t src = *(uint32_t *)((uint8_t *)g_xbox_mem_offset + g_esp + 8);
    uint32_t n   = *(uint32_t *)((uint8_t *)g_xbox_mem_offset + g_esp + 12);

    memmove((uint8_t *)g_xbox_mem_offset + dst,
            (uint8_t *)g_xbox_mem_offset + src, n);
    g_eax = dst;
    g_esp += 4;     /* the `ret` */
}

/* sub_0025D520 (player_apply_look, re/names.json): mouse look.
 *
 * Wrapped, not replaced: the generated body runs first and integrates the
 * stick into the camera angles as it always did, then the mouse movement
 * since the last frame is added to them directly -- degrees per count, no
 * stick, so no deadzone, response curve or acceleration ramp between the
 * hand and the aim. The pad keeps working beside it.
 *
 * What the stick path does that is kept: it only turns while the player has
 * control, in the movement states that look at all, and while the game has
 * not frozen or neutralised the pad (tsfp_mouse_blocked); vertical movement
 * counts as look-y input for the auto-lookahead (tsfp_mouse_cancel_lookahead);
 * the moving crosshair leads a mouse turn as it does a stick's
 * (tsfp_mouse_aim_lead); the yaw wraps into [0,360) and the pitch is clamped to +-g_pitch_limit (60),
 * and the turn is scaled by the camera's fov over its default one so a zoomed
 * scope does not swing wildly. Not kept: the 0.65 slow-down while aiming
 * (aim_turn_scale), which is there for a thumb, and auto-aim.
 *
 * A frame's movement is what arrived up to the title's last Swap
 * (recomp_mouse_frame_mark), not up to now, so a host that presents late
 * does not hand the next frame its lateness as extra turn.
 *
 * Pitch: positive looks up; mouse y grows downwards, so it is subtracted.
 * Yaw: grows to the left (counter-clockwise from above), so mouse x, which
 * grows to the right, is subtracted too (it was added at first: inverted).
 * Checked in Brain Drain with RECOMP_INPUT_SEQ's mouse steps: 1000 counts
 * right turned 55.00 degrees, 300 up raised the view 16.50 to the ceiling. */
#include "recomp_mouse.h"
#include "tsfp_names.h"
#include "recomp_config.h"

extern void sub_0025D520_gen(void);

#define GUEST_U32(va) (*(volatile uint32_t *)((uint8_t *)g_xbox_mem_offset + (uint32_t)(va)))
#define GUEST_F32(va) (*(volatile float *)((uint8_t *)g_xbox_mem_offset + (uint32_t)(va)))
#define GUEST_S8(va)  (*(volatile int8_t *)((uint8_t *)g_xbox_mem_offset + (uint32_t)(va)))
#define GUEST_U8(va)  (*(volatile uint8_t *)((uint8_t *)g_xbox_mem_offset + (uint32_t)(va)))

/* Mounted guns and seats, the two move states the walking path above skips.
 *
 * 0x80 (player_look_mounted): the stick turns the *object*, not the player:
 * its data (object+0x7C) holds pitch +0x1C, which the stick y is subtracted
 * from, and yaw +0x24, which the stick x is added to, each kept in its arc
 * by angle_limit; +0x5C scales both when +0x08 has 0x1000000. Objects of
 * type 1 and 9 are not turned at all. 0x40 (player_look_seated): the stick
 * turns Player.yaw/pitch, which are then held inside the object's arcs
 * measured from its heading (object+0x70). Both use the same degrees per
 * count as on foot; the limits are reapplied after the mouse is added, the
 * way the game applies them after the stick. */
#define TSFP_CHARDATA_MOUNT      0xC14u  /* character data -> object used */
#define TSFP_OBJECT_DATA         0x07Cu
#define TSFP_OBJECT_HEADING      0x070u
#define TSFP_MOUNT_FLAGS         0x008u
#define TSFP_MOUNT_TYPE          0x010u
#define TSFP_MOUNT_PITCH         0x01Cu
#define TSFP_MOUNT_YAW           0x024u
#define TSFP_MOUNT_PITCH_LO      0x034u  /* lo == hi: the axis is fixed */
#define TSFP_MOUNT_PITCH_HI      0x038u
#define TSFP_MOUNT_YAW_LO        0x03Cu
#define TSFP_MOUNT_YAW_HI        0x040u
#define TSFP_MOUNT_SENSITIVITY   0x05Cu

static float tsfp_wrap180(float a)
{
    return a - floorf((a + 180.0f) / 360.0f) * 360.0f;
}

/* angle_limit (0x00153F90), for the lo != hi case its callers use it in. */
static float tsfp_angle_limit(float a, float lo, float hi)
{
    if (fabsf(hi - lo) >= 360.0f)
        return tsfp_wrap180(a);
    if (a < lo)
        return lo;
    if (a > hi)
        return hi;
    return a;
}

static void tsfp_mouse_look_mounted(uint32_t state, uint32_t mount,
                                    float dyaw, float dpitch)
{
    uint32_t d = GUEST_U32(mount + TSFP_OBJECT_DATA);
    float lo, hi;

    if (!d)
        return;
    if (state == 0x80) {
        uint32_t type = GUEST_U32(d + TSFP_MOUNT_TYPE);
        float yaw, pitch;

        if (type == 1 || type == 9)
            return;
        if (GUEST_U32(d + TSFP_MOUNT_FLAGS) & 0x1000000u) {
            dyaw *= GUEST_F32(d + TSFP_MOUNT_SENSITIVITY);
            dpitch *= GUEST_F32(d + TSFP_MOUNT_SENSITIVITY);
        }
        /* The object's pitch runs the other way to the player's. */
        pitch = GUEST_F32(d + TSFP_MOUNT_PITCH) + dpitch;
        yaw = GUEST_F32(d + TSFP_MOUNT_YAW) + dyaw;
        lo = GUEST_F32(d + TSFP_MOUNT_PITCH_LO);
        hi = GUEST_F32(d + TSFP_MOUNT_PITCH_HI);
        pitch = lo == hi ? lo : tsfp_angle_limit(pitch, lo, hi);
        lo = GUEST_F32(d + TSFP_MOUNT_YAW_LO);
        hi = GUEST_F32(d + TSFP_MOUNT_YAW_HI);
        yaw = lo == hi ? lo : tsfp_angle_limit(yaw, lo, hi);
        GUEST_F32(d + TSFP_MOUNT_PITCH) = pitch;
        GUEST_F32(d + TSFP_MOUNT_YAW) = yaw;
    } else {
        uint32_t p = GUEST_U32(TSFP_G_PLAYER);
        float heading = GUEST_F32(mount + TSFP_OBJECT_HEADING);
        float yaw = tsfp_wrap180(GUEST_F32(p + TSFP_PLAYER_YAW) + dyaw - heading);
        float pitch = tsfp_wrap180(GUEST_F32(p + TSFP_PLAYER_PITCH) - dpitch);

        lo = GUEST_F32(d + TSFP_MOUNT_PITCH_LO);
        hi = GUEST_F32(d + TSFP_MOUNT_PITCH_HI);
        if (lo != hi)
            pitch = tsfp_angle_limit(pitch, lo, hi);
        lo = GUEST_F32(d + TSFP_MOUNT_YAW_LO);
        hi = GUEST_F32(d + TSFP_MOUNT_YAW_HI);
        if (lo != hi)
            yaw = tsfp_angle_limit(yaw, lo, hi);
        GUEST_F32(p + TSFP_PLAYER_YAW) = tsfp_wrap180(yaw + heading);
        GUEST_F32(p + TSFP_PLAYER_PITCH) = pitch;
    }
}

/* pad_neutralise (0x00042870) and pads_update (0x000438B0): which players'
 * pads the game has set to neutral since it last polled them. The game takes
 * the look away from the stick that way rather than through the control flag:
 * level 2's opening camera pan (the script action at 0x00234800) eases every
 * player's yaw and pitch to a target and neutralises pads 0 and 1 each frame
 * while it runs, and player_update does the same when its player is not to
 * be steered. The flag stays set, so the mouse turned the view and the pan
 * pulled it back every frame. A bit per player number, cleared by the next
 * poll. */
extern void sub_00042870_gen(void);
extern void sub_000438B0_gen(void);
static double tsfp_now(void);         /* menus with the mouse, below */
static double s_menu_seen;
static int s_menu_trace = -1;
static uint32_t s_pads_neutral;

void sub_00042870(void)
{
    uint32_t player = GUEST_U32(g_esp + 4);     /* before the body runs */

    if (player < 32)
        s_pads_neutral |= 1u << player;
    sub_00042870_gen();
}

void sub_000438B0(void)
{
    sub_000438B0_gen();
    s_pads_neutral = 0;
    /* No menu of player 1's has read its pads for a while: the mouse is for
     * looking again (tsfp_menu_mouse). */
    if (recomp_mouse_pointer_mode() && tsfp_now() - s_menu_seen > 0.25) {
        recomp_mouse_set_pointer_mode(0);
        if (s_menu_trace > 0)
            fprintf(stderr, "[MENU] pointer mode off\n");
    }
}

/* ── Menus with the mouse ─────────────────────────────────────
 *
 * FP's menu system has a cursor mode of its own (MenuContext.cursor_mode,
 * menu_cursor_on), which only MapMaker turns on: the left stick moves a
 * cursor, the item under it takes the focus (menu_cursor_update, with the
 * focus sound), and A acts on the focused item; value rows of kind 5/6 take
 * a click on their arrows (menu_click_option). Player 1's menus use it for
 * the mouse:
 *
 *   - While one of them reads its pads (menu_read_pads), the host is in
 *     pointer mode (recomp_mouse_set_pointer_mode): the cursor is the
 *     system's own, free and visible, and the buttons are the menu's.
 *     pads_update turns pointer mode off again once no menu has read its
 *     pads for a moment, and the mouse is captured for look as before.
 *   - Moving or clicking the mouse puts the context in cursor mode and the
 *     pointer becomes its cursor; a pad or keyboard press or a deflected
 *     stick takes cursor mode away again, so up/down and left/right work as
 *     they always did. Only cursor modes turned on here are turned off.
 *     The pad is looked at before the menu reads it, so the press that
 *     takes cursor mode away already moves the focus or accepts; and as a
 *     cursor over nothing leaves no focus (menu_cursor_update), the last
 *     item the mouse focused gets it back.
 *   - Left click over an item is A. Value rows of kind 1 (left/right on the
 *     pad, and nothing at all in cursor mode) step back when the click is
 *     left of the middle of their value and forward otherwise. Right click
 *     is B anywhere. A click in a window with nothing to pick (the story
 *     briefing) is A without cursor mode, which the window would otherwise
 *     take as a grab of its scroll bar.
 *   - While the left button is down, A is held: that is what the game's
 *     scroll bar wants in cursor mode (menu_scroll_update scrolls while A
 *     is held on an arrow, menu_cursor_update drags the thumb). Rows scrolled
 *     out of a list are clipped to nothing, so without it a list could be
 *     scrolled down by hovering its half-shown last row but never back up.
 *   - The wheel in a list longer than its window scrolls it a step, as the
 *     arrows do; elsewhere it steps a value row or sends left/right.
 *
 * The cursor is measured in the guest's screen units: FP draws its menus on
 * a 640x480 screen, widescreen or not (the 16:9 picture is that screen
 * stretched), so a point on the picture is that fraction of 640x480.
 * RECOMP_MENU_TRACE=1 logs the context, the pointer and every click. */
extern void sub_00073870_gen(void);
extern void sub_000756A0(void);         /* menu_option_prev */
extern void sub_000755D0(void);         /* menu_option_next */

#define TSFP_SCREEN_W 640.0f
#define TSFP_SCREEN_H 480.0f
#define TSFP_MENU_A    (0x10u | 0x100000u)   /* MenuWindow.pressed: A went down */
#define TSFP_MENU_B    (0x20u | 0x200000u)
#define TSFP_MENU_LEFT  (0x04u | 0x40000u)
#define TSFP_MENU_RIGHT (0x08u | 0x80000u)

static int s_menu_cursor;               /* the mouse is driving player 1's menus */
static uint32_t s_cursor_ours;          /* contexts whose cursor mode is ours */
static int s_menu_pad_frame;            /* the pad took cursor mode away this frame */
static uint32_t s_menu_focus;           /* the last item the mouse focused */
static long s_menu_click_later;         /* a left click held over a frame */
extern void sub_00076EF0(void);         /* menu_focus_item */

static double tsfp_now(void)
{
    struct timespec ts;

    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* Calls a lifted cdecl function with its arguments on the guest stack.
 * The callee pops the return address with its ret; the caller pops the rest. */
static uint32_t tsfp_guest_call(void (*fn)(void), int n, const uint32_t *args)
{
    int i;

    for (i = n - 1; i >= 0; i--) {
        g_esp -= 4;
        GUEST_U32(g_esp) = args[i];
    }
    g_esp -= 4;
    GUEST_U32(g_esp) = 0;               /* return address */
    fn();
    g_esp += 4u * (uint32_t)n;
    return g_eax;
}

/* The enabled item whose screen rectangle holds (sx, sy), or 0. */
static uint32_t tsfp_menu_item_at(uint32_t w, float sx, float sy)
{
    uint32_t it = GUEST_U32(w + TSFP_MENUWINDOW_ITEMS);
    int n;

    for (n = 0; it && n < 1024; it = GUEST_U32(it + TSFP_MENUITEM_NEXT), n++) {
        if (GUEST_U32(it + TSFP_MENUITEM_FLAGS) & 4)
            continue;
        if (sx >= GUEST_F32(it + TSFP_MENUITEM_SX0) && sx < GUEST_F32(it + TSFP_MENUITEM_SX1) &&
            sy >= GUEST_F32(it + TSFP_MENUITEM_SY0) && sy < GUEST_F32(it + TSFP_MENUITEM_SY1))
            return it;
    }
    return 0;
}

/* Where the arrows of value rows were last drawn. The game draws a kind 1
 * row's value with an arrow either side (0x7B7B0, the item in EAX): the
 * left one by 0x81B20, the right one by 0x81AD0, each (x0, y0, x1, y1,
 * colour) in screen units, just outside a value text of the font's width.
 * Taking them from the drawing is simpler than measuring the text. */
#define TSFP_ARROW_ROWS 64
static struct { uint32_t item; float left[4], right[4]; int have; } s_arrows[TSFP_ARROW_ROWS];
static uint32_t s_arrow_item;           /* the row 0x7B7B0 is drawing, or 0 */
static int s_arrow_next;

extern void sub_0007B7B0_gen(void);
extern void sub_00081AD0_gen(void);
extern void sub_00081B20_gen(void);

static int tsfp_arrow_slot(uint32_t item, int make)
{
    int i;

    for (i = 0; i < TSFP_ARROW_ROWS; i++)
        if (s_arrows[i].item == item)
            return i;
    if (!make)
        return -1;
    i = s_arrow_next;
    s_arrow_next = (s_arrow_next + 1) % TSFP_ARROW_ROWS;
    s_arrows[i].item = item;
    s_arrows[i].have = 0;
    return i;
}

static void tsfp_arrow_record(int right)
{
    int i = tsfp_arrow_slot(s_arrow_item, 1), k;
    float *r = right ? s_arrows[i].right : s_arrows[i].left;

    for (k = 0; k < 4; k++)
        r[k] = GUEST_F32(g_esp + 4 + 4u * (uint32_t)k);
    s_arrows[i].have |= right ? 2 : 1;
}

void sub_0007B7B0(void)
{
    s_arrow_item = g_eax;               /* (window, rect), the item in EAX */
    sub_0007B7B0_gen();
    s_arrow_item = 0;
}

void sub_00081AD0(void)
{
    if (s_arrow_item)
        tsfp_arrow_record(1);
    sub_00081AD0_gen();
}

void sub_00081B20(void)
{
    if (s_arrow_item)
        tsfp_arrow_record(0);
    sub_00081B20_gen();
}

/* A click on a value row of kind 1: back when it is nearer the left arrow
 * (from a little before it up to half way to the right one), forward
 * anywhere else on the row, the label included. */
static void tsfp_menu_step_value(uint32_t it, float sx)
{
    int i = tsfp_arrow_slot(it, 0), back = 0;

    if (i >= 0 && s_arrows[i].have == 3) {
        float l0 = s_arrows[i].left[0], mid = (s_arrows[i].left[2] + s_arrows[i].right[0]) * 0.5f;
        back = sx >= l0 - (s_arrows[i].left[2] - l0) && sx < mid;
    }
    if (s_menu_trace)
        fprintf(stderr, "[MENU] value row 0x%08X choice %u/%u arrows %s%.1f-%.1f %.1f-%.1f: %s\n",
                it, GUEST_U32(it + TSFP_MENUITEM_CHOICE), GUEST_U32(it + TSFP_MENUITEM_CHOICE_COUNT),
                i >= 0 && s_arrows[i].have == 3 ? "" : "(none) ",
                i >= 0 ? s_arrows[i].left[0] : 0.0f, i >= 0 ? s_arrows[i].left[2] : 0.0f,
                i >= 0 ? s_arrows[i].right[0] : 0.0f, i >= 0 ? s_arrows[i].right[2] : 0.0f,
                back ? "back" : "forward");
    tsfp_guest_call(back ? sub_000756A0 : sub_000755D0, 1, &it);
}

/* The player's pad or keyboard was used this frame: a press, or a stick
 * pushed past half way. */
static int tsfp_pad_used(int player)
{
    uint32_t port = GUEST_U32(TSFP_G_PLAYER_PORT + 4u * (uint32_t)player), pad;
    int lx, ly;

    if (port >= 4)
        return 0;
    pad = TSFP_G_PADS + port * TSFP_PADSTATE_SIZE;
    if (GUEST_U32(pad + TSFP_PADSTATE_PRESSED))
        return 1;
    lx = (int)GUEST_U32(pad + TSFP_PADSTATE_LX) - 0x80;
    ly = (int)GUEST_U32(pad + TSFP_PADSTATE_LY) - 0x80;
    return lx > 0x40 || lx < -0x40 || ly > 0x40 || ly < -0x40;
}

/* An enabled item of the window's, or 0 for none. */
static int tsfp_menu_has_item(uint32_t w, uint32_t want)
{
    uint32_t it = GUEST_U32(w + TSFP_MENUWINDOW_ITEMS);
    int n;

    for (n = 0; it && n < 1024; it = GUEST_U32(it + TSFP_MENUITEM_NEXT), n++)
        if (!(GUEST_U32(it + TSFP_MENUITEM_FLAGS) & 4) && (!want || it == want))
            return 1;
    return 0;
}

/* One row of a list window in its scroll units: the tallest enabled item,
 * from screen units (menu_scroll_update keeps scroll * 0x7BA188 in +0xC0). */
static float tsfp_menu_row(uint32_t w)
{
    uint32_t it = GUEST_U32(w + TSFP_MENUWINDOW_ITEMS);
    float k = GUEST_F32(0x007BA188u), row = 0.0f, h;
    int n;

    for (n = 0; it && n < 1024; it = GUEST_U32(it + TSFP_MENUITEM_NEXT), n++) {
        h = GUEST_F32(it + TSFP_MENUITEM_SY1) - GUEST_F32(it + TSFP_MENUITEM_SY0);
        if (!(GUEST_U32(it + TSFP_MENUITEM_FLAGS) & 4) && h > row)
            row = h;
    }
    return k > 0.0f && row > 0.0f ? row / k : GUEST_F32(w + TSFP_MENUWINDOW_SCROLL_STEP);
}

/* A list window's scroll bar, as menu_scroll_bar (0x71400) lays it out:
 * x from the style's bar edges, y the visible area, an arrow at each end as
 * tall as the bar is wide times 0x47839C. 0 when the list fits. */
static int tsfp_menu_bar(uint32_t w, float r[4], float *arrow)
{
    uint32_t st = GUEST_U32(w + TSFP_MENUWINDOW_STYLE), o;
    float k = GUEST_F32(0x007BA188u), view = GUEST_F32(w + TSFP_MENUWINDOW_VIEW_Y1) -
                                             GUEST_F32(w + TSFP_MENUWINDOW_VIEW_Y0);

    if (!st || !(GUEST_F32(w + TSFP_MENUWINDOW_CONTENT_HEIGHT) > view))
        return 0;
    o = GUEST_U32(st + 0x70) ? 0xECu : 0xE4u;
    r[0] = GUEST_F32(st + 0x74) * GUEST_F32(0x007BA038u) + GUEST_F32(w + o);
    r[2] = GUEST_F32(st + 0x78) * GUEST_F32(0x007BA038u) + GUEST_F32(w + o);
    r[1] = GUEST_F32(w + TSFP_MENUWINDOW_VIEW_Y0) * k + GUEST_F32(w + 0xACu);
    r[3] = GUEST_F32(w + TSFP_MENUWINDOW_VIEW_Y1) * k + GUEST_F32(w + 0xACu);
    *arrow = (r[2] - r[0]) * GUEST_F32(0x0047839Cu);
    return r[2] > r[0] && r[3] > r[1];
}

/* The arrows are drawn left of and (the lower one) above the strip the
 * cursor update tests for them, overlapping it by a unit or two, so the
 * pointer would have to find a sliver. Over a drawn arrow (from the right
 * end of the rows to the bar, a little more than an arrow's height from
 * either end) the game's cursor is put in the middle of the arrow's box. */
static void tsfp_menu_snap_arrow(uint32_t w, float *sx, float *sy)
{
    float r[4], h;

    if (!tsfp_menu_bar(w, r, &h) || *sx < r[0] - 1.1f * (r[2] - r[0]) || *sx >= r[2] + 4.0f)
        return;
    if (*sy >= r[1] - 4.0f && *sy < r[1] + 1.5f * h) {
        *sx = (r[0] + r[2]) * 0.5f;
        *sy = r[1] + h * 0.5f;
    } else if (*sy >= r[3] - 2.0f * h && *sy < r[3] + 4.0f) {
        *sx = (r[0] + r[2]) * 0.5f;
        *sy = r[3] - h * 0.5f;
    }
}

/* Player 1's menu window w, and the context it is in (or 32). */
static uint32_t tsfp_menu_ctx(uint32_t w)
{
    uint32_t ctx = GUEST_U32(w + TSFP_MENUWINDOW_CONTEXT);

    if (!(GUEST_U32(w + TSFP_MENUWINDOW_PLAYERS) & 1u) || ctx >= 32 ||
        (GUEST_U32(w + TSFP_MENUWINDOW_FLAGS) & 0x40000u))
        return 32;                      /* not player 1's, or keeps the pad's navigation */
    return ctx;
}

static void tsfp_menu_cursor_off(uint32_t w, uint32_t ctx)
{
    uint32_t c = TSFP_G_MENU_CONTEXTS + ctx * TSFP_MENUCONTEXT_SIZE, keep = s_menu_focus;

    s_menu_cursor = 0;
    if (s_cursor_ours & (1u << ctx)) {
        GUEST_U32(c + TSFP_MENUCONTEXT_CURSOR_MODE) = 0;
        s_cursor_ours &= ~(1u << ctx);
    }
    if (!GUEST_U32(w + TSFP_MENUWINDOW_FOCUS) && keep && tsfp_menu_has_item(w, keep)) {
        uint32_t a[2] = { w, keep };
        tsfp_guest_call(sub_00076EF0, 2, a);
    }
}

/* Before the menu reads its pads: a press or a pushed stick gives the menu
 * back to the pad now, not after this frame's navigation was the cursor's. */
static void tsfp_menu_mouse_pre(uint32_t w)
{
    uint32_t ctx = tsfp_menu_ctx(w);

    s_menu_pad_frame = 0;
    if (ctx < 32 && s_menu_cursor && tsfp_pad_used(0)) {
        tsfp_menu_cursor_off(w, ctx);
        s_menu_pad_frame = 1;
    }
}

static void tsfp_menu_mouse(uint32_t w)
{
    uint32_t ctx = GUEST_U32(w + TSFP_MENUWINDOW_CONTEXT), c, bit, it = 0;
    long clicks, held = 0;
    int wheel = 0, moved, over;
    float px, py, sx, sy, x0, y0, scale_x, scale_y;

    if (!(GUEST_U32(w + TSFP_MENUWINDOW_PLAYERS) & 1u) || ctx >= 32)
        return;                         /* not player 1's */
    if (s_menu_trace < 0) {
        const char *e = getenv("RECOMP_MENU_TRACE");
        s_menu_trace = e ? atoi(e) : 0;
    }
    s_menu_seen = tsfp_now();
    if (!recomp_mouse_pointer_mode()) {
        recomp_mouse_set_pointer_mode(1);
        if (s_menu_trace)
            fprintf(stderr, "[MENU] pointer mode on (window 0x%08X)\n", w);
    }
    if (GUEST_U32(w + TSFP_MENUWINDOW_FLAGS) & 0x40000u)
        return;                         /* keeps the pad's navigation */

    c = TSFP_G_MENU_CONTEXTS + ctx * TSFP_MENUCONTEXT_SIZE;
    bit = 1u << ctx;
    moved = recomp_mouse_pointer_take_moves() != 0;
    clicks = recomp_mouse_pointer_take_clicks(&held, &wheel) | s_menu_click_later;
    /* A click in the frame the pointer moved waits a frame: what is under
     * the pointer (a row, or the scroll bar over it) is the game's cursor
     * update's to find first. */
    s_menu_click_later = moved ? clicks & (1L << RECOMP_MOUSE_LEFT) : 0;
    clicks &= ~s_menu_click_later;
    if (tsfp_pad_used(0))
        s_menu_cursor = 0;
    else if ((moved || clicks || wheel) && !s_menu_pad_frame)
        s_menu_cursor = 1;
    if (!s_menu_cursor) {
        tsfp_menu_cursor_off(w, ctx);
        return;
    }
    if (GUEST_U32(w + TSFP_MENUWINDOW_FOCUS))
        s_menu_focus = GUEST_U32(w + TSFP_MENUWINDOW_FOCUS);
    if (!GUEST_U32(c + TSFP_MENUCONTEXT_CURSOR_MODE)) {
        GUEST_U32(c + TSFP_MENUCONTEXT_CURSOR_MODE) = 1;
        s_cursor_ours |= bit;
    }

    over = recomp_mouse_pointer(&px, &py);
    sx = px * TSFP_SCREEN_W;
    sy = py * TSFP_SCREEN_H;
    tsfp_menu_snap_arrow(w, &sx, &sy);
    /* menu_cursor_place's mapping, backwards: screen = x0 + n * scale. */
    x0 = GUEST_F32(c + TSFP_MENUCONTEXT_X0);
    y0 = GUEST_F32(c + TSFP_MENUCONTEXT_Y0);
    scale_x = (GUEST_F32(c + TSFP_MENUCONTEXT_X1) - x0) * GUEST_F32(0x00475D1Cu);
    scale_y = (GUEST_F32(c + TSFP_MENUCONTEXT_Y1) - y0) * GUEST_F32(0x00475D1Cu);
    GUEST_F32(c + TSFP_MENUCONTEXT_CURSOR_SX) = sx;
    GUEST_F32(c + TSFP_MENUCONTEXT_CURSOR_SY) = sy;
    if (scale_x != 0.0f && scale_y != 0.0f) {
        GUEST_F32(c + TSFP_MENUCONTEXT_CURSOR_X) = (sx - x0) / scale_x;
        GUEST_F32(c + TSFP_MENUCONTEXT_CURSOR_Y) = (sy - y0) / scale_y;
    }

    if (clicks && s_menu_trace > 1) {    /* RECOMP_MENU_TRACE=2: the window's items */
        uint32_t i = GUEST_U32(w + TSFP_MENUWINDOW_ITEMS);
        int n;
        for (n = 0; i && n < 64; i = GUEST_U32(i + TSFP_MENUITEM_NEXT), n++)
            fprintf(stderr, "[MENU]   item 0x%08X kind %u accept 0x%X b8 0x%X flags 0x%X rect %.1f,%.1f-%.1f,%.1f choice %u/%u\n",
                    i, GUEST_U32(i + TSFP_MENUITEM_KIND), GUEST_U32(i + TSFP_MENUITEM_ON_ACCEPT), GUEST_U32(i + 0xB8), GUEST_U32(i + TSFP_MENUITEM_FLAGS),
                    GUEST_F32(i + TSFP_MENUITEM_SX0), GUEST_F32(i + TSFP_MENUITEM_SY0),
                    GUEST_F32(i + TSFP_MENUITEM_SX1), GUEST_F32(i + TSFP_MENUITEM_SY1),
                    GUEST_U32(i + TSFP_MENUITEM_CHOICE), GUEST_U32(i + TSFP_MENUITEM_CHOICE_COUNT));
    }
    if (held & (1L << RECOMP_MOUSE_LEFT))
        GUEST_U32(w + TSFP_MENUWINDOW_HELD) |= TSFP_MENU_A;
    if (clicks & (1L << RECOMP_MOUSE_LEFT)) {
        /* The scroll bar's arrows lie over the rows' rectangles: a click
         * the cursor update put on a part of the bar (focus_id -2..-5) is
         * the bar's, held A scrolls, and no row is picked. */
        int bar = (int)GUEST_U32(w + TSFP_MENUWINDOW_FOCUS_ID) <= -2;
        it = over && !bar ? tsfp_menu_item_at(w, sx, sy) : 0;
        if (bar)
            ;
        else if (it && GUEST_U32(it + TSFP_MENUITEM_KIND) == 1)
            tsfp_menu_step_value(it, sx);
        else if (it)
            GUEST_U32(w + TSFP_MENUWINDOW_PRESSED) |= TSFP_MENU_A;
        else if (over && !tsfp_menu_has_item(w, 0)) {
            tsfp_menu_cursor_off(w, ctx);
            GUEST_U32(w + TSFP_MENUWINDOW_PRESSED) |= TSFP_MENU_A;
        }
    }
    if (clicks & (1L << RECOMP_MOUSE_RIGHT))
        GUEST_U32(w + TSFP_MENUWINDOW_PRESSED) |= TSFP_MENU_B;
    /* The wheel in a list longer than its window scrolls it by the arrows'
     * step (up is towards the top), from wherever it is easing to. */
    if (wheel && over && GUEST_F32(w + TSFP_MENUWINDOW_CONTENT_HEIGHT) >
            GUEST_F32(w + TSFP_MENUWINDOW_VIEW_Y1) - GUEST_F32(w + TSFP_MENUWINDOW_VIEW_Y0)) {
        float most = GUEST_F32(w + TSFP_MENUWINDOW_CONTENT_HEIGHT) -
                     (GUEST_F32(w + TSFP_MENUWINDOW_VIEW_Y1) - GUEST_F32(w + TSFP_MENUWINDOW_VIEW_Y0));
        float to = GUEST_F32(w + TSFP_MENUWINDOW_SCROLL_TARGET) - (float)wheel * tsfp_menu_row(w);
        GUEST_F32(w + TSFP_MENUWINDOW_SCROLL_TARGET) = to < 0.0f ? 0.0f : to > most ? most : to;
    }
    /* The wheel over a row: a value row steps its value (up forward), any
     * other gets left/right as from the pad, which is how sliders move. */
    else if (wheel && over && (it = tsfp_menu_item_at(w, sx, sy)) != 0) {
        if (GUEST_U32(it + TSFP_MENUITEM_KIND) == 1)
            tsfp_guest_call(wheel > 0 ? sub_000755D0 : sub_000756A0, 1, &it);
        else
            GUEST_U32(w + TSFP_MENUWINDOW_PRESSED) |= wheel > 0 ? TSFP_MENU_RIGHT : TSFP_MENU_LEFT;
    }
    if (s_menu_trace && (moved || clicks || wheel))
        fprintf(stderr, "[MENU] window 0x%08X ctx %u rect %.1f,%.1f-%.1f,%.1f bounds %.1f,%.1f-%.1f,%.1f "
                "pointer %.3f,%.3f%s -> %.1f,%.1f clicks 0x%lX wheel %d scroll %.1f->%.1f part %d "
                "item 0x%08X kind %d focus 0x%08X\n",
                w, ctx, x0, y0, GUEST_F32(c + TSFP_MENUCONTEXT_X1), GUEST_F32(c + TSFP_MENUCONTEXT_Y1),
                GUEST_F32(c + TSFP_MENUCONTEXT_CURSOR_MIN_X), GUEST_F32(c + TSFP_MENUCONTEXT_CURSOR_MIN_Y),
                GUEST_F32(c + TSFP_MENUCONTEXT_CURSOR_MAX_X), GUEST_F32(c + TSFP_MENUCONTEXT_CURSOR_MAX_Y),
                px, py, over ? "" : " (off)", sx, sy, clicks, wheel,
                GUEST_F32(w + TSFP_MENUWINDOW_SCROLL), GUEST_F32(w + TSFP_MENUWINDOW_SCROLL_TARGET),
                (int)GUEST_U32(w + TSFP_MENUWINDOW_FOCUS_ID), it,
                it ? (int)GUEST_U32(it + TSFP_MENUITEM_KIND) : -1,
                GUEST_U32(w + TSFP_MENUWINDOW_FOCUS));
}

void sub_00073870(void)
{
    uint32_t w = GUEST_U32(g_esp + 4);  /* (MenuWindow *) */

    if (w)
        tsfp_menu_mouse_pre(w);
    sub_00073870_gen();
    if (w)
        tsfp_menu_mouse(w);
}

/* ── PC settings: a screen of our own in FP's menus ───────────
 *
 * The settings that live in the title's config file (resolution, widescreen,
 * antialiasing, texture filtering, frame cap, FPS counter) as a menu screen,
 * reached from a "PC Settings" entry added below Controls in the front end's
 * Settings and in the pause menu. Everything is built with the game's own
 * calls, as menu_build_controls builds Controls: a screen is a descriptor
 * {build, update, 0, 0} opened with menu_open (0x79B70) on the root window,
 * whose build adds the rows; Accept and Cancel close it with menu_back
 * (0x79C00) as Controls' Cancel does. The descriptor's functions and the
 * items' handlers are code addresses only recomp_lookup_manual knows: the
 * game reaches them through indirect calls, which ask it first.
 *
 * The display is set up once at start-up, so Accept writes the config file
 * and the new settings apply the next time the game starts (an environment
 * variable still wins over the file, as everywhere). */
extern void sub_00079D60_gen(void);     /* menu_add_item */
extern void sub_0003D7B0(void);         /* lang_string */
extern void sub_00075B70(void);         /* window header: flags, title, width, 0, draw cb */
extern void sub_00071C50(void);
extern void sub_00071B10(void);
extern void sub_0007D480(void);         /* menu_make_option */
extern void sub_000717D0(void);         /* menu_add_choice */
extern void sub_00071830(void);         /* menu_add_off_on */
extern void sub_00079710(void);
extern void sub_000795F0(void);
extern void sub_000771D0(void);
extern void sub_00077140(void);
extern void sub_00077110(void);
extern void sub_002E45A0(void);
extern void sub_00074A00(void);         /* help line: index, text, 0, 0 */
extern void sub_00079B70(void);         /* menu_open(root, descriptor) */
extern void sub_00079C00(void);         /* menu_back(root) */
extern uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment);

/* Code addresses for the game to call: odd bytes inside the Controls
 * builder, which nothing calls or jumps to. */
#define TSFP_PC_OPEN    0x002EA8E1u     /* the "PC Settings" item */
#define TSFP_PC_BUILD   0x002EA8E2u
#define TSFP_PC_UPDATE  0x002EA8E3u
#define TSFP_PC_ACCEPT  0x002EA8E4u
#define TSFP_PC_CANCEL  0x002EA8E5u

#define TSFP_PC_ROW_FLAGS 0x400u        /* Controls' rows on a one-player screen */
#define TSFP_PC_ACCEPT_ID 10
#define TSFP_PC_CANCEL_ID 11

/* One row: a value list whose choices map to config values. */
typedef struct { const char *label; int count; const char *names[6]; int values[6]; } TsfpPcRow;
enum { PCROW_RES, PCROW_WIDE, PCROW_MSAA, PCROW_ANISO, PCROW_CAP, PCROW_FPS, PCROW_ROWS };
static const TsfpPcRow s_pc_rows[PCROW_ROWS] = {
    { "Resolution",        5, { "640x480", "720p", "1080p", "1440p", "4K" }, { 1, 720, 1080, 1440, 2160 } },
    { "Widescreen",        2, { 0 }, { 0, 1 } },                         /* the game's Off/On */
    { "Antialiasing",      4, { "Off", "2x", "4x", "8x" }, { 1, 2, 4, 8 } },
    { "Texture filtering", 5, { "Bilinear", "2x", "4x", "8x", "16x" }, { 1, 2, 4, 8, 16 } },
    { "Frame cap",         4, { "Adaptive", "60", "30", "Off" }, { 0, 60, 30, -1 } },
    { "FPS counter",       2, { 0 }, { 0, 1 } },
};

static uint32_t s_pc_desc;              /* the descriptor, in guest memory */
static uint32_t s_pc_window;            /* the screen while it is up */
static RecompSettings s_pc_settings;    /* what the file held when it opened */
static char s_pc_path[1024];

/* A string the game can read; the same text always gets the same copy. */
static uint32_t tsfp_guest_str(const char *s)
{
    static struct { const char *s; uint32_t va; } made[64];
    static int n;
    uint32_t len = (uint32_t)strlen(s) + 1, va;
    int i;

    for (i = 0; i < n; i++)
        if (made[i].s == s)
            return made[i].va;
    va = xbox_HeapAlloc(len, 4);
    if (!va)
        return 0;
    memcpy((uint8_t *)g_xbox_mem_offset + va, s, len);
    if (n < 64) {
        made[n].s = s;
        made[n].va = va;
        n++;
    }
    return va;
}

static uint32_t tsfp_lang(uint32_t id)
{
    return tsfp_guest_call(sub_0003D7B0, 1, &id);
}

static uint32_t tsfp_add_item(uint32_t text, int id, uint32_t flags, uint32_t on_accept)
{
    uint32_t a[6] = { text, (uint32_t)id, flags, 0, on_accept, 0 };
    return tsfp_guest_call(sub_00079D60_gen, 6, a);
}

static int tsfp_in_game(void)
{
    return (int32_t)GUEST_U32(TSFP_G_MENU_MODE) > 100;   /* the pause menu, not the front end */
}

/* The root window our screen opens on and goes back to: the one the
 * Controls entry beside ours uses. The Settings screen's (0x002CBE50) opens
 * on 0x7613E4, the pause menu's (0x002EC1A0) and 0x002E7B70 on the root of
 * the window's context. */
static uint32_t s_pc_root;

static uint32_t tsfp_menu_root(uint32_t w)
{
    uint32_t it = GUEST_U32(w + TSFP_MENUWINDOW_ITEMS);
    int n;

    for (n = 0; it && n < 64; it = GUEST_U32(it + TSFP_MENUITEM_NEXT), n++)
        if (GUEST_U32(it + TSFP_MENUITEM_ON_ACCEPT) == 0x002CBE50u)
            return GUEST_U32(TSFP_G_MENU_SETTINGS_ROOT);
    return GUEST_U32(TSFP_G_MENU_ROOTS + 4u * GUEST_U32(w + TSFP_MENUWINDOW_CONTEXT));
}

/* The settings file's current value as one of the row's choices. */
static int tsfp_pc_choice(int row)
{
    const RecompSettings *s = &s_pc_settings;
    int v, i;

    switch (row) {
    case PCROW_RES:   v = s->resolution_scale; break;
    case PCROW_WIDE:  v = s->widescreen != 0; break;
    case PCROW_MSAA:  v = s->msaa; break;
    case PCROW_ANISO: v = s->anisotropy; break;
    case PCROW_CAP:
        v = !strcmp(s->frame_cap, "adaptive") ? 0 : atoi(s->frame_cap);
        if (v == 0 && strcmp(s->frame_cap, "adaptive"))
            v = -1;
        break;
    default:       v = s->fps_overlay != 0; break;
    }
    for (i = 0; i < s_pc_rows[row].count; i++)
        if (s_pc_rows[row].values[i] == v)
            return i;
    if (row == PCROW_RES && v >= 2 && v <= 8)      /* a multiple of 480 lines */
        v *= 480;
    for (i = s_pc_rows[row].count - 1; i > 0; i--)   /* the nearest below */
        if (s_pc_rows[row].values[i] <= v)
            return i;
    return 0;
}

static void tsfp_pc_build(void)
{
    uint32_t w = GUEST_U32(g_esp + 4), a[5], it;
    int r, i, game = tsfp_in_game();

    s_pc_window = w;
    if (!recomp_config_path() || snprintf(s_pc_path, sizeof s_pc_path, "%s", recomp_config_path()) <= 0)
        if (!recomp_settings_path(0x45410066u, s_pc_path, sizeof s_pc_path))
            s_pc_path[0] = 0;
    recomp_settings_read(s_pc_path, &s_pc_settings);

    /* As the Controls builder does for one player (0x002EA8E0). */
    a[0] = 0x2000;
    a[1] = tsfp_guest_str("PC Settings");
    a[2] = GUEST_U32(GUEST_U32(0x004C0454u) != 1 ? 0x0047D1F4u : 0x0049F4A0u);
    a[3] = 0;
    a[4] = 0;
    tsfp_guest_call(sub_00075B70, 5, a);
    a[0] = 0x2000;
    tsfp_guest_call(sub_00071C50, 1, a);
    a[0] = 1; a[1] = 0x3F800000u; a[2] = 1; a[3] = 1;
    tsfp_guest_call(sub_00071B10, 4, a);

    for (r = 0; r < PCROW_ROWS; r++) {
        const TsfpPcRow *row = &s_pc_rows[r];
        it = tsfp_add_item(tsfp_guest_str(row->label), r, TSFP_PC_ROW_FLAGS | 0x20000u, 0);
        GUEST_U32(it + 0x1C) = 3;
        a[0] = TSFP_PC_ROW_FLAGS; a[1] = 0;
        tsfp_guest_call(sub_0007D480, 2, a);
        if (!row->names[0])
            tsfp_guest_call(sub_00071830, 0, a);
        else
            for (i = 0; i < row->count; i++) {
                a[0] = tsfp_guest_str(row->names[i]);
                tsfp_guest_call(sub_000717D0, 1, a);
            }
        GUEST_U32(it + 0xBC) = 3;
        GUEST_U32(it + TSFP_MENUITEM_CHOICE) = (uint32_t)tsfp_pc_choice(r);
    }
    /* A row that cannot be selected (flag 4, as the separators have). */
    tsfp_add_item(tsfp_guest_str("Applies after a restart"), -1, TSFP_PC_ROW_FLAGS | 4u, 0);
    a[0] = 1;
    tsfp_guest_call(sub_00079710, 1, a);
    if (game)
        tsfp_guest_call(sub_000795F0, 0, a);
    tsfp_add_item(tsfp_lang(0x1B08), TSFP_PC_ACCEPT_ID, TSFP_PC_ROW_FLAGS | 0x10000u, TSFP_PC_ACCEPT);
    tsfp_add_item(tsfp_lang(0x1B0B), TSFP_PC_CANCEL_ID, TSFP_PC_ROW_FLAGS | 0x10000u, TSFP_PC_CANCEL);
    a[0] = w;
    if (game) {
        a[0] = GUEST_U32(TSFP_G_MENU_BUILDING);
        tsfp_guest_call(sub_000771D0, 1, a);
    } else {
        tsfp_guest_call(sub_002E45A0, 1, a);
        tsfp_guest_call(sub_00077140, 1, a);
    }
    a[0] = 0; a[1] = tsfp_lang(0x1A26); a[2] = 0; a[3] = 0;
    tsfp_guest_call(sub_00074A00, 4, a);
    a[0] = 1; a[1] = tsfp_lang(0x1A27);
    tsfp_guest_call(sub_00074A00, 4, a);
    GUEST_U32(w + 0x210) = GUEST_U32(0x00478674u);
    a[0] = w; a[1] = PCROW_RES;            /* focus the first row (0x00077110 finds it by id) */
    tsfp_guest_call(sub_00077110, 2, a);
    g_esp += 4;
}

static void tsfp_pc_update(void)
{
    g_esp += 4;
}

static void tsfp_pc_close(void)
{
    uint32_t root = s_pc_root;

    s_pc_window = 0;
    tsfp_guest_call(sub_00079C00, 1, &root);
}

static void tsfp_pc_accept(void)
{
    RecompSettings s = s_pc_settings;
    uint32_t it;
    int n, chosen[PCROW_ROWS], r;

    for (r = 0; r < PCROW_ROWS; r++)
        chosen[r] = tsfp_pc_choice(r);
    it = s_pc_window ? GUEST_U32(s_pc_window + TSFP_MENUWINDOW_ITEMS) : 0;
    for (n = 0; it && n < 64; it = GUEST_U32(it + TSFP_MENUITEM_NEXT), n++) {
        uint32_t id = GUEST_U32(it + 0x14), c = GUEST_U32(it + TSFP_MENUITEM_CHOICE);
        if (id < PCROW_ROWS && c < (uint32_t)s_pc_rows[id].count)
            chosen[id] = (int)c;
    }
    /* A resolution the list cannot show is kept unless it was changed. */
    if (chosen[PCROW_RES] != tsfp_pc_choice(PCROW_RES))
        s.resolution_scale = s_pc_rows[PCROW_RES].values[chosen[PCROW_RES]];
    s.widescreen  = s_pc_rows[PCROW_WIDE].values[chosen[PCROW_WIDE]];
    s.msaa        = s_pc_rows[PCROW_MSAA].values[chosen[PCROW_MSAA]];
    s.anisotropy  = s_pc_rows[PCROW_ANISO].values[chosen[PCROW_ANISO]];
    r = s_pc_rows[PCROW_CAP].values[chosen[PCROW_CAP]];
    if (r == 0)
        snprintf(s.frame_cap, sizeof s.frame_cap, "adaptive");
    else
        snprintf(s.frame_cap, sizeof s.frame_cap, "%d", r < 0 ? 0 : r);
    s.fps_overlay = s_pc_rows[PCROW_FPS].values[chosen[PCROW_FPS]];
    if (memcmp(&s, &s_pc_settings, sizeof s)) {
        int ok = s_pc_path[0] && recomp_settings_write(s_pc_path, &s);
        fprintf(stderr, "[MENU] PC settings %s %s: resolution %d widescreen %d msaa %d anisotropy %d "
                "frame cap %s fps %d (from the next start)\n", ok ? "written to" : "NOT written to",
                s_pc_path, s.resolution_scale, s.widescreen, s.msaa, s.anisotropy, s.frame_cap, s.fps_overlay);
    }
    tsfp_pc_close();
    g_esp += 4;
}

static void tsfp_pc_cancel(void)
{
    tsfp_pc_close();
    g_esp += 4;
}

static void tsfp_pc_open(void)
{
    uint32_t w = GUEST_U32(g_esp + 4), a[2];    /* (window), as Controls' handlers take */

    if (!s_pc_desc && (s_pc_desc = xbox_HeapAlloc(16, 4)) != 0) {
        GUEST_U32(s_pc_desc + 0) = TSFP_PC_BUILD;
        GUEST_U32(s_pc_desc + 4) = TSFP_PC_UPDATE;
        GUEST_U32(s_pc_desc + 8) = 0;
        GUEST_U32(s_pc_desc + 12) = 0;
    }
    if (s_pc_desc) {
        a[0] = s_pc_root = tsfp_menu_root(w);
        a[1] = s_pc_desc;
        tsfp_guest_call(sub_00079B70, 2, a);
    }
    g_esp += 4;
}

/* menu_add_item: after the game adds its Controls entry (the Settings
 * screen's, 0x002CBE50; the pause menu's, 0x002EC1A0; 0x002E7D50's,
 * 0x002E7B70), add ours with the same flags. Ours becomes
 * g_menu_last_item, which the next menu_add_item lays out; every one of
 * these builders adds a plain item next. */
void sub_00079D60(void)
{
    uint32_t on_accept = GUEST_U32(g_esp + 0x14), flags = GUEST_U32(g_esp + 0xC);
    uint32_t item;

    sub_00079D60_gen();
    if (on_accept != 0x002E7B70u && on_accept != 0x002CBE50u && on_accept != 0x002EC1A0u)
        return;
    item = g_eax;
    tsfp_add_item(tsfp_guest_str("PC Settings"), on_accept == 0x002E7B70u ? 2 : -1, flags, TSFP_PC_OPEN);
    g_eax = item;
}

static recomp_func_t tsfp_pc_lookup(uint32_t va)
{
    switch (va) {
    case TSFP_PC_OPEN:   return tsfp_pc_open;
    case TSFP_PC_BUILD:  return tsfp_pc_build;
    case TSFP_PC_UPDATE: return tsfp_pc_update;
    case TSFP_PC_ACCEPT: return tsfp_pc_accept;
    case TSFP_PC_CANCEL: return tsfp_pc_cancel;
    }
    return (recomp_func_t)0;
}

/* Non-zero when this frame's mouse movement is dropped: whenever the stick
 * would not turn the view either. Read before the original body runs; it
 * changes none of these. */
static int tsfp_mouse_blocked(uint32_t p)
{
    uint32_t state = GUEST_U32(p + TSFP_PLAYER_MOVE_STATE), cam, mode;

    if (GUEST_U32(TSFP_G_PADS_FROZEN))
        return 1;                               /* pad_axis reads 0 */
    if (s_pads_neutral & (1u << GUEST_U32(p + TSFP_PLAYER_PAD_INDEX)))
        return 2;
    /* player_apply_look skips 0x20 and 0x100; player_read_look_move reads no
     * look at all in 0x100, 0x2000 and 0x20000. */
    if (state == 0x20 || (state & 0x22100u))
        return 3;
    cam = GUEST_U32(TSFP_G_CAMERA);
    mode = cam ? GUEST_U32(cam + TSFP_CAMERA_MODE) : 0;
    if (!(GUEST_S8(p + TSFP_PLAYER_FLAGS) < 0 ||
          (mode && GUEST_U32(mode) == 0x18 && state != 0x400 && state != 0x200)))
        return 4;                               /* no control */
    return 0;
}

/* Auto-lookahead. Walking forward with no look-y input puts look_mode at 2,
 * and in 2 the original body pulls the pitch towards level by 5% a vblank
 * (when the profile's controls+2 has bit 3) until it is within half a
 * degree, ignoring the stick's pitch; look-y input moves it to 1, which holds
 * until the player stops. The mouse never counted as look-y input, so with
 * the option on the game kept levelling the view the hand was aiming: the
 * pull fought every vertical movement for as long as the walk lasted.
 * Vertical mouse movement now does what the stick's does, before the body
 * runs its state machine: 2 -> 1, and 0 -> 1 when moving (move.x, the body's
 * own test, non-zero). */
static void tsfp_mouse_cancel_lookahead(uint32_t p, uint32_t move)
{
    uint32_t m = GUEST_U32(p + TSFP_PLAYER_LOOK_MODE);

    if (m == 2 || (m == 0 && GUEST_F32(move) != 0.0f))
        GUEST_U32(p + TSFP_PLAYER_LOOK_MODE) = 1;
}

static void tsfp_mouse_look(uint32_t p, long dx, long dy)
{
    uint32_t cam = GUEST_U32(TSFP_G_CAMERA), state = GUEST_U32(p + TSFP_PLAYER_MOVE_STATE);
    float scale = 1.0f, yaw, pitch, limit;

    if (cam) {
        float def = (GUEST_U32(cam + TSFP_CAMERA_MODE_A) == 0 && GUEST_U32(cam + TSFP_CAMERA_MODE_B) == 1)
                    ? GUEST_F32(TSFP_G_FOV_DEFAULT_ALT) : GUEST_F32(TSFP_G_FOV_DEFAULT);
        float fov = GUEST_F32(cam + TSFP_CAMERA_FOV);
        if (def > 1.0f && fov > 1.0f && fov <= def)
            scale = fov / def;
    }

    if (state == 0x40 || state == 0x80) {
        uint32_t ch = GUEST_U32(p + TSFP_PLAYER_CHARACTER);
        uint32_t cd = ch ? GUEST_U32(ch + TSFP_OBJECT_DATA) : 0;
        uint32_t mount = cd ? GUEST_U32(cd + TSFP_CHARDATA_MOUNT) : 0;
        if (mount)
            tsfp_mouse_look_mounted(state, mount,
                -(float)recomp_mouse_counts_to_degrees(dx) * scale,
                (float)recomp_mouse_counts_to_degrees(dy) * scale);
        return;
    }

    yaw = GUEST_F32(p + TSFP_PLAYER_YAW) - (float)recomp_mouse_counts_to_degrees(dx) * scale;
    while (yaw < 0.0f)
        yaw += 360.0f;
    while (yaw >= 360.0f)
        yaw -= 360.0f;
    limit = GUEST_F32(TSFP_G_PITCH_LIMIT);
    pitch = GUEST_F32(p + TSFP_PLAYER_PITCH) - (float)recomp_mouse_counts_to_degrees(dy) * scale;
    if (pitch > limit)
        pitch = limit;
    if (pitch < -limit)
        pitch = -limit;
    GUEST_F32(p + TSFP_PLAYER_YAW) = yaw;
    GUEST_F32(p + TSFP_PLAYER_PITCH) = pitch;
}

/* "Crosshair: On and moving". aim_update (0x0025A7F0), after the look,
 * eases the crosshair (and the gun's aim) towards the look stick's raw
 * deflection * -0.33, so the crosshair leads a turn by up to a third of the
 * way to the screen's edge and drifts back when the turn stops. The camera's
 * own turn does not wait for it. The mouse deflects no stick, so with the
 * mouse the crosshair stayed put whatever the option said. Each frame the
 * mouse turned the view, aim_update is handed, on top of the stick, the
 * deflection that would have turned it as far: the stick turns
 * aim_turn_scale * g_yaw_speed / ticks_per_second * ticks * (turn_speed +
 * 0.5) degrees at full deflection past the 0.1 deadzone (pitch: g_pitch_speed
 * and half the aiming slow-down), the same terms player_apply_look uses. */
static uint32_t s_aim_player;
static float s_aim_look[2];

static float tsfp_stick_for(float degrees, float full)
{
    float d;

    if (full <= 0.0f || degrees == 0.0f)
        return 0.0f;
    d = degrees / full;
    if (d > 1.0f)
        d = 1.0f;
    if (d < -1.0f)
        d = -1.0f;
    return d > 0.0f ? 0.1f + 0.9f * d : -0.1f + 0.9f * d;   /* back through the deadzone */
}

static void tsfp_mouse_aim_lead(uint32_t p, long dx, long dy)
{
    uint32_t controls = GUEST_U32(p + TSFP_PLAYER_CONTROLS);
    float ticks = GUEST_F32(TSFP_G_TICKS_PER_FRAME);
    float tps = (float)(int32_t)GUEST_U32(TSFP_G_TICKS_PER_SECOND);
    float aim = GUEST_F32(p + TSFP_PLAYER_AIM_TURN_SCALE);
    float turn = controls ? GUEST_F32(controls + TSFP_CONTROLS_TURN_SPEED) + 0.5f : 1.0f;
    float per = tps > 0.0f ? ticks * turn / tps : 0.0f;

    /* yaw grows to the left and pitch upwards (tsfp_mouse_look); the stick's
     * x and look-y push them the same way, so a right turn is negative x */
    s_aim_look[0] = tsfp_stick_for(-(float)recomp_mouse_counts_to_degrees(dx),
                                   aim * GUEST_F32(TSFP_G_YAW_SPEED) * per);
    s_aim_look[1] = tsfp_stick_for(-(float)recomp_mouse_counts_to_degrees(dy),
                                   (1.0f - (1.0f - aim) * 0.5f) * GUEST_F32(TSFP_G_PITCH_SPEED) * per);
    if (controls && (GUEST_U8(controls + TSFP_CONTROLS_FLAGS) & 1))
        s_aim_look[1] = -s_aim_look[1];         /* the raw stick, before invert look */
    s_aim_player = p;
}

extern void sub_0025A7F0_gen(void);

void sub_0025A7F0(void)
{
    uint32_t look = GUEST_U32(g_esp + 4);       /* (float *look) */
    float x, y, ax, ay;

    if (!s_aim_player || s_aim_player != GUEST_U32(TSFP_G_PLAYER) || !look) {
        sub_0025A7F0_gen();
        return;
    }
    s_aim_player = 0;
    x = GUEST_F32(look);
    y = GUEST_F32(look + 4);
    ax = x + s_aim_look[0];
    ay = y + s_aim_look[1];
    GUEST_F32(look) = ax > 1.0f ? 1.0f : ax < -1.0f ? -1.0f : ax;
    GUEST_F32(look + 4) = ay > 1.0f ? 1.0f : ay < -1.0f ? -1.0f : ay;
    sub_0025A7F0_gen();
    GUEST_F32(look) = x;                        /* player_update's own look vector */
    GUEST_F32(look + 4) = y;
}

/* RECOMP_LOOK_TRACE=1: a line per player 1 frame -- the mouse counts, why
 * they were dropped (blocked: 1 pads frozen, 2 pad neutralised, 3 move state,
 * 4 no control), look_mode before and after the body, the profile's controls
 * byte (bit 3: lookahead), and yaw/pitch at entry, after the body and after
 * the mouse. How the walk-and-turn stutter and the level 2 pan were found. */
static int s_look_trace = -1;

void sub_0025D520(void)
{
    uint32_t p = GUEST_U32(TSFP_G_PLAYER);
    uint32_t move = GUEST_U32(g_esp + 8);       /* (look, move, look_out) */
    long dx = 0, dy = 0;
    int mine, blocked = 0;
    uint32_t mode0 = 0;
    float yaw0 = 0, pitch0 = 0, yaw1 = 0, pitch1 = 0;

    if (s_look_trace < 0) {
        const char *e = getenv("RECOMP_LOOK_TRACE");
        s_look_trace = e && *e && *e != '0';
    }
    /* The mouse is player 1's. Taken only in player 1's update, so another
     * local player's does not swallow what came in between; taken every
     * frame it runs, used or not, so movement while the view is not the
     * player's is dropped rather than saved up. */
    mine = p && GUEST_U32(p + TSFP_PLAYER_PAD_INDEX) == 0;
    if (mine) {
        recomp_mouse_take_motion(&dx, &dy);
        blocked = tsfp_mouse_blocked(p);
        if (s_look_trace) {
            mode0 = GUEST_U32(p + TSFP_PLAYER_LOOK_MODE);
            yaw0 = GUEST_F32(p + TSFP_PLAYER_YAW);
            pitch0 = GUEST_F32(p + TSFP_PLAYER_PITCH);
        }
        if (!blocked && dy)
            tsfp_mouse_cancel_lookahead(p, move);
    }
    sub_0025D520_gen();
    if (!mine)
        return;
    if (s_look_trace) {
        yaw1 = GUEST_F32(p + TSFP_PLAYER_YAW);
        pitch1 = GUEST_F32(p + TSFP_PLAYER_PITCH);
    }
    if (!blocked && (dx || dy)) {
        uint32_t state = GUEST_U32(p + TSFP_PLAYER_MOVE_STATE);
        tsfp_mouse_look(p, dx, dy);
        if (state != 0x40 && state != 0x80)     /* mounted: the object turns */
            tsfp_mouse_aim_lead(p, dx, dy);
    }
    if (s_look_trace) {
        uint32_t controls = GUEST_U32(p + TSFP_PLAYER_CONTROLS);
        fprintf(stderr, "[LOOK] clock %u state 0x%X flags 0x%02X mouse %ld,%ld blocked %d "
                "mode %u>%u controls 0x%02X move %.2f yaw %.3f>%.3f>%.3f pitch %.3f>%.3f>%.3f\n",
                GUEST_U32(TSFP_G_GAME_CLOCK), GUEST_U32(p + TSFP_PLAYER_MOVE_STATE),
                GUEST_U32(p + TSFP_PLAYER_FLAGS) & 0xFFu, dx, dy, blocked,
                mode0, GUEST_U32(p + TSFP_PLAYER_LOOK_MODE),
                controls ? GUEST_U8(controls + TSFP_CONTROLS_FLAGS) : 0u,
                GUEST_F32(move), yaw0, yaw1, GUEST_F32(p + TSFP_PLAYER_YAW),
                pitch0, pitch1, GUEST_F32(p + TSFP_PLAYER_PITCH));
    }
}

/* ── Manual function overrides ─────────────────────────────── */

/*
 * Return a function pointer to override the given Xbox VA, or NULL
 * to fall through to the auto-generated dispatch table.
 *
 * This is called on every indirect call (RECOMP_ICALL) and every
 * direct call through the dispatch table, so keep it fast. A chain
 * of if-statements on uint32_t compiles to a simple comparison
 * sequence; for large override tables, consider a sorted array
 * with binary search.
 *
 * Examples of common override patterns:
 *
 *   // Trace wrapper: log entry/exit around the generated function
 *   extern void sub_00012345(void);
 *   static void traced_sub_00012345(void) {
 *       fprintf(stderr, "[TRACE] sub_00012345 entered, eax=0x%08X\n", g_eax);
 *       sub_00012345();
 *       fprintf(stderr, "[TRACE] sub_00012345 returned, eax=0x%08X\n", g_eax);
 *   }
 *
 *   // Stub: skip a function entirely (return 0 in eax)
 *   static void stub_00067890(void) {
 *       g_eax = 0;
 *   }
 *
 *   // Fix: replace a broken lifted function with correct C
 *   static void fixed_sub_000ABCDE(void) {
 *       // Read arguments from stack/registers per calling convention
 *       uint32_t arg1 = g_ecx;
 *       uint32_t arg2 = MEM32(g_esp + 4);
 *       // ... correct implementation ...
 *       g_eax = result;
 *   }
 */
recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    /*
     * TODO: Add your overrides here. Examples:
     *
     * if (xbox_va == 0x00012345) return traced_sub_00012345;
     * if (xbox_va == 0x00067890) return stub_00067890;
     * if (xbox_va == 0x000ABCDE) return fixed_sub_000ABCDE;
     */

    if (xbox_va >= TSFP_PC_OPEN && xbox_va <= TSFP_PC_CANCEL)
        return tsfp_pc_lookup(xbox_va);
    return (recomp_func_t)0;
}

/* ── ICALL failure logging ─────────────────────────────────── */

/*
 * Called when RECOMP_ICALL cannot resolve a target address.
 * This usually means one of:
 *   - A vtable dispatch to an address not in the dispatch table
 *   - A function pointer loaded from uninitialized or corrupt memory
 *   - A kernel thunk address that the bridge doesn't handle
 *
 * During early bring-up you will see many of these. Most are harmless
 * (the ICALL macro pops the dummy return address and continues).
 * Focus on the ones that cause crashes or incorrect behavior.
 */
void recomp_icall_fail_log(uint32_t va)
{
    /*
     * Rate-limited per target, reporting at 1, 10, 100 ...
     *
     * An unresolved target inside a loop is the normal case, not the rare
     * one: Mortal Kombat: Deadly Alliance produced 46843 of these for a
     * single address in one 30-second run, seventeen lines each. That buries
     * every other diagnostic in the log and says nothing the first report did
     * not. The progression is the useful part -- one line says a target was
     * never identified, a run of them says the title is stuck on it.
     */
    enum { SLOTS = 16 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
    }
    hits[i]++;
    {
        uint64_t n = hits[i];
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n != 1)
            return;
    }

    RECOMP_DIAG_LOCK();
    fprintf(stderr, "[ICALL] unresolved %starget 0x%08X -- %llu time(s) "
                    "(total calls: %llu)\n",
            g_icall_dispatch_form == 1 ? "call " :
            g_icall_dispatch_form == 2 ? "jump " : "",
            va, (unsigned long long)hits[i],
            (unsigned long long)g_icall_count);
    if (g_icall_dispatch_form == 2 && hits[i] == 1)
        fprintf(stderr, "  a jump, not a call: if this address is inside a "
                        "function rather than at its start, the guest is doing "
                        "its own control flow (a coroutine, a longjmp, or a "
                        "switch arm) and seeding it as a function will not "
                        "help\n");

    /*
     * Who called it. The lifted caller pushed its return address on the guest
     * stack, so code-range values above esp name the chain -- the same scan
     * the memory watchpoints use. It is a heuristic: stale return addresses
     * from earlier frames show up too, and the first entry is the reliable
     * one. It is still the difference between an address with no context and
     * a function to open, which is what an unresolved target needs.
     */
    {
        const uint8_t *stack = (const uint8_t *)g_xbox_mem_offset
                             + g_icall_saved_esp;
        int shown = 0;
        fprintf(stderr, "  callers:");
        for (i = 0; g_xbox_mem_offset && g_icall_saved_esp
                    && i < 160 && shown < 6; i++) {
            uint32_t v;
            memcpy(&v, stack + (size_t)i * 4, sizeof v);
            if (v >= g_xbox_code_lo && v < g_xbox_code_hi) {
                fprintf(stderr, "%s 0x%08X", shown ? " <-" : "", v);
                shown++;
            }
        }
        if (!shown)
            fprintf(stderr, " (none on the stack)");
        fprintf(stderr, "\n");
    }

    /* The recent-target ring buffer, once per address. What ran just before
     * an unresolved call is context for the first report and noise after. */
    if (hits[i] == 1) {
        fprintf(stderr, "  Recent ICALL targets:\n");
        for (i = 0; i < 16; i++) {
            int idx = (g_icall_trace_idx - 16 + i) & 15;
            if (g_icall_trace[idx])
                fprintf(stderr, "    [%2d] 0x%08X\n", i, g_icall_trace[idx]);
        }
    }
    RECOMP_DIAG_UNLOCK();
    fflush(stderr);
}
/* An indirect call whose target is not code: a null or wild function pointer.
 *
 * Skipping these is right -- calling a data address is worse -- but skipping
 * them *silently* is not. They almost always arrive inside a loop, so the
 * symptom is a hang with no output rather than a diagnosable null vtable call.
 *
 * Rate-limited per address: a spin can produce millions of these, and the
 * useful information is which addresses occur, not how often.
 */
void recomp_icall_not_code_log(uint32_t va, uint32_t saved_esp)
{
    /* A power of ten, because the rate limiter above only reaches this
     * function body at 1, 10, 100 ... and the verdict has to land on one of
     * those. A hundred thousand skips of one target is unambiguous: no title
     * makes progress through that. */
    enum { SLOTS = 16, SPIN_VERDICT = 100000 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    static int said_it;   /* the verdict below is said once, not once per target */
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
    }
    hits[i]++;
    /* Report at 1, 10, 100, 1000 ... rather than once. A single line says a
     * wild pointer was skipped; the progression says it is being skipped in a
     * loop, which is the difference between a curiosity and the reason the
     * title is hung. */
    {
        uint64_t n = hits[i];
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n != 1)
            return;
    }
    {
        /* The call site, read off the guest stack. Without it the log
         * says a wild pointer was skipped but not by whom, and the
         * caller is the only thing that leads anywhere.
         *
         * It comes from the esp the dispatch macro captured, not from
         * g_esp. The lifted call site pushes its return address onto the
         * *local* esp and only syncs g_esp at certain points, so g_esp is
         * stale here -- it read 0 exactly when a null target most needed
         * explaining, which sent three rounds of Jet Set Radio Future
         * chasing inferences instead of a call site. saved_esp is the value
         * before that push, so the return address is the dword below it.
         *
         * Only for the call form. A tail jump pushes no return address, so
         * that dword is whatever the frame happened to leave there: Tony
         * Hawk's Pro Skater 2X reported "from 0x00000008" and then "from
         * 0x00000004" for the same million skipped calls, and both are
         * plainly not code. Printing a number that cannot be an address as
         * though it were the caller is worse than printing nothing, because
         * it is the one field a reader goes to next.
         *
         * So: name the form, and where the top of the stack cannot answer,
         * scan for code-range values the way the unresolved-target logger
         * above does. Stale return addresses from earlier frames come back
         * too and the first is the reliable one, but a list of real code
         * addresses is a place to start and 0x00000008 is not. */
        uint32_t caller = 0;
        if (g_icall_dispatch_form != 2 && saved_esp >= 4 && g_xbox_mem_offset)
            caller = *(const uint32_t *)((const uint8_t *)g_xbox_mem_offset
                                         + (saved_esp - 4));
        if (caller < g_xbox_code_lo || caller >= g_xbox_code_hi)
            caller = 0;
        RECOMP_DIAG_LOCK();
    fprintf(stderr, "[ICALL] target 0x%08X is not code -- skipped "
                        "%llu time(s) via a %s",
                va, (unsigned long long)hits[i],
                g_icall_dispatch_form == 2 ? "jump" : "call");
        if (caller)
            fprintf(stderr, " from 0x%08X", caller);
        fprintf(stderr, " (null or wild function pointer, at call #%llu)\n",
                (unsigned long long)g_icall_count);

        /* The stack scan, once per address: the same heuristic the
         * unresolved-target logger uses, and the only thing that names a
         * caller when the return address is not where it should be. */
        if (hits[i] == 1) {
            const uint8_t *stack = (const uint8_t *)g_xbox_mem_offset
                                 + saved_esp;
            int shown = 0, k;
            fprintf(stderr, "  callers:");
            for (k = 0; g_xbox_mem_offset && saved_esp
                        && k < 160 && shown < 6; k++) {
                uint32_t v;
                memcpy(&v, stack + (size_t)k * 4, sizeof v);
                if (v >= g_xbox_code_lo && v < g_xbox_code_hi) {
                    fprintf(stderr, "%s 0x%08X", shown ? " <-" : "", v);
                    shown++;
                }
            }
            if (!shown)
                fprintf(stderr, " (none on the stack)");
            fprintf(stderr, "\n");
        }

        /* RECOMP_ICALL_FATAL=1: fault here instead of skipping, so the crash
         * handler prints the guest call stack that reached this call.
         *
         * The caller printed above is read from the top of the guest stack,
         * which is right only when the lifted call site pushed a return
         * address there and nothing has since moved esp. When it reads 0 --
         * exactly the case where a null target most needs explaining -- the
         * line says a wild pointer was skipped and nothing about by whom. A
         * deliberate fault costs the run and buys the backtrace. */
        {
            static int fatal = -1;
            if (fatal < 0) {
                const char *v = getenv("RECOMP_ICALL_FATAL");
                fatal = v && *v && strcmp(v, "0") != 0;
            }
            if (fatal) {
                fprintf(stderr, "[ICALL] RECOMP_ICALL_FATAL: faulting here for "
                                "a backtrace\n");
                fflush(stderr);
                *(volatile int *)0 = 1;
            }
        }
    }

    /* Past a certain count this stops being a warning and becomes a verdict.
     *
     * A skipped call sets eax to 0 and returns. Zero is a fine answer for a
     * null function pointer in most code -- it reads as NULL, false, or
     * nothing -- but it is also S_OK, so in COM-shaped code a loop that
     * repeats while the result is non-negative can never leave. Panzer
     * Dragoon Orta hung exactly there, three billion skips in forty seconds,
     * with no frame ever presented.
     *
     * The value is deliberately left alone: there is no return value that is
     * right for both conventions, and guessing failure would break the
     * callers for which zero is correct. What can be fixed is the silence.
     * Say plainly that the title is hung, once, and say what to do about it.
     */
    if (hits[i] == SPIN_VERDICT && !said_it) {
        said_it = 1;
        fprintf(stderr,
            "[ICALL] ^^ this is a hang, not slow progress. One target has been\n"
            "        skipped %d times. The skip returns eax = 0, which is also\n"
            "        S_OK, so a loop testing for a non-negative result will\n"
            "        never exit. Try `py -3 -m tools.seed_from_log <log>` to\n"
            "        recover the target as a function, and read\n"
            "        docs/technical/memory-watchpoints.md for who wrote the\n"
            "        pointer. Set RECOMP_ICALL_SPIN_FATAL=1 to stop here\n"
            "        instead of spinning.\n", SPIN_VERDICT);
        fflush(stderr);
        if (getenv("RECOMP_ICALL_SPIN_FATAL")) {
            fprintf(stderr, "[ICALL] RECOMP_ICALL_SPIN_FATAL is set; exiting.\n");
            fflush(stderr);
            exit(3);
        }
    }
    RECOMP_DIAG_UNLOCK();
    fflush(stderr);
}

