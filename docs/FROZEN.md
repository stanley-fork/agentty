# Frozen scrollback — the live-render retirement pipeline

> Authoritative source is the code: `src/runtime/app/update/frozen.cpp`,
> `src/runtime/view/thread/turn/turn.cpp`, and the freezer declarations in
> `include/agentty/runtime/app/update/internal.hpp`. This document explains
> *why* those files are shaped the way they are. If the two ever disagree,
> the code wins — fix the doc.

## Contents

1. [The problem this subsystem exists to solve](#1-the-problem-this-subsystem-exists-to-solve)
2. [The three layers](#2-the-three-layers)
3. [The five freezer entry points](#3-the-five-freezer-entry-points)
4. [The row accounting and why it must be width-aware](#4-the-row-accounting-and-why-it-must-be-width-aware)
5. [The two trims](#5-the-two-trims)
6. [The end-of-turn repaint (resolved upstream)](#6-the-end-of-turn-repaint-resolved-upstream)
7. [Invariants to preserve](#7-invariants-to-preserve)
8. [Where to look when something is wrong](#8-where-to-look-when-something-is-wrong)
9. [Life of a turn, end to end](#9-life-of-a-turn-end-to-end)
10. [Glossary](#10-glossary)

## 1. The problem this subsystem exists to solve

agentty renders inline at the bottom of your terminal, preserving native
scrollback. Every frame, maya re-derives a full `O(rows × width)` canvas
witness, clears the back buffer, runs `render_tree` over the element graph,
and diffs against the previous frame. That cost scales with the **number of
rows currently on the canvas** — not with how cleverly the content was built.

A naïve TUI chat client rebuilds the entire visible transcript every frame.
After ten turns with a few tool cards each, the per-frame cost climbs until
the spinner stutters and keystroke latency becomes perceptible. Over an
SSH-tunnelled airgap it is worse: every byte that changes between frames is
re-transmitted, so a fat canvas pays a per-byte wire cost on top of the
local layout cost.

The fix is to **retire settled content** out of the per-frame render path
as fast as it settles, so the live cost stays `O(active turn)` regardless of
how long the session runs. That retirement is the job of the frozen
scrollback subsystem.

## 2. The three layers

Content moves through three progressively-cheaper layers as it settles:

| Layer | Where | What it does | Cost after |
|-------|-------|--------------|------------|
| 1. Reveal | `turn.cpp::cached_markdown_for` | Typewriter-reveals streaming bytes at a fixed rate; one `StreamingMarkdown` widget for live AND settled | `O(new_chars)` per frame |
| 2. Settle gating | `turn.cpp::cached_markdown_for` | Fast-paths a fully-settled message to a cached `build()`; commits trailing blocks via `finish()` | `O(1)` per frame |
| 3. Freeze | `frozen.cpp` | Builds settled turns into immutable `Element`s in `m.ui.frozen`, rendered zero-copy via maya `list_ref` | `O(0)` per frame (blit only) |

The live tail — `messages[frozen_through .. end)` — is the only region
rebuilt each frame. Everything before `frozen_through` is a frozen
`Element` snapshot whose `hash_id` is stamped once and reused by maya's
component cache forever.

### Layer 1 — typewriter reveal

The model delivers bytes in chunks of arbitrary size: sometimes one token,
often hundreds of bytes at once. Feeding those verbatim makes text *pop*.
`cached_markdown_for` maintains a reveal cursor (`MessageMdCache::revealed_size`)
and feeds the widget only `source[0 .. revealed_size]` each frame, advancing
the cursor at `kRevealCharsPerSec = 220` (≈ChatGPT cadence):

- The cursor is **time-based** — `elapsed_µs × rate` — and advances by the
  bytes it actually consumed, not by resetting to `now`, so sub-character
  fractions aren't silently dropped to floating-point drift each frame.
- **Backlog snap**: if the model is more than `kBacklogSnap = 200` bytes
  ahead (a sudden multi-KB dump), the cursor jumps forward to within
  `kBacklogSnap` of `source.size()`. The reveal is a *smoothing filter*,
  not a hard rate limit.
- **Codepoint-clean**: the target byte index is rounded *down* off any UTF-8
  continuation byte (`& 0xC0 == 0x80`) so the parser never sees half a
  multibyte sequence.
- On **settle** the cursor snaps to full size immediately, because the
  freezer is about to snapshot this message and stop calling
  `cached_markdown_for` for it — a half-revealed snapshot would freeze
  scramble glyphs forever.

One widget renders both live and settled content. The streaming widget's
output (a prefix `ComponentElement` plus a tail `Element`, wrapped in
`vstack.gap(1)`) has a different total height than the one-shot
`maya::markdown()` parser's output. Swapping between them at
`StreamFinished` shifted the canvas by about three rows, which propagated
through the per-row diff and left the composer at a different terminal row —
visible as "composer pulled down plus a duplicate composer above it on the
first keypress." Staying on the streaming widget keeps height stable across
the streaming → idle seam.

### Layer 2 — settle gating

Two fast paths keep settled turns from paying per-frame cost:

- The **fully-settled fast path** at the top of `cached_markdown_for`: if a
  message is settled and both `last_settled_size` and `revealed_size` equal
  `source.size()`, return the cached `build()` directly and skip all
  per-frame work. This is the dominant case on long sessions.
- `set_content_async` keeps tiny appends on the cheap synchronous
  incremental path but offloads a ≥16 KB diverging-prefix swap (loading an
  old thread, a long paste) to a worker, so the render thread never stalls
  on a big parse.

`last_settled_size` is a **size**, not a content hash. The invariant that
makes this sound: `msg.text` is never rewritten in place after
`StreamFinished` moves `streaming_text → text`. Any edit replaces the whole
`Message` (new `MessageId` → new cache slot), so size alone is sufficient.
Hashing every visible turn's bytes every frame to guard against an in-place
rewrite that doesn't exist was the old dominant per-frame cost.

`finish()` flushes whatever is still in the streaming tail into the
canonical committed block path. It is idempotent (a no-op once
`committed_ == source_.size()`). Historically it was load-bearing for a
trailing closed code fence — see §6.

### Layer 3 — the freezer

This is `frozen.cpp`. Settled transcript is pre-built into `m.ui.frozen`
(a `vector<Element>`) and rendered zero-copy. The parallel `frozen_rows`
vector and `frozen_row_total` running sum bound the canvas by *row count*,
which is what per-frame cost actually scales with.

## 3. The five freezer entry points

```
freeze_through(m, live_start)        bulk retire at each new user turn
freeze_settled_subturns(m)           mid-run incremental, per ToolExecOutput
freeze_streaming_text_prefix(m)      mid-stream split for a long prose body
trim_frozen_if_oversized(m)          turn-boundary trim (may drop on-screen)
trim_frozen_above_viewport(m)        mid-run-SAFE trim (only off-screen)
rehydrate_frozen(m) / clear_frozen(m)  thread load / reset
```

### freeze_through

At each new user turn, walk `[frozen_through .. live_start)` and push built
Turn `Element`s (each preceded by a gap row) into frozen. One Turn per
speaker-run: a User message is its own Turn; a run of consecutive Assistant
messages collapses into one Turn whose body interleaves each sub-turn's text
and tool batch. The same `ui::turn_run_end` helper drives the run boundary
here and in the live-tail builder, so frozen and live row shapes are
identical for the same input.

A run is only frozen if every tool in it is terminal (`run_is_freezable`).
Freezing a run with a still-Running tool would snapshot a permanently-Running
spinner into scrollback, because the `hash_id` is stamped once and never
recomputed. When a non-terminal tool is hit, `frozen_through` advances only
to the start of that run, and the next pass resumes there once the live path
has settled it.

### freeze_settled_subturns

During an active auto-pilot run, the trailing Assistant run can't be
`freeze_through`'d — its tail is still streaming or has a running tool — so
the whole run stays live and re-lays-out every frame. This entry point
freezes the *completed leading sub-turns* (all tools terminal) one at a time
on each `ToolExecOutput`, committing them as a continuation entry. It sets
`frozen_midrun` so the live tail draws the remainder as a continuation:
rail only, no repeated header, no turn number, no inter-turn gap. The result
reads as one turn even though it's split across the frozen prefix and the
live tail.

### freeze_streaming_text_prefix

The gap the other two miss: a long *pure-text* answer (a prose reply with no
tool calls) has no terminal tool to mark a sub-turn done, so the whole
growing markdown body stays live. At 5,000 lines that is roughly 13 ms per
frame of re-layout versus 0.26 ms flat for a tail-windowed card.

This splits the active text-only message at the last safe markdown block
boundary — a blank line outside an open code fence — keeping a trailing
window live so the actively-revealing edge still animates. The committed
prefix becomes its own settled Assistant `Message` inserted just before the
active tail, producing a `[settled-text][growing-text]` run: the exact shape
a post-tool continuation makes, which `freeze_settled_subturns` then freezes
with no new render logic.

The live tail window is **width-aware**: "well under a viewport of prose" is
a row budget, not a fixed byte count. 1 KB of prose is about 13 rows at 80
columns but 25-plus rows at 40 columns, so the byte budget is derived from
the real wrap width (`kLiveTailBytes ≈ cols × half-a-screen-of-rows`, floored
at 1 KB) instead of a hard `1024`.

#### The fence fallback

A single giant code block — "write the whole file" — has *no* blank-line
boundary outside an open fence, so the clean split returns 0 and the entire
block would stay live, re-laid-out every frame. That is the exact unbounded
cost this function exists to bound, applied to the one content shape most
likely to be enormous.

The fallback: when scanning ends inside one open fence past a viewport,
split at the last fence-internal newline and **close + reopen the fence**.
The frozen half gets a synthetic closing marker so it parses as a complete
code block; the live tail is re-seeded with the same opening marker (kind
and info string preserved) so its remaining lines keep rendering as that
same language. The content the user sees is identical — only an internal
fence boundary is synthesized at a line break. Here is what the transform
looks like on a Python block streaming in:

```python
# ───────────────────────────────────────────────────────────────────────
# BEFORE the fallback fires: the whole block is one live message.
# streaming_text holds everything; no blank-line boundary exists outside
# the fence, so last_safe_block_split() returns 0 and nothing freezes.
# ───────────────────────────────────────────────────────────────────────
#
#   ```python
#   def build_index(paths):
#       index = {}
#       for p in paths:
#           ...                       # 3000 more lines, all live, all
#           ...                       # re-laid-out every single frame
#   ```  <- not here yet; still streaming
#
# ───────────────────────────────────────────────────────────────────────
# AFTER the fallback fires at the last fence-internal newline past the
# live window:
# ───────────────────────────────────────────────────────────────────────
#
#   FROZEN HALF (settled Message.text, parses as a complete block):
#     ```python
#     def build_index(paths):
#         index = {}
#         for p in paths:
#             ...                     # the committed prefix
#     ```                            # <- SYNTHETIC closing marker
#
#   LIVE HALF (active.streaming_text, re-seeded with the opening marker):
#     ```python                      # <- SYNTHETIC reopening marker
#             ...                     # the still-growing tail, now small
#     ```  <- arrives later, real close

def build_index(paths):
    """Map every token to the files that contain it.

    This is the kind of body that used to pin the live canvas at
    thousands of rows. With the fence fallback it graduates into the
    frozen prefix in viewport-sized chunks, each chunk a syntactically
    complete code block that maya can blit from cache forever.
    """
    index: dict[str, set[str]] = {}
    for path in paths:
        try:
            with open(path, "r", encoding="utf-8") as handle:
                text = handle.read()
        except (OSError, UnicodeDecodeError):
            # Unreadable or binary file — skip, don't crash the indexer.
            continue
        for token in tokenize(text):
            index.setdefault(token, set()).add(path)
    return index


def tokenize(text):
    """Cheap identifier tokenizer: runs of [A-Za-z0-9_], lowercased."""
    token = []
    for ch in text:
        if ch.isalnum() or ch == "_":
            token.append(ch.lower())
        elif token:
            yield "".join(token)
            token = []
    if token:
        yield "".join(token)


def query(index, term):
    """Return the set of files containing `term`, or empty set."""
    return index.get(term.lower(), set())
```

The key correctness property: at no point does the user-visible *rendered*
content change. A complete code block followed by a continuation of the same
code block looks, line for line, exactly like the original uninterrupted
block. The only thing that changed is which bytes live in the frozen
(blit-forever) layer versus the live (re-laid-out) layer.

## 4. The row accounting and why it must be width-aware

Both trims and the canvas sizing depend on `frozen_rows`, populated by
`estimate_msg_rows` at push time. The actual rendered height is decided by
maya at layout time against the real terminal width — so the estimate is the
*only* thing tying the trim's correctness to the real canvas.

`estimate_msg_rows` therefore counts **wrapped rows**, not bytes:

- Prose bodies (`text`, `streaming_text`) go through `wrapped_rows()`, which
  is newline-aware — each hard newline starts a fresh row, and a line longer
  than the content width soft-wraps to `ceil(len / cols)` rows. This mirrors
  what the renderer does.
- Tool-card bodies are estimated from a cheap non-allocating byte walk of the
  args JSON (`estimate_json_bytes`) divided by the real content width. The
  rendered body of a settled card comes from its **args**, not its one-line
  `output()` footer: a write card shows `args["content"]` (the whole file),
  an edit card shows every hunk. Counting `output()` alone once
  under-estimated a 3,000-line write as roughly one row, the row cap never
  tripped, and the canvas ballooned while `frozen_row_total` read tiny.
- Per-card and per-message chrome are fixed row counts, width-independent.

The width comes from `term_dims()` — one `TIOCGWINSZ` ioctl returning both
axes — read through `estimate_wrap_cols()` (clamped to a 16-column floor,
minus a few columns for the turn rail). Crucially, `frozen_row_budget()` and
`trim_frozen_above_viewport()` read terminal geometry through the *same*
helper, so width (the row estimate) and height (the keep margin) can never
disagree across a frame.

Why this matters: the trims' safety argument is "keep at least a viewport of
real rows, so the oldest kept entry's top sits at or above the viewport top,
so everything dropped is provably already in native scrollback." That proof
is only as good as the estimate. A fixed 60-column divisor (the old code)
over-counts on an 80-plus column terminal — the *safe* direction, keep too
much — but **under**-counts on a narrow terminal, so the trim drops an entry
that is still on screen and re-emits committed scrollback at a shifted row:
the duplication ghost the trim exists to prevent. Counting against the real
width removes the entire failure mode.

## 5. The two trims

`trim_frozen_if_oversized` runs at **turn boundaries** (submit, stream
finish). It can drop entries that are still on screen — safe at a turn
boundary because the whole canvas is about to be re-diffed anyway — and
caps both total rows (a ~2-viewport budget) and entry count (120). The keep
count is row-driven: walk back from the newest entry summing rows until the
budget is covered; widen to `kKeepMinEntries = 8` only if those entries were
small enough that the budget wasn't yet spent. A single giant body that fills
the budget keeps just itself (floored at 2 so the latest exchange is always
visible).

`trim_frozen_above_viewport` runs **during an active run**. It is the
conservative variant: it only drops entries provably above the viewport. It
keeps `kViewportKeepRows ≈ 1.5 × term_h` of the most recent frozen content
on the canvas, and never drops the last two entries. Because every kept row
count is now width-accurate, "provably above the viewport" is provable
against reality, not against a guess.

Both trims return `Cmd::commit_scrollback_overflow()` when they drop
anything — a trigger telling maya to release whatever has already overflowed
the viewport. The Cmd carries no row count: only the renderer knows the
right physical-row figure (`max(0, prev_rows − term_h)`), so the row-counted
variant was retired in the maya scrollback-corruption audit.

## 6. The end-of-turn repaint (resolved upstream)

A message ending at a closing code fence with no trailing newline — the
common shape for a reply that ends in a code example — used to leave the last
block stuck in the streaming tail. The tail rendered via `render_tail`'s
inline path; `finish()` re-rendered via the canonical `md_block_to_element`
path. The two feed the same border/padding builder slightly different code
strings, so their painted cells weren't byte-identical. At settle, the whole
last block re-emitted to the terminal — the visible "repaint," worst over
SSH.

This is fixed upstream in maya: `boundary.cpp`'s `find_block_boundary` now
eager-commits a closing fence at end-of-buffer, so the live and settled cells
match before `finish()` is ever called. `finish()` is kept in
`cached_markdown_for` because it is idempotent and is still the right place
to flush any *other* trailing-block kind — see the comment there. The maya
side is guarded by the `st_eager_closing_fence` cell-equality test.

## 7. Invariants to preserve

1. **`frozen` is append-only and immutable.** Entries are built once; their
   `hash_id` is stamped at freeze time and never recomputed. Any mutation of
   `messages[i]` for `i < frozen_through` is forbidden — route tool mutations
   through `with_live_tool`, which only touches the live tail. A retroactive
   edit must call `rehydrate_frozen()` to rebuild from scratch.
2. **`frozen.size()`, `frozen_rows`, and `frozen_row_total` stay in
   lockstep.** Every push goes through `push_frozen`; every trim updates all
   three together.
3. **Row accounting reflects the real terminal width.** Never reintroduce a
   fixed-width divisor — the trim correctness depends on it.
4. **Never freeze a run with a non-terminal tool** (`run_is_freezable`).
5. **The live tail and the frozen prefix use the same run-boundary helper**
   (`ui::turn_run_end`) so a turn looks identical whichever layer renders it.
6. **`force_redraw` is resize-only.** It demotes to a Divergent repaint that
   can wipe native scrollback; it must never be batched into normal flow.

## 8. Where to look when something is wrong

- Composer jumps / ghost composer on first keypress after a reply →
  height instability across the streaming → idle seam (Layer 1/2,
  `cached_markdown_for`).
- A turn appears twice in scrollback during a long run → a trim dropped an
  on-screen entry (row undercount, or `trim_frozen_if_oversized` fired
  mid-run instead of `trim_frozen_above_viewport`).
- Spinner stutters / keystroke lag on a long thread → `frozen_row_total` is
  not bounded (estimate undercount, or a trim isn't firing).
- A permanently-spinning tool in scrollback → a run was frozen before its
  tools went terminal (`run_is_freezable` bypassed).
- The whole last code block flickers at settle → the maya boundary fix (§6);
  verify with the `st_eager_closing_fence` test.

## 9. Life of a turn, end to end

The clearest way to hold the subsystem in your head is to follow one turn
from the user pressing Enter to it landing in native scrollback. The numbers
on the left are the layer (§2) doing the work.

```
user presses Enter
  │
  ├─ submit_message
  │    (3) freeze_through(messages.size())
  │          → the ENTIRE prior transcript retires into m.ui.frozen.
  │            frozen_through now == old message count. Live tail is empty.
  │    trim_frozen_if_oversized()  (turn boundary → on-screen drop is safe)
  │
  ├─ launch_stream → bytes start arriving
  │
  ▼
Assistant streams (one live message at messages.back())
  │
  ├─ each Tick / StreamTextDelta:
  │    (1) cached_markdown_for advances the reveal cursor at 220 chars/s,
  │        feeds source[0..revealed_size] to the StreamingMarkdown widget.
  │        request_animation_frame() keeps the loop awake while revealing.
  │
  ├─ if it's a long PURE-TEXT answer (no tools):
  │    (3) freeze_streaming_text_prefix splits at the last safe block
  │        boundary (or, inside one giant fence, close+reopen the fence).
  │        The committed prefix becomes its own settled Message inserted
  │        before back(); freeze_settled_subturns freezes it this same tick.
  │        Live tail shrinks back to ~half a screen.
  │
  ├─ if it runs tools (auto-pilot):
  │    on each ToolExecOutput:
  │      (3) freeze_settled_subturns freezes completed leading sub-turns,
  │          sets frozen_midrun so the live remainder draws as a
  │          continuation (no repeated header / turn number / gap).
  │      (3) trim_frozen_above_viewport()  (mid-run → only off-screen drop)
  │
  ▼
StreamFinished
  │
  ├─ (1) reveal cursor snaps to source.size() (no partial-text snapshot)
  ├─ (2) finish() flushes the trailing block to the committed path;
  │      last_settled_size = size → the fully-settled fast path engages.
  │      From here this message costs O(1)/frame: a cached build().
  │
  ▼
next user turn
  │
  └─ (3) freeze_through retires this whole run into m.ui.frozen.
         As newer turns push the canvas past ~2 viewports, the oldest
         frozen entries are trimmed; their rows are already in the
         terminal's NATIVE scrollback, so scroll-up still shows them —
         painted instantly by the terminal, never re-emitted by agentty.
```

The single invariant the whole sequence protects: **per-frame cost is
`O(active turn)`, never `O(transcript)`** — and the bytes that change
between frames stay minimal, which is what an SSH link cares about.

## 10. Glossary

- **Frozen entry** — one immutable `Element` in `m.ui.frozen` representing a
  settled visual unit (a turn, a gap row, or a compaction divider). Built
  once, blitted from maya's component cache forever via its `hash_id`.
- **Live tail** — `messages[frozen_through .. end)`. The only region rebuilt
  each frame. Everything before it is frozen.
- **`frozen_through`** — exclusive upper bound into `messages`: the boundary
  between frozen and live.
- **Run / speaker-run** — a maximal stretch of consecutive messages from the
  same speaker. One User message is its own run; consecutive Assistant
  messages (text + tool sub-turns) collapse into one Turn. Boundary computed
  by `ui::turn_run_end`.
- **Sub-turn** — one message within an Assistant run: a chunk of prose, or a
  batch of tool calls. A run is a sequence of sub-turns.
- **`frozen_midrun`** — set when the freezer committed the leading sub-turns
  of a still-active run. Tells the live tail to render the remainder as a
  *continuation* (rail only) so the split run reads as one turn.
- **Reveal cursor** (`revealed_size`) — the typewriter playback head; how
  many bytes of the source have been fed to the widget so far.
- **Settle** — the moment a message's bytes are final
  (`streaming_text` drained into `text`). The reveal snaps to full and the
  fast path engages.
- **Commit (scrollback)** — releasing rows that have overflowed the viewport
  to the terminal's native scrollback via
  `Cmd::commit_scrollback_overflow()`. The terminal owns them after that.
- **Divergent repaint** — maya's full wipe-and-redraw, triggered by resize /
  `force_redraw`. Expensive and scrollback-disturbing; bounding the canvas
  to ~2 viewports keeps even this cheap.
- **`hash_id`** — the cache key stamped on a frozen Turn's config. Folds in
  every run-member `MessageId` plus its render key, so a different run
  produces a different key and maya reuses painted cells for an unchanged
  one.
