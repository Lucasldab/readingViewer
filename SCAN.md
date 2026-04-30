# SCAN.md — Pan-Clamping & Image-Stitching Boundary Audit

Generated: 2026-04-30  
Source examined: `rv.c` (1011 lines, single-file SDL2 viewer)

---

## (a) All Clamp Call Sites

### `clamp_scroll()` — rv.c:573–582
Clamps only `V.scroll_target` (not `V.scroll_y`) against `[0, total_h - win_h/zoom]`.

| Caller | File:Line | Context |
|--------|-----------|---------|
| `ipc_handle` | rv.c:676 | `goto` command sets `scroll_target = images[idx].y_offset` |
| `ipc_handle` | rv.c:681 | `scroll` command adds raw pixel offset |
| `do_action` | rv.c:706 | `ACT_SCROLL_DOWN` |
| `do_action` | rv.c:709 | `ACT_SCROLL_UP` |
| `do_action` | rv.c:713 | `ACT_FAST_SCROLL_DOWN` |
| `do_action` | rv.c:716 | `ACT_FAST_SCROLL_UP` |
| `do_action` | rv.c:721 | `ACT_PAGE_DOWN` |
| `do_action` | rv.c:725 | `ACT_PAGE_UP` |
| `do_action` | rv.c:733 | `ACT_BOTTOM` |
| `handle_event` | rv.c:803 | Mouse-wheel (non-Ctrl) scroll |
| `handle_event` | rv.c:831 | Mouse-drag motion |

### `clamp_pan()` — rv.c:584–594
Clamps only `V.pan_x_target` against `[-max_pan, +max_pan]` where `max_pan = (win_w*zoom - win_w)/2`.

| Caller | File:Line | Context |
|--------|-----------|---------|
| `do_action` | rv.c:744 | `ACT_ZOOM_OUT` |
| `do_action` | rv.c:758 | `ACT_PAN_LEFT` |
| `do_action` | rv.c:761 | `ACT_PAN_RIGHT` |
| `handle_event` | rv.c:800 | Ctrl+mouse-wheel zoom |
| `handle_event` | rv.c:836 | Mouse-drag motion |

---

## (b) Edge Cases Not Yet Covered

### BUG-1 (Critical): Drag clamping is a no-op — clamp immediately overwritten

In `handle_event / SDL_MOUSEMOTION` (rv.c:823–838):

```c
V.scroll_target -= dy / V.zoom;
V.scroll_y      -= dy / V.zoom;
clamp_scroll();                      // clamps scroll_target
V.scroll_target = V.scroll_y;       // ← overwrites clamped value with unclamped scroll_y

V.pan_x_target += dx;
V.pan_x        += dx;
clamp_pan();                         // clamps pan_x_target
V.pan_x_target = V.pan_x;          // ← overwrites clamped value with unclamped pan_x
```

`clamp_scroll()` touches only `scroll_target`; `clamp_pan()` touches only `pan_x_target`.
The "snap" assignments that follow immediately restore the unclamped values from the live
`scroll_y` / `pan_x` fields. Result: mouse-drag bypasses all boundary enforcement. You can
drag the strip past the top, past the bottom, and arbitrarily far left or right.

Fix: clamp both the target **and** the actual value, or apply `V.scroll_y = V.scroll_target`
(copy the clamped target into the live field) instead of the reverse.

### BUG-2 (Moderate): Zoom-out never re-clamps scroll position

`ACT_ZOOM_OUT` (rv.c:741–745) calls `clamp_pan()` but not `clamp_scroll()`.
When you zoom out, `max_scroll = total_h - win_h/zoom` shrinks (larger viewport in layout
coords). If `scroll_target` was already near the bottom it can now exceed the new
`max_scroll`, leaving the strip scrolled off the bottom after zoom.

Same gap at Ctrl+wheel zoom (rv.c:794–800): `clamp_pan()` called, `clamp_scroll()` missing.

### BUG-3 (Moderate): `clamp_pan()` uses wrong content-width formula for narrow images

```c
double scaled_content_w = V.win_w * V.zoom;   // rv.c:586
```

Actual rendered image width is `img_w * fit_scale(img_w) * zoom`.
`fit_scale` returns `min(win_w/img_w, 1.0)`. For images **narrower** than the window
(`img_w < win_w`), `fit_scale = 1.0`, so rendered width = `img_w * zoom`, which is
**less** than `win_w * zoom`. `clamp_pan` allows panning up to `(win_w*zoom - win_w)/2`
pixels — wider than the actual image — exposing background on both sides and breaking
the "content always touches one viewport edge" invariant.

For the common manhwa case (`img_w >= win_w`) `fit_scale = win_w/img_w`, rendered width
= `win_w * zoom`, so the formula is accidentally correct. Bug only surfaces for narrow panels.

### BUG-4 (Minor): Sub-pixel seam gaps between stitched images at zoom ≠ 1.0

`layout()` stores `y_offset` as a truncated integer (rv.c:477):
```c
y += (int)(h * scale);
```

`render()` independently recomputes pixel height (rv.c:521):
```c
int scaled_h = (int)(h * scale * V.zoom);
```

The y-position of image N+1 is `(int)(offset_{N+1} * zoom - zy)` where
`offset_{N+1} = offset_N + (int)(h_N * scale_N)` (integer, from layout).
The bottom edge of image N is `y_N + (int)(h_N * scale_N * zoom)`.

Because `(int)(x*z) + (int)(y*z) ≠ (int)((x+y)*z)` in general, 1-pixel gaps (or
overlaps) appear at seam boundaries whenever zoom is not an integer. Over a 100-image
chapter the visual stutter is noticeable during slow scroll.

### BUG-5 (Minor): ACT_ZOOM_IN (keyboard) skips `clamp_pan()` while Ctrl+wheel does not

`do_action(ACT_ZOOM_IN)` (rv.c:735–739) sets `needs_layout = 1` with no clamp.
The Ctrl+wheel path (rv.c:794–800) calls `clamp_pan()` after the same zoom change.
Zooming in expands content, so existing positions usually remain valid; the asymmetry
still means a user who zooms in to maximum, pans hard, then zooms out via keyboard can
land outside the clamped range until the next explicit pan input.

---

## (c) Recommended Unit Test — BUG-1 (Drag Clamping No-Op)

This is the most likely regression to cause a bug report: dragging allows arbitrary
out-of-bounds scroll, making the content strip vanish off the screen.

```c
/*
 * test_drag_clamp.c
 *
 * Compile: cc -o test_drag_clamp test_drag_clamp.c
 * Run:     ./test_drag_clamp
 *
 * Exercises the drag-motion clamp logic extracted from handle_event().
 * Verifies that scroll_y is bounded to [0, max_scroll] after a large upward drag.
 */

#include <assert.h>
#include <stdio.h>

/* Minimal state mirroring V fields used in the drag handler */
static struct {
    double scroll_y, scroll_target;
    double pan_x, pan_x_target;
    double zoom;
    int    win_w, win_h;
    double total_h;
} S;

static void clamp_scroll(void)
{
    if (S.scroll_target < 0) S.scroll_target = 0;
    double max_s = S.total_h - (double)S.win_h / S.zoom;
    if (max_s < 0) max_s = 0;
    if (S.scroll_target > max_s) S.scroll_target = max_s;
}

/* Current (buggy) drag handler — copy-pasted from rv.c:829-833 */
static void drag_scroll_buggy(int dy)
{
    S.scroll_target -= dy / S.zoom;
    S.scroll_y      -= dy / S.zoom;
    clamp_scroll();
    S.scroll_target = S.scroll_y;   /* BUG: overwrites clamp */
}

/* Fixed drag handler */
static void drag_scroll_fixed(int dy)
{
    S.scroll_target -= dy / S.zoom;
    S.scroll_y      -= dy / S.zoom;
    clamp_scroll();
    S.scroll_y = S.scroll_target;   /* copy clamped value into live field */
}

int main(void)
{
    S.win_w = 800; S.win_h = 900;
    S.zoom  = 1.0;
    S.total_h = 5000;   /* 5-panel chapter */
    S.scroll_y = S.scroll_target = 0.0;

    /* Simulate dragging upward by 2000px past top boundary */
    int big_upward_drag = 2000;

    /* --- buggy path --- */
    drag_scroll_buggy(big_upward_drag);
    printf("buggy  scroll_y=%.1f  (expected: 0.0)\n", S.scroll_y);
    /* With the bug, scroll_y == -2000 (past top), assert fails */
    assert(S.scroll_y >= 0.0 && "BUG-1: drag bypasses clamp — scroll_y went negative");

    /* --- fixed path --- */
    S.scroll_y = S.scroll_target = 0.0;
    drag_scroll_fixed(big_upward_drag);
    printf("fixed  scroll_y=%.1f  (expected: 0.0)\n", S.scroll_y);
    assert(S.scroll_y >= 0.0 && "fixed path should hold at 0");

    /* Also test past-bottom clamp */
    double max_s = S.total_h - S.win_h / S.zoom;   /* 4100 */
    S.scroll_y = S.scroll_target = max_s - 10.0;
    drag_scroll_fixed(-3000);   /* large downward drag */
    printf("fixed  scroll_y=%.1f  (expected: %.1f)\n", S.scroll_y, max_s);
    assert(S.scroll_y <= max_s + 0.01 && "fixed path should hold at max_scroll");

    printf("All assertions passed.\n");
    return 0;
}
```

The test will **fail on the first assert** with the current code, confirming BUG-1.
Applying the one-line fix (`S.scroll_y = S.scroll_target` instead of the reverse) makes
all three assertions pass.

---

## Summary Table

| ID | Severity | Location | Description |
|----|----------|----------|-------------|
| BUG-1 | **Critical** | rv.c:829–837 | Drag clamp overwritten — out-of-bounds scroll & pan |
| BUG-2 | Moderate | rv.c:741–745, 794–800 | Zoom-out missing `clamp_scroll()` |
| BUG-3 | Moderate | rv.c:586 | `clamp_pan` uses `win_w*zoom` not actual image width |
| BUG-4 | Minor | rv.c:477, 521 | Integer truncation causes 1-px seam gaps at zoom ≠ 1 |
| BUG-5 | Minor | rv.c:735–739 | Keyboard zoom-in skips `clamp_pan()` |
