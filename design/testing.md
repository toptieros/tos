# tOS — testing philosophy

Our test suite started as one shape: **every test boots the whole OS in QEMU and
drives it over the serial port.** That is an *end-to-end system test* — the slowest,
most fragile, most expensive kind there is — and we were using it for *everything*,
including pure logic and "does this widget exist." That is the classic inverted
pyramid (the "ice-cream cone"): the wrong shape. This document is the corrective.

## The pyramid

```
        e2e / system   ~15   boot the real OS, drive a critical journey   (slow, precious)
      integration       few   a couple of components together
   unit                 many  one pure function, on the host, microseconds (cheap, plentiful)
```

- **Unit tests** (`tests/unit/`, `make unit`). Compile a *pure* tOS function with the
  host compiler and assert on it directly. No QEMU. Runs in milliseconds. This is where
  **most new tests should go** and where most logic belongs: parsers, string/index math,
  geometry, data-structure invariants.
- **Integration tests.** A few components wired together (fs read/write round-trip,
  pty + shell). In practice these live in the e2e harness today; that's fine when the
  thing under test genuinely needs the kernel.
- **e2e / system tests** (`tests/run_tests.py`, `make test`). Boot tOS under QEMU on
  **both** the BIOS and UEFI paths and exercise a *critical user journey*. Keep this set
  **small and deliberate** (~12–18). Every one costs a full boot and is the most likely
  to flake (we have hit host-load and screen-coordinate flakiness more than once).

## The rule: a test must earn its place

Add a test only when it does one of these:

1. **Guards an invariant that is hard to get right and easy to silently break** — e.g.
   word-boundary math, the CSI/SGR parser, line-wrapping, tosfs slot/parent arithmetic,
   the MRU snapshot order, a fault-isolation guarantee.
2. **Pins a specific bug** so it cannot come back.

"I built a feature, therefore I owe an e2e test" is **not** a reason. That reflex is
exactly what balloons a suite to a thousand brittle boot tests. If a regression would be
**instantly obvious the moment you boot** (Notepad won't open; the dock is gone), it does
not need a permanent QEMU test — it needs to be covered *once* by a general smoke journey,
and verified during development with a screenshot.

## Where each kind of change goes

| You changed… | Test it as… |
|---|---|
| Pure logic (parse, wrap, index math, geometry, fs metadata) | **Unit test** (required) |
| "Is this wired end-to-end / does it not crash?" | Fold into an **existing smoke journey**; only add a new e2e if it's a critical journey or a fixed bug |
| UI look / interaction polish | **Dev-time screenshot** + manual check; do *not* enshrine a permanent e2e |
| A bug you just fixed | A **regression test** at the lowest level that reproduces it (prefer unit) |

A good gut check: *"If I delete this test, what real failure slips through — and would I
not have noticed it anyway?"* If the honest answer is "nothing / I'd notice instantly,"
the test is cost without coverage.

## Tiers (2026-06-10): smoke by default, the catalog on demand

The doc above said "keep the e2e set small" — and the suite still ballooned to 56
boots because every feature kept adding "its" journey and every increment re-ran all
of them. The structure now enforces the policy:

- **`make test` = the SMOKE tier** (`SMOKE_BIOS`/`SMOKE_UEFI_LIST` in run_tests.py,
  ~10 BIOS + 3 UEFI boots): boot+fs, the in-OS **selftest** batch, reboot persistence,
  desktop + window management, the two richest app journeys (Files details, Trash,
  Notepad edit/save), focus switching, mouse. This is the day-to-day gate (`make check`
  = `unit` + this).
- **`make test-all` = the full catalog** (`--all`): every journey we have. Run it for
  cross-cutting kernel / compositor / toolkit changes and before tagging a release —
  **not** per feature increment.
- **`selftest` (user/selftest/selftest.c)**: an in-OS self-check program — dozens of
  native fs/registry/proc/clipboard assertions in **one** boot, where the suite used to
  spend a boot per area driving the shell over serial. Type `selftest` in the terminal;
  it prints `selftest: N/N OK` (failures name the exact expression). **Add checks here
  before reaching for a new boot test.**
- **Verification boots are disposable.** While building a feature: unit-test the logic,
  drive ONE ad-hoc boot (a `tests/repro_*.py` script or a manual harness run) to see it
  work, screenshot it if it's visible — and that's it. The boot that verified the
  feature does **not** get enshrined in the suite; at most the feature earns an assert
  folded into an existing smoke journey.

## The e2e smoke/journey set (what stays in QEMU)

Keep these because they prove the system is fundamentally alive, and because the thing
under test genuinely needs a real machine:

- **Boot** both paths (BIOS + UEFI) to a usable shell; the desktop + dock come up.
- **Filesystem**: a CRUD journey (write/read/rewrite/rm, mkdir/cd/mv/rm -r) and
  **persistence across a reboot** (the one thing only a real disk can prove).
- **Processes**: fork/exec/wait + zombie reap, orphan reparenting, **SMP**, and
  **fault isolation** (a crashing app must not halt the machine).
- **Compositor**: *one* representative interaction journey (launch an app, switch
  windows, route a client click/drag through the compositor, close a window).
- **An app → fs journey**: type in a GUI editor, save, read it back through the shell.
- **Hardware smoke**: RAM scales with the machine (no fixed cap), mouse tracks, RTC,
  PCI enumeration.
- **No panic / no exception** is asserted by every e2e test.

Everything else — per-feature GUI behaviours (launcher filtering, status-bar layout,
scroll-thumb drag, each clipboard chord, notification animations) — is either the
*logic* under it (→ unit test) or "it's wired" (→ folded into a journey) or polish
(→ a screenshot during development), **not** a standalone permanent e2e test.

## Mechanics

tOS userspace and kernel are freestanding C/C++ (no host libc assumptions, syscalls,
globals), so only **pure** functions unit-test cleanly. The pattern:

- Keep pure logic in plain-C headers (e.g. `user/lib/textutil.h`, `user/lib/applist.h`)
  that the OS compiles as today **and** the host test `#include`s.
- For a function with a small dependency (e.g. `ugfx_text_w` for pixel widths), have the
  unit test provide a **stub** before including, or pass the metric in as a parameter.
- When logic is tangled in a method or a file full of globals, **extract the pure core**
  into such a header and have the original delegate to it (it's also just better code).
- `make unit` builds and runs `tests/unit/` with the host `cc` in milliseconds;
  `make test` runs the smoke tier under QEMU (`make test-all` for the full catalog);
  `make check` runs unit + smoke.

The goal state: adding a feature usually adds a **cheap unit test**, not a boot test, and
the e2e suite stays a small, trustworthy set of canaries.
