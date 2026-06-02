# Streaming render test — v4

The write/edit card lag is fixed. The only thing left to verify is the
maya inline-frame bottom-anchor (empty band / jump-up after a tool
finishes when the transcript overflows the viewport).

## The prompt (paste this)

> Write `scratch/big.cpp`: a C++ file with 300 lines, one
> `void fn_0001() {}` … `void fn_0300() {}` per line, numbered with
> zero-padded 4-digit indices. Then edit that file to renumber every
> function +1000 (fn_0001 → fn_1001, …). Don't explain, just do it.

## Fixed (tool_body_preview.cpp)

- [x] `write` card: stable height while streaming, full body on ✓ DONE.
- [x] `edit` card: full diff on settle, no "squeeze then expand" lag
      (fence-extraction path was eliding terminal edits in the live
      tail until freeze).

## Still open (maya inline-frame bottom-anchor)

- [ ] No empty band below the status bar (frame ends on the last row).
- [ ] No "jump up" after a tool finishes when content overflows.

## How to repro the open issues

1. Make the terminal short enough that the prompt above overflows the
   viewport (or use a long enough file — 300 lines guarantees it).
2. Send the prompt; watch the moment each tool flips to ✓ DONE.
3. Empty-band bug: after settle, the status bar floats with blank rows
   between it and the terminal's bottom edge.
4. Jump-up bug: the whole frame snaps upward one beat after the tool
   finishes (the freeze handoff re-anchors the canvas).

## Notes / next steps

- These two are a maya concern, not agentty's view layer — the
   composer + status bar ride the bottom of a vstack that overflows
   into native scrollback, so their screen row is decided by maya's
   inline-frame emit (serialize.cpp case-(B)), not by anything
   conversation_config does.
- A view-layer bottom-pin pad was tried and reverted: it fought maya's
   own anchoring and made the jump worse.
- Fix likely lives in serialize.cpp's viewport-cap / cursor_up math at
   the freeze handoff (canvas grows by the now-frozen rows in one frame).

