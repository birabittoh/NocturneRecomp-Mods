// game_symbols mod: a "library mod": no UI, no code of its own to react to
// anything, just published guest addresses other mods can depend on instead
// of re-deriving (or copy-pasting) the same reverse-engineered constants.
//
// Registers every known address in OnCreateDialogs (dispatched before
// OnModuleLaunched, and before any consumer's own OnCreateDialogs runs a
// lazy lookup) via rex::system::ModRegistry, reached through
// ctx->runtime->mod_registry(). Consumers add `requires = "game_symbols"` to
// their own mod.toml (see mods_src/ui_color/mod.toml) so the SDK guarantees
// this mod is enabled and ordered first, instead of relying on convention.
//
// See docs/making-mods.md's "Library mods and the shared registry" section.

#include <rex/system/mod_plugin.h>

#include <rex/runtime.h>
#include <rex/system/mod_registry.h>

namespace {

// Guest addresses of the live Settings -> Accent Color struct (three
// consecutive big-endian uint32 fields, R/G/B, each 0-15)
constexpr uint32_t kAccentAddrVanilla = 0x83173CC8u;
constexpr uint32_t kAccentAddrTU = 0x83173A88u;

// Guest address of the live screen-stretch viewport rect: four consecutive
// big-endian uint32 fields (x offset, y offset, width, height). Found via
// the min/max boundary-matrix memory scan documented in the project's
// "live-memory-reverse-engineering-technique" note. Width ranges
// 1052-1280, height 667-720 (this game renders at 1280x720); x/y offset
// are derived by the game as (1280 - width) and (720 - height); the rect
// is anchored to the bottom-right corner, not centered. This setting is
// NOT save-file-persisted; it resets to some default every launch.
constexpr uint32_t kScreenStretchRectAddrVanilla = 0x82882C68u;

// Guest address of the graphics-style *menu selection*, in the same
// Settings screen as the screen-stretch rect above: a single big-endian
// uint32, 0 = "Original", 1 = "Enhanced".
constexpr uint32_t kGraphicsStyleMenuAddrVanilla = 0x828B04A4u;

// Guest address of the *applied* graphics-style flag, 4040 bytes after the
// menu-selection address above: 0 = "Original", 2 = "Enhanced" (not a plain
// bool). Read-only from a mod's perspective; writing it does not render
// correctly. See mods_src/graphics_settings for the read-only display.
constexpr uint32_t kGraphicsStyleAddrVanilla = 0x828B146Cu;

// Guest address near the graphics-style fields above; idle value 0xFFFFFFFF,
// pulses briefly to 8 when the Settings menu selection changes.
constexpr uint32_t kGraphicsStyleTriggerAddrVanilla = 0x82883038u;

// Guest address of the in-game (post-boot) Settings screen's Volume Level, a
// single big-endian uint32, 0-10. Found via live value-transition scan
// (scripts/re/scan_guest_memory.py): scanned for the on-screen value (4),
// changed it to a distinctive value (9) in-game, rescanned, and intersected
// the two hit sets, narrowing from thousands of candidates for each common
// small value down to exactly one address (plus its usual +0x100000000
// heap-alias mirror). Updates live as the value is changed with the d-pad,
// before Accept is pressed (same as the menu-selection fields above).
// Unlike graphics.style_menu/graphics.style, no separate "applied" copy was
// found nearby, so this may be the only backing field, but that isn't
// confirmed. TU offset not yet derived.
constexpr uint32_t kVolumeLevelAddrVanilla = 0x828B0504u;

// Guest address of the live player-stats struct (character screen: HP/Heart/
// MP, STR/CON/INT/LCK, level, exp, gold, kills, rooms, playtime).
// Note: STR/CON/INT/LCK are shown consecutively on the character screen, so
// scanning for that exact 4-value big-endian uint32 sequence turned up
// exactly one hit (plus its usual +0x10000000 heap-alias mirror).
//
// All fields are consecutive big-endian uint32_t, offsets from this address:
//   +0x00 hp_current        73
//   +0x04 hp_max            73
//   +0x08 heart_current     56   (consumed by item use)
//   +0x0c heart_max         56
//   +0x10 mp_current        34
//   +0x14 mp_max            34
//   +0x18 str (copy #1)      8
//   +0x1c con (copy #1)      9
//   +0x20 int (copy #1)      7
//   +0x24 lck (copy #1)      9
//   +0x28..0x34 unidentified (all 0 this session)
//   +0x38 str (copy #2)      8
//   +0x3c con (copy #2)      9
//   +0x40 int (copy #2)      7
//   +0x44 lck (copy #2)      9
//   +0x48 level              4
//   +0x4c exp               686
//   +0x50 gold              429
//   +0x54 kills              65
//   +0x58..0x6c unidentified (2, 5, 19, 26, 0, 48 this session; plausibly
//                             equipped item/slot indices, not confirmed)
//   +0x74 NOT rooms (disproven, see below), read as 57 this session
//   +0x78 unidentified, also 57 this session (duplicate or coincidence)
//   +0x7c..0x8c unidentified (0, 2, 0, 0, 0 this session)
//   +0x90 playtime_hours      0
//   +0x94 playtime_minutes   22
//   +0x98 playtime_seconds   44
//   +0x9c playtime_frames    20  (sub-second; not shown by the HUD's 00:22:44)
//
// The two STR/CON/INT/LCK copies were identical this session (no equipped
// stat bonus active), so which one is "base" vs. "equipped/effective" isn't
// disambiguated yet; re-test by equipping a stat-boosting item and seeing
// which copy moves before a mod depends on telling them apart.
//
// NOT found in this struct: "NEXT" (exp remaining to next level, shown on
// the same screen) is presumably computed from a level/exp table rather
// than stored; "STATUS" (ailment indicator, e.g. "GOOD") is presumably
// tracked on a separate actor-state struct. Both need a follow-up
// investigation before a mod can expose them.
//
// +0x74 was originally labeled "rooms" because it happened to read 57 (the
// on-screen ROOMS value) in the first session. Disproven live in a later
// session via mods_src/player_stats's edit mode: writing to +0x74 did NOT
// move the on-screen ROOMS counter at all, but instead caused STR/CON/INT/
// LCK to jump around (+5 at one value, reset to 0 two values later, +1 two
// values after that), behavior consistent with indexing into some
// stat-growth/level table (plausibly the *real* level value that drives
// stat computation, as opposed to +0x48 above which may just be a display
// cache), not a simple incrementing room counter. Do NOT treat +0x74 as
// "rooms" or write to it from a mod until it's properly re-identified;
// the real rooms-visited counter is registered separately below, at a
// completely different address (not part of this struct at all).
constexpr uint32_t kPlayerStatsAddrVanilla = 0x83174B7Cu;

// TU address for the same struct: found the same way (scanned the TU
// process for the STR/CON/INT/LCK sequence), confirmed by re-reading the
// full struct at the candidate address and matching every field above
// exactly (playtime differed slightly, as expected for a separately-running
// session; everything else, including the unidentified/duplicate fields,
// matched). The delta from the vanilla address (0x240) is exactly the same
// vanilla/TU offset as kAccentAddrVanilla/kAccentAddrTU above, which is a
// good independent sanity check that this is the right address rather than
// a coincidental match.
constexpr uint32_t kPlayerStatsAddrTU = 0x8317493Cu;

// Guest address of the real rooms-visited counter, a single big-endian
// uint32_t living well outside the kPlayerStatsAddrVanilla struct above,
// found via the min/max boundary-matrix technique's sibling, an exact value-
// transition scan (scripts/re/scan_guest_memory.py in NocturneRecomp):
// snapshotted every host address holding the on-screen ROOMS value (57),
// asked the user to walk into one new room (ROOMS -> 58), rescanned, and
// intersected the two sets. Collapsed to exactly 2 candidates (this address
// plus its usual +0x10000000 heap-alias mirror); confirmed live, matching
// the user's on-screen ROOMS exactly across 57, 58, and 59 in sequence.
//
// CORRECTION: this address was originally published as 0x83164CD0 (see git
// history), derived the same way against an earlier vanilla session. That
// value went stale (reads 0 in later sessions/builds, reported via
// player_stats/room_presence in NocturneRecomp both showing ROOMS 0 with a
// real save at 57). Re-running the scan against a live vanilla process found
// the counter at 0x83164F10 instead (0x240 higher); re-running it again
// against a NOCTURNE_TU build found it at exactly the *original* 0x83164CD0,
// i.e. the address first published as "vanilla" was actually the TU one,
// mislabeled, and 0x83164F10 is the real vanilla address. The 0x240 delta
// between them matches the vanilla/TU delta used everywhere else in this
// file (see kAccentAddrVanilla/TU, kPlayerStatsAddrVanilla/TU), which is
// what caught the mislabeling.
constexpr uint32_t kRoomsAddrVanilla = 0x83164F10u;
constexpr uint32_t kRoomsAddrTU = 0x83164CD0u;

// Guest address of the level -> cumulative-exp-required table (indexed by
// level, 4-byte big-endian entries), e.g. dword_82E440E4[level]. Found via
// IDA (default.xex, imagebase 0x82000000): the character screen's stats
// dump (sub_82243EC0) computes the on-screen "NEXT" field as
// `dword_82E440E4[level] - exp` (clamped to 0 at level 99), which is exactly
// the "exp to next level" TODO noted above kPlayerStatsAddrVanilla. Also
// consumed by the real level-up check below. Vanilla only so far; not yet
// re-derived for TU.
constexpr uint32_t kExpToLevelTableAddrVanilla = 0x82E440E4u;

// Guest address of the actual level-up function (found via IDA: the only
// xref to both dword_83174BC4 (level, +0x48) and dword_83174BC8 (exp, +0x4c)
// besides the character-screen renderer above). NOT a dedicated "level up"
// entry point; it's a small per-tick status function that also counts
// down a few unrelated timers (dword_82E80F40/44/4C) before falling through
// to the level-up check, so it's safe to call repeatedly/out of its natural
// cadence (worst case: an unrelated timer ticks down one extra frame).
//
// The level-up check itself is `level == 99 || table[level] > exp`: if
// false, it increments level, rolls HP/MP max growth and a random subset of
// STR/CON/INT/LCK (at least one stat, usually two), then recurses on itself
// (return value only feeds the next stat roll's RNG draw), so if exp
// clears more than one level's threshold, it cascades through every level
// in a single call, matching vanilla's real multi-level-up-on-huge-exp
// behavior. Calling this after writing exp to at least
// kExpToLevelTableAddrVanilla[level] triggers exactly one real, in-game
// level-up (not a fabricated stat write); see mods_src/player_stats's
// "Level Up" button. Vanilla only so far; not yet re-derived for TU.
constexpr uint32_t kLevelUpTickFnAddrVanilla = 0x82256CC0u;

// Guest global holding a POINTER to the Application/framework singleton
// object (the `a1` passed to the game's fwmain mode loop / fixed-timestep
// tick, sub_8258B8A0 / sub_8258B3B8). On the pointed-to object: +2236 =
// game_time, +2232 = target_time (the fixed-timestep catch-up loop's own
// clock, units "300ths of a second"; one 60Hz frame = 5 units). Verified
// against assets/default.xex (imagebase 0x82000000) and live-probed.
//
// Consumers must not trust this blindly: dereference it, then sanity-check
// the pointed-to struct (see src/fast_forward.cpp's plausibility guard)
// before reading/writing game_time/target_time through it, in case this
// ever runs against a build where the address doesn't hold what's expected.
constexpr uint32_t kAppSingletonPtrAddrVanilla = 0x82E4F808u;

// TU address: the TU relocates the image's .data statics by -0x240 (same
// delta as kAccentAddrVanilla/TU and kPlayerStatsAddrVanilla/TU above).
// Derived from TU codegen output, not guessed from the delta: the TU build
// of the Live-logon screen's Update (TU sub_825B6CB0, vanilla sub_825B6AB0)
// loads this global as lis 0x82E50000 + offset -2616 = 0x82E4F5C8, where
// vanilla uses offset -2040 = 0x82E4F808. See scripts/match_tu_functions.py
// for the vanilla->TU function-matching workflow.
constexpr uint32_t kAppSingletonPtrAddrTU = 0x82E4F5C8u;

// Guest address of the game-clear flag: OR of all save slots' clear flags.
// Non-zero = game has been cleared at least once, which is a prerequisite for
// Richter mode (sub_82395868 checks this alongside the player name being
// "richter "). Address found via the save-load path: sub_8238FC38 resets it
// to 0 (case 0), then sub_8238FAC8 ORs in each slot's save offset 0x124.
constexpr uint32_t kGameClearFlagAddrVanilla = 0x83133AFCu;
constexpr uint32_t kGameClearFlagAddrTU = 0x831338BCu;  // -0x240 TU delta

// Guest address of the driver function behind XSessionWriteStats (XGI
// 0xB0025), the game's leaderboard-score write path; see
// docs/leaderboard-write-path-xsessionwritestats.md (or the equivalent
// memory note) for how this was found. Registered here (rather than a
// data address) so mods_src/function_override_demo can look it up by name
// and pass it to rex::runtime::FunctionDispatcher::OverrideFunction;
// see docs/making-mods.md's "Overriding a recompiled function" section.
// Vanilla only so far; not yet confirmed in the TU build.
constexpr uint32_t kLeaderboardWriteStatsFnAddrVanilla = 0x8257CD48u;

// Guest address of the widget "set text by localized-string-id" helper:
// looks up the string via the shared string table (dword_82E61854, indexed
// by id, UTF-16BE null-terminated entries, confirmed live via a scan of
// that table's contents, e.g. id 1 = "DEFAULT", id 9 = "SELECT", id 278 =
// "GRAPHICS: ENHANCED"), then COPIES (not just points to) the string bytes
// into the widget's own buffer at [a1+572] (up to 2048 bytes, via
// sub_825E5D30). This copy-not-reference behavior is why editing the shared
// string table's entry at runtime has no visible effect on any widget that
// already called this for that id, confirmed live (see
// mods_src/resolution_preset_native's button-prompt relabeling, which hooks
// this function directly instead). Registered as a function (not a data
// address) so a mod can wrap it to intercept/rewrite specific ids' text
// after the vanilla copy runs.
//
// TU address re-derived from NocturneRecomp's own port of this same function
// (src/graphics_settings.cpp's kSetWidgetTextByIdFnAddr) via
// scripts/match_tu_functions.py: +0x200, matching the bodies exactly.
constexpr uint32_t kSetWidgetTextByIdFnAddrVanilla = 0x825CFC68u;
constexpr uint32_t kSetWidgetTextByIdFnAddrTU = 0x825CFE68u;

// Guest address of the in-game (post-boot) options-menu message dispatcher:
// handles Accept/Cancel input for the Graphics/Volume Level/Change Screen
// Size... menu, reading the highlighted row index from a nested cursor
// widget at [a1+544]+536 and using it to index a row-tag table at
// a1+556 (one int per row). Confirmed via IDA: tag 2 pushes a screen
// transition to id 11 (sub_825BA678, the "Change Screen Size..." live-
// stretch screen already published as graphics.stretch_rect's consumer),
// and tag 3 is an inline toggle writing the graphics-style mirror byte at
// data_ptr+4548 (data_ptr = app.singleton_ptr chain, same byte
// mods_src/graphics_settings's SetGraphicsStyle writes). Registered here
// (rather than a data address) for mods_src/function_override_demo-style
// consumers to pass to FunctionDispatcher::OverrideFunction. Vanilla only
// so far; not yet confirmed in the TU build.
constexpr uint32_t kOptionsMenuDispatcherFnAddrVanilla = 0x825B2F18u;

// Guest address of the "Change Screen Size..." live-stretch screen's own
// message handler (screen id 11, reached from the options menu dispatcher
// above). Fully decompiled: on-open (msg 0/4) seeds a1+560 (X, percent
// 1-50) and a1+564 (Y, percent 1-30) from the current save slot; d-pad
// input (msg 11/0..3) adjusts those two fields (clamped to the same
// ranges) and calls the recompute/redraw chain below; on-close (msg 0/5)
// writes them back to the save slot. Registered so a mod can hook it to
// track when the screen is open/closed and to drive the same percent
// fields the game's own d-pad handler drives, instead of writing the
// derived LTRB rect directly (see mods_src/graphics_settings, which has to
// fight the game's own per-frame re-derivation because it writes LTRB).
//
// TU address re-derived via scripts/match_tu_functions.py (+0x200), same
// technique as kSetWidgetTextByIdFnAddrTU above.
constexpr uint32_t kChangeScreenSizeFnAddrVanilla = 0x825BA678u;
constexpr uint32_t kChangeScreenSizeFnAddrTU = 0x825BA878u;

// Guest addresses of the two "recompute the LTRB stretch rect from the
// percent fields" functions the stretch screen's own d-pad handler calls
// after adjusting a1+560/a1+564, and the redraw/commit function called
// after that. Signatures not independently confirmed beyond the calling
// pattern seen in kChangeScreenSizeFnAddrVanilla's decompile (single `a1`
// screen-instance pointer argument assumed, matching the surrounding
// code's convention), so a mod calling these directly should treat that as
// unverified until it's been exercised live.
constexpr uint32_t kStretchRecomputeFn1AddrVanilla = 0x825AE898u;
constexpr uint32_t kStretchRecomputeFn2AddrVanilla = 0x825AEF30u;
constexpr uint32_t kStretchRedrawFnAddrVanilla = 0x825D1410u;

// Guest address of the ACTUAL percent-to-rect converter for the "Change
// Screen Size..." screen (sub_825BB2B0), NOT
// menu.stretch_recompute_fn1/fn2 above, which decompilation revealed are a
// no-arg getter and a widget-repositioning walk, respectively, neither of
// which touches the LEFT/TOP/RIGHT/BOTTOM rect at all. This is the one
// that does: takes a single `a1` (screen instance pointer, same as the
// message handler's own a1), reads/clamps [a1+560] to [1,50] and [a1+564]
// to [1,30] in place, interpolates the LTRB rect between the min
// (graphics.stretch_rect... no, the OVERSCAN box at 0x82882CC8/CC/D0/D4)
// and max (0x82882C98/9C/A0/A4, = {0,0,W,H}) bounds, writes the result to
// the live rect at graphics.stretch_rect (0x82882C68 etc), and also calls
// sub_825AB2B0 (repositions the on-screen stretch-adjustment arrow
// widgets) with the correct arguments itself, so calling this function
// is safe even though sub_825AB2B0's own calling convention was never
// independently confirmed, since this caller already knows it.
//
// Confirmed live: the d-pad message path (word0=11) updates [a1+560]/
// [a1+564] but does NOT call this function as part of handling that
// message; a mod driving those fields directly (bypassing real d-pad
// messages) must call this explicitly afterward, or the live rect/on-screen
// preview never updates to match.
//
// TU address re-derived via scripts/match_tu_functions.py (+0x200).
constexpr uint32_t kStretchPercentToRectFnAddrVanilla = 0x825BB2B0u;
constexpr uint32_t kStretchPercentToRectFnAddrTU = 0x825BB4B0u;

// Guest address of the stretch screen's on-screen arrow-widget repositioner
// (sub_825AB2B0), called by kStretchPercentToRectFnAddrVanilla with
// (value-of-global-dword_82E7A02C, X-percent, Y-percent), confirmed via
// that caller's own decompile, not guessed independently. Registered
// separately so a mod replacing the percent-to-rect converter (to remove
// its [1,50]/[1,30] clamp; see that address's comment) can still call
// this with the same arguments the original would have used.
//
// TU address re-derived via scripts/match_tu_functions.py (+0x200).
constexpr uint32_t kStretchWidgetRepositionFnAddrVanilla = 0x825AB2B0u;
constexpr uint32_t kStretchWidgetRepositionFnAddrTU = 0x825AB4B0u;

// Guest address of the global sub_825AB2B0's first argument is read from
// (not the function's own address; the VALUE stored here is passed as
// that arg). Likely a shared UI-transition-manager instance, based on
// other callers in the binary passing the same global's value around.
//
// TU address re-derived by finding the vanilla guest function
// (sub_825ABED0, the UI manager's own init) that references this global,
// matching it to its TU counterpart, and comparing the load/store offset
// literal at the same body line in both a vanilla and a --tu codegen tree
// (nocturnerecomp's own port of this global,
// src/graphics_settings.cpp's kUiTransitionManagerAddr, documents the full
// technique). Delta -0x240, the same delta seen for
// kAccentAddrVanilla/TU, kPlayerStatsAddrVanilla/TU and
// kAppSingletonPtrAddrVanilla/TU above, confirmed live against a running
// --tu process (scripts/re/scan_guest_memory.py) reading a plausible heap
// pointer, cross-checked against the static line-match so a coincidentally
// plausible-looking-but-wrong pointer at a nearby address couldn't fool it
// (this happened once during derivation: the vanilla address itself
// happened to read a plausible pointer too, purely by chance).
constexpr uint32_t kUiTransitionManagerAddrVanilla = 0x82E7A02Cu;
constexpr uint32_t kUiTransitionManagerAddrTU = 0x82E79DECu;

// Guest addresses of the two bounding rects kStretchPercentToRectFnAddrVanilla
// interpolates graphics.stretch_rect between: the full-screen bound (LTRB,
// = {0,0,W,H}) and the ~7.5% overscan-safe bound (LTRB). Confirmed via that
// function's own decompile. A mod that removes its [1,50]/[1,30] clamp (to
// let presets exceed the native slider's range) still needs these two to
// replicate its interpolation formula.
//
// Confirmed UNMOVED under the TU (same address as vanilla), verified both
// by reading back their exact expected content ({0,0,1280,720} max,
// {232,54,1048,666} min) from a live --tu process, and statically, by
// checking that kStretchPercentToRectFnAddrTU's own recompiled body resolves
// the same absolute addresses at the same lines as the vanilla body. Not
// every .data address in this part of the image moved; see
// kUiTransitionManagerAddrTU above, which lives nearby and DID move by
// -0x240, so this isn't a "whole region is static" result, just these two
// structs specifically.
constexpr uint32_t kStretchRectMaxAddrVanilla = 0x82882C98u;
constexpr uint32_t kStretchRectMinAddrVanilla = 0x82882CC8u;

// Guest address of the per-frame render conversion (reads the graphics-style
// applied-flag entry byte every frame; see graphics.style's comment above
// and mods_src/graphics_settings's SetGraphicsStyle). Registered so a mod
// can wrap it purely to get a live PPCContext on a guaranteed-every-frame
// guest call, e.g. to safely invoke other guest functions without waiting
// for a real player input event to supply one.
//
// CAVEAT (found live, not from decompilation): this turned out to only run
// during actual 3D gameplay rendering, not while a UI screen like Settings
// is open, so a mod hooking it for "every frame" cadence while in a menu
// will see it not fire at all until gameplay resumes. Use
// app.fixed_timestep_tick_fn below instead for a menu-agnostic anchor.
constexpr uint32_t kPerFrameRenderConversionFnAddrVanilla = 0x824FB460u;

// Guest address of the fixed-timestep catch-up tick (sub_8258B3B8) called
// repeatedly from inside the main loop (sub_8258B8A0, which itself never
// returns; do not hook that one directly, it wraps the whole game).
// Takes a single argument: the app singleton object itself (i.e. the value
// pointed to by app.singleton_ptr, already dereferenced, matching the
// +2232/+2236 game_time/target_time fields documented on
// kAppSingletonPtrAddrVanilla above, which this function reads/writes).
// Unlike the per-frame render conversion above, this keeps running
// regardless of which UI screen is showing (confirmed live: it's what
// finally picked up a queued resolution-preset cycle while sitting idle in
// the Settings > Change Screen Size screen, where the render-conversion
// function never fired at all). Use this as the "guaranteed every frame"
// anchor for hooks that need a live PPCContext without waiting for player
// input.
//
// TU address re-derived via scripts/match_tu_functions.py: +0x1F8, NOT the
// +0x200 seen for most functions in this address range, a reminder that
// the TU's code shift is regional and grows gradually rather than jumping to
// a fixed constant everywhere at once (confirmed correct by eyeballing the
// matched bodies: identical instruction sequences, just relocated).
constexpr uint32_t kFixedTimestepTickFnAddrVanilla = 0x8258B3B8u;
constexpr uint32_t kFixedTimestepTickFnAddrTU = 0x8258B5B0u;

// ---------------------------------------------------------------------------
// Settings screen (native options list) infrastructure
// ---------------------------------------------------------------------------
//
// Everything below was reverse-engineered for NocturneRecomp's
// src/native_options.cpp (appends Resolution/Fullscreen/Language/GPU Backend
// rows to the in-game settings screen) and src/graphics_settings.cpp (the
// Preset row + stretch-screen hooks); see those files for the fuller usage
// context. All TU addresses were re-derived the same way: match the vanilla
// guest function referencing each address to its TU counterpart with
// scripts/match_tu_functions.py, then either take the matched function's own
// (+delta) entry point directly, or (for plain .data addresses, which that
// script can't find on its own) compare the load/store offset literal at
// the same body line between a vanilla and a --tu codegen tree. Every TU data
// address was also confirmed live against a running --tu process with
// scripts/re/scan_guest_memory.py.

// Generic guest heap allocator: (size) -> pointer. Used throughout the
// front-end to allocate widgets/buffers; sub_82576710 is what actually backs
// it, but this is the outer wrapper every caller uses.
constexpr uint32_t kAllocFnAddrVanilla = 0x82576950u;
constexpr uint32_t kAllocFnAddrTU = 0x82576B28u;  // +0x1D8 (match_tu_functions.py)

// Generic UI text-widget constructor: (memory, parent) -> widget. `memory`
// is a kTextWidgetSize=4668-byte block from menu.alloc_fn; `parent` puts the
// widget in that parent's draw list.
constexpr uint32_t kTextWidgetCtorFnAddrVanilla = 0x825CEDA8u;
constexpr uint32_t kTextWidgetCtorFnAddrTU = 0x825CEFA8u;  // +0x200

// Sets a widget's text from a literal UTF-16BE guest string (as opposed to
// menu.set_widget_text_by_id_fn's string-table lookup): (widget, utf16_ptr).
// Copies the string into the widget's own buffer immediately, so the source
// buffer can be reused right after the call. Leaves the widget's text scale
// at its construction default; pair with
// menu.settings_text_widget_set_colour_fn below or set the scale explicitly
// if it needs to match a stock row.
constexpr uint32_t kSetTextWidgetLiteralFnAddrVanilla = 0x825CEE40u;
constexpr uint32_t kSetTextWidgetLiteralFnAddrTU = 0x825CF040u;  // +0x200

// Measures a text widget's rendered width in pixels: (widget) -> pixels.
// Useful for centering/laying out text built from menu.set_text_widget_literal_fn.
constexpr uint32_t kTextWidgetWidthFnAddrVanilla = 0x825CF008u;
constexpr uint32_t kTextWidgetWidthFnAddrTU = 0x825CF208u;  // +0x200

// Sets a text widget's colour: (widget, argb). NOT the same function as
// menu.option_list_set_widget_colour_fn below; this one operates on the
// plain text widgets menu.text_widget_ctor_fn builds, that one on option-list
// row widgets specifically. Confirmed distinct: different guest addresses,
// found via different call sites.
constexpr uint32_t kSetTextWidgetColourFnAddrVanilla = 0x825CF000u;
constexpr uint32_t kSetTextWidgetColourFnAddrTU = 0x825CF200u;  // +0x200

// The four functions shared by every "option list" (spinner-row list) widget
// in the game: the in-game settings screen's Graphics/Volume/Change Screen
// Size... rows, and the separate Controls screen's rows, both use the same
// underlying list class. A mod hooking these must filter by list instance
// (compare against the specific list pointer it cares about) since these
// fire for every option list in the game, not just one screen's.
//
//   option_list_setup_fn(list, x, y, count, spacing, ...)
//     Creates `count` rows: a label widget (list+648+4*i) and a value widget
//     (list+760+4*i) each, marks each row cyclable (byte at list+584+i).
//     Room for up to 16 rows.
//   option_list_bind_rows_fn(list, entries, count)
//     Binds an array of 96-byte setting entries (see the layout note below)
//     and renders each row's current value into its value widget.
//   option_list_cycle_row_fn(list, direction)
//     Left/right on the selected row: `entry.value += entry.step * direction`,
//     wraps at [entry.min, entry.max], repaints just that row.
//   option_list_update_fn(list, delta)
//     Per-frame: pulses the selected row's label with a highlight colour,
//     after first resetting the *previous* frame's pulse, but that reset
//     is hardcoded to exactly 3 labels (list+648..+656), so an appended row
//     past index 2 needs its own reset before calling this, or it keeps
//     whatever colour the pulse last left it on.
//
// Each 96-byte setting entry:
//   +0  label string id      +12 min value        +24 display multiplier
//   +4  current value        +16 max value        +32.. inline array of
//   +20 step                                      per-value string ids
//                                                  (all zero => render the
//                                                   number itself)
constexpr uint32_t kOptionListSetupFnAddrVanilla = 0x825D32B0u;
constexpr uint32_t kOptionListSetupFnAddrTU = 0x825D34B0u;  // +0x200
constexpr uint32_t kOptionListBindRowsFnAddrVanilla = 0x825D36B8u;
constexpr uint32_t kOptionListBindRowsFnAddrTU = 0x825D38B8u;  // +0x200
constexpr uint32_t kOptionListCycleRowFnAddrVanilla = 0x825D35C0u;
constexpr uint32_t kOptionListCycleRowFnAddrTU = 0x825D37C0u;  // +0x200
constexpr uint32_t kOptionListUpdateFnAddrVanilla = 0x825D38A8u;
constexpr uint32_t kOptionListUpdateFnAddrTU = 0x825D3AA8u;  // +0x200

// Per-row enable/disable virtuals on an option list: (list, row). Used by the
// in-game settings screen's Activate to grey out Volume/Change-Screen-Size
// until a save is loaded (slot 14, sub_825D4690, to disable; slot 15,
// sub_825D3530 below, to re-enable); see menu.settings_screen_activate_fn.
constexpr uint32_t kOptionListEnableRowFnAddrVanilla = 0x825D3530u;
constexpr uint32_t kOptionListEnableRowFnAddrTU = 0x825D3730u;  // +0x200

// Sets an option-list row widget's colour: (widget, argb). See
// menu.set_text_widget_colour_fn above for the distinct plain-text-widget
// equivalent.
constexpr uint32_t kOptionListSetWidgetColourFnAddrVanilla = 0x825D2138u;
constexpr uint32_t kOptionListSetWidgetColourFnAddrTU = 0x825D2338u;  // +0x200

// Sets an option-list row widget's text from a literal UTF-16BE guest
// string: (widget, utf16_ptr). Leaves the widget's text scale at its
// construction default (noticeably larger than a stock row); pair with
// menu.option_list_set_text_scale_fn to match. Distinct from both
// menu.set_text_widget_literal_fn above (plain text widgets, not option-list
// rows) and menu.option_list_set_widget_text_by_id_fn below (string-table
// lookup, not a literal).
constexpr uint32_t kOptionListSetWidgetTextFnAddrVanilla = 0x825D1EE0u;
constexpr uint32_t kOptionListSetWidgetTextFnAddrTU = 0x825D20E0u;  // +0x200

// Sets an option-list row widget's text scale: (widget, scale), scale passed
// in f1 (not a GPR). The list's own normal row scale is a runtime value (0.75
// by default, set by sub_825D2C48/menu.option_list_setup_fn), not a
// constant; read it live from the list instance rather than hardcode it.
constexpr uint32_t kOptionListSetTextScaleFnAddrVanilla = 0x825D20C0u;
constexpr uint32_t kOptionListSetTextScaleFnAddrTU = 0x825D22C0u;  // +0x200

// Sets an option-list row widget's text by localized-string-id: (widget,
// string_id). Distinct from menu.set_widget_text_by_id_fn above, which
// operates on the stretch screen's "Default" prompt widget, not an
// option-list row, confirmed as two different guest functions (different
// addresses, different call sites), despite doing conceptually the same
// string-table lookup + copy.
constexpr uint32_t kOptionListSetWidgetTextByIdFnAddrVanilla = 0x825D1EE8u;
constexpr uint32_t kOptionListSetWidgetTextByIdFnAddrTU = 0x825D20E8u;  // +0x200

// The in-game settings screen's own message handler, activate hook, and
// per-frame update: (screen, message), (screen, user_index) and (screen,
// delta) respectively. This is the SAME screen object as
// menu.change_screen_size_fn's caller (the Graphics/Volume/Change-Screen-
// Size... list), shared between the main-menu and pause-menu entry points,
// per that screen's own exit-page logic (see app.singleton_ptr's in-game
// latch, +29 from the pointed-to object).
constexpr uint32_t kSettingsScreenEventFnAddrVanilla = 0x825B3F58u;
constexpr uint32_t kSettingsScreenEventFnAddrTU = 0x825B4158u;  // +0x200
constexpr uint32_t kSettingsScreenActivateFnAddrVanilla = 0x825B43E0u;
constexpr uint32_t kSettingsScreenActivateFnAddrTU = 0x825B45E0u;  // +0x200
constexpr uint32_t kSettingsScreenUpdateFnAddrVanilla = 0x825B4CC0u;
constexpr uint32_t kSettingsScreenUpdateFnAddrTU = 0x825B4EC0u;  // +0x200

// The in-game settings screen's own widget builder (constructs its option
// list and its A/B/X prompt-bar widgets). Registered as an address *range*
// (this address to +kSettingsScreenBuildFnSize) rather than a single guest
// function to call, since its main use is recognising a virtual-dispatched
// call's return address as coming from inside this function (there's no
// other way to tell this screen's option list apart from the Controls
// screen's, which is set up identically via the same shared
// menu.option_list_* functions). Size confirmed unchanged under the TU (the
// matched function's own internal label range grows by exactly the +0x200
// delta in both directions, i.e. nothing was inserted inside it).
constexpr uint32_t kSettingsScreenBuildFnAddrVanilla = 0x825B4650u;
constexpr uint32_t kSettingsScreenBuildFnAddrTU = 0x825B4850u;  // +0x200
constexpr uint32_t kSettingsScreenBuildFnSize = 0x4D4u;

// The exact call site (return address) of the front-end watchdog's automatic
// page-switch-back, inside the UI manager's per-frame update (sub_825AAE90,
// TU sub_825AB090): with no game running, this watchdog notices "a page
// switch isn't pending, and we're not on page 0" and forces a switch back to
// the previous screen, which fights any mod trying to keep a screen open
// pre-save-load (e.g. reaching the stretch screen from the main menu). Filter
// a hooked menu.post_event_fn call on this exact LR to suppress just that one
// post, leaving every other page switch (including genuine ones) untouched.
constexpr uint32_t kUiManagerWatchdogPostLrVanilla = 0x825AB030u;
constexpr uint32_t kUiManagerWatchdogPostLrTU = 0x825AB230u;

// Every front-end page-switch goes through this one queue push: (class,
// arg1, page, controller), class=8 for a page switch. Registered as a
// function so a mod can override it to suppress/redirect specific switches
// (see menu.ui_manager_watchdog_post_lr above for the concrete use case),
// while still being able to call the original for every switch it doesn't
// care about.
constexpr uint32_t kPostEventFnAddrVanilla = 0x825CE8E8u;
constexpr uint32_t kPostEventFnAddrTU = 0x825CEAE8u;  // +0x200

// The settings screen's prompt-bar layout: (x, y, width, prompt_x, prompt_a,
// prompt_b): the only place all three stock prompts (X/A/B) are visible at
// once as function arguments; the builder keeps just the A prompt on the
// screen object afterward (+584) and drops the X/B ones as locals. Also
// takes exactly three widgets and spreads them across the bar's width (gaps
// of (width - total)/4), so adding a fourth prompt means re-laying the row
// out by hand after this returns.
constexpr uint32_t kPromptBarLayoutFnAddrVanilla = 0x825CA820u;
constexpr uint32_t kPromptBarLayoutFnAddrTU = 0x825CAA20u;  // +0x200

// A prompt-bar prompt's own constructor: (memory, parent, flag) -> prompt.
// `memory` is a kPromptSize=552-byte block from menu.alloc_fn. A prompt is a
// glyph widget plus a text widget, glyph at +544, text at +548, with the text
// drawn at a fixed offset from the glyph (menu.prompt_text_offset_fn below
// sets it; the stock prompts all use dx=30, dy=5).
constexpr uint32_t kPromptCtorFnAddrVanilla = 0x825D1DB0u;
constexpr uint32_t kPromptCtorFnAddrTU = 0x825D1FB0u;  // +0x200

// Sets a prompt's glyph image: (prompt, image). `image` comes from
// menu.find_image_fn below, looked up against menu.image_bank_ptr.
constexpr uint32_t kPromptSetGlyphFnAddrVanilla = 0x825D1EF8u;
constexpr uint32_t kPromptSetGlyphFnAddrTU = 0x825D20F8u;  // +0x200

// Shows a prompt's glyph: (prompt, flag-unused-pass-0).
constexpr uint32_t kPromptShowGlyphFnAddrVanilla = 0x825D2010u;
constexpr uint32_t kPromptShowGlyphFnAddrTU = 0x825D2210u;  // +0x200

// Sets a prompt's text offset from its glyph: (prompt, dx, dy). Stock prompts
// all use (30, 5).
constexpr uint32_t kPromptTextOffsetFnAddrVanilla = 0x825D2018u;
constexpr uint32_t kPromptTextOffsetFnAddrTU = 0x825D2218u;  // +0x200

// Sets a prompt's screen position: (prompt, x, y).
constexpr uint32_t kPromptSetPosFnAddrVanilla = 0x825D1FA8u;
constexpr uint32_t kPromptSetPosFnAddrTU = 0x825D21A8u;  // +0x200

// Looks up an image by name in an image bank: (image_bank, name) -> image.
// `name` is one entry of the descending-letter glyph-name table (see
// menu.glyph_name_y below); `image_bank` comes from menu.image_bank_ptr.
constexpr uint32_t kFindImageFnAddrVanilla = 0x825CEB68u;
constexpr uint32_t kFindImageFnAddrTU = 0x825CED68u;  // +0x200

// Guest pointer to the image bank the prompt-bar's glyph names (see
// menu.glyph_name_y below) are resolved against with menu.find_image_fn.
constexpr uint32_t kImageBankPtrAddrVanilla = 0x82E7A570u;
constexpr uint32_t kImageBankPtrAddrTU = 0x82E7A330u;  // -0x240

// Prompt-bar text colour, as ARGB: a single guest uint32_t, read live (not a
// compile-time constant on the game's side) by whatever paints prompt text.
// Confirmed UNMOVED under the TU (0xFF2F153E read back live from a running
// --tu process, and the same offset literal resolves at the same body line
// in both a vanilla and --tu codegen tree of the settings screen's own
// builder).
constexpr uint32_t kPromptColourAddrVanilla = 0x82895098u;

// One entry of a descending-letter glyph-name table used to resolve
// controller-button prompt icons via menu.find_image_fn: "Z" at -4 from this
// address, this = "Y", "X" at +4, and so on downward (W, V, U, T, S, R, ...).
// Each entry is a big-endian uint32_t with the ASCII letter code in the top
// byte (e.g. "Y" = 0x59000000), not a null-terminated string. The stock
// prompt bar uses the X, A and B entries.
//
// TU address re-derived by decrypting the vanilla assets/default.xex
// (scripts/re/dump_xex_image.py) to confirm the exact vanilla table layout
// and content, then scanning a live --tu process
// (scripts/re/scan_guest_memory.py) for the same distinctive
// Z/Y/X/W/V/U/T/S byte sequence: 2 hits (the address and its usual
// +0x10000000 heap-alias mirror). Delta +0x30, notably NOT the -0x240 seen
// for the other .data addresses in this file; this table lives in a
// different part of the image (0x8220xxxx vs 0x82E4xxxx-0x82E8xxxx) that
// shifted independently.
constexpr uint32_t kGlyphNameYAddrVanilla = 0x82202274u;
constexpr uint32_t kGlyphNameYAddrTU = 0x822022A4u;

class GameSymbolsMod : public rex::system::IModPlugin {
 public:
  explicit GameSymbolsMod(rex::Runtime* runtime) : runtime_(runtime) {}

  void OnCreateDialogs(rex::ui::ImGuiDrawer* /*drawer*/) override {
    if (runtime_ && runtime_->mod_registry()) {
      runtime_->mod_registry()->RegisterAddress("ui.accent_color", kAccentAddrVanilla,
                                                kAccentAddrTU);
      runtime_->mod_registry()->RegisterAddress("graphics.stretch_rect",
                                                kScreenStretchRectAddrVanilla);
      runtime_->mod_registry()->RegisterAddress("graphics.style",
                                                kGraphicsStyleAddrVanilla);
      runtime_->mod_registry()->RegisterAddress("graphics.style_menu",
                                                kGraphicsStyleMenuAddrVanilla);
      runtime_->mod_registry()->RegisterAddress("graphics.style_trigger",
                                                kGraphicsStyleTriggerAddrVanilla);
      runtime_->mod_registry()->RegisterAddress("audio.volume_level",
                                                kVolumeLevelAddrVanilla);
      runtime_->mod_registry()->RegisterAddress("player.stats", kPlayerStatsAddrVanilla,
                                                kPlayerStatsAddrTU);
      runtime_->mod_registry()->RegisterAddress("player.rooms", kRoomsAddrVanilla, kRoomsAddrTU);
      runtime_->mod_registry()->RegisterAddress("player.exp_to_level_table",
                                                kExpToLevelTableAddrVanilla);
      runtime_->mod_registry()->RegisterAddress("player.level_up_tick_fn",
                                                kLevelUpTickFnAddrVanilla);
      runtime_->mod_registry()->RegisterAddress("app.singleton_ptr",
                                                kAppSingletonPtrAddrVanilla,
                                                kAppSingletonPtrAddrTU);
      runtime_->mod_registry()->RegisterAddress("game.clear_flag",
                                                kGameClearFlagAddrVanilla,
                                                kGameClearFlagAddrTU);
      runtime_->mod_registry()->RegisterAddress("leaderboard.write_stats_fn",
                                                kLeaderboardWriteStatsFnAddrVanilla);
      runtime_->mod_registry()->RegisterAddress("menu.set_widget_text_by_id_fn",
                                                kSetWidgetTextByIdFnAddrVanilla,
                                                kSetWidgetTextByIdFnAddrTU);
      runtime_->mod_registry()->RegisterAddress("menu.options_dispatcher_fn",
                                                kOptionsMenuDispatcherFnAddrVanilla);
      runtime_->mod_registry()->RegisterAddress("menu.change_screen_size_fn",
                                                kChangeScreenSizeFnAddrVanilla,
                                                kChangeScreenSizeFnAddrTU);
      runtime_->mod_registry()->RegisterAddress("menu.stretch_recompute_fn1",
                                                kStretchRecomputeFn1AddrVanilla);
      runtime_->mod_registry()->RegisterAddress("menu.stretch_recompute_fn2",
                                                kStretchRecomputeFn2AddrVanilla);
      runtime_->mod_registry()->RegisterAddress("menu.stretch_redraw_fn",
                                                kStretchRedrawFnAddrVanilla);
      runtime_->mod_registry()->RegisterAddress("menu.stretch_percent_to_rect_fn",
                                                kStretchPercentToRectFnAddrVanilla,
                                                kStretchPercentToRectFnAddrTU);
      runtime_->mod_registry()->RegisterAddress("menu.stretch_widget_reposition_fn",
                                                kStretchWidgetRepositionFnAddrVanilla,
                                                kStretchWidgetRepositionFnAddrTU);
      runtime_->mod_registry()->RegisterAddress("menu.ui_transition_manager",
                                                kUiTransitionManagerAddrVanilla,
                                                kUiTransitionManagerAddrTU);
      runtime_->mod_registry()->RegisterAddress("graphics.stretch_rect_max",
                                                kStretchRectMaxAddrVanilla);
      runtime_->mod_registry()->RegisterAddress("graphics.stretch_rect_min",
                                                kStretchRectMinAddrVanilla);
      runtime_->mod_registry()->RegisterAddress("render.per_frame_conversion_fn",
                                                kPerFrameRenderConversionFnAddrVanilla);
      runtime_->mod_registry()->RegisterAddress("app.fixed_timestep_tick_fn",
                                                kFixedTimestepTickFnAddrVanilla,
                                                kFixedTimestepTickFnAddrTU);

      runtime_->mod_registry()->RegisterAddress("menu.alloc_fn", kAllocFnAddrVanilla,
                                                kAllocFnAddrTU);
      runtime_->mod_registry()->RegisterAddress("menu.text_widget_ctor_fn",
                                                kTextWidgetCtorFnAddrVanilla,
                                                kTextWidgetCtorFnAddrTU);
      runtime_->mod_registry()->RegisterAddress("menu.set_text_widget_literal_fn",
                                                kSetTextWidgetLiteralFnAddrVanilla,
                                                kSetTextWidgetLiteralFnAddrTU);
      runtime_->mod_registry()->RegisterAddress("menu.text_widget_width_fn",
                                                kTextWidgetWidthFnAddrVanilla,
                                                kTextWidgetWidthFnAddrTU);
      runtime_->mod_registry()->RegisterAddress("menu.set_text_widget_colour_fn",
                                                kSetTextWidgetColourFnAddrVanilla,
                                                kSetTextWidgetColourFnAddrTU);
      runtime_->mod_registry()->RegisterAddress("menu.option_list_setup_fn",
                                                kOptionListSetupFnAddrVanilla,
                                                kOptionListSetupFnAddrTU);
      runtime_->mod_registry()->RegisterAddress("menu.option_list_bind_rows_fn",
                                                kOptionListBindRowsFnAddrVanilla,
                                                kOptionListBindRowsFnAddrTU);
      runtime_->mod_registry()->RegisterAddress("menu.option_list_cycle_row_fn",
                                                kOptionListCycleRowFnAddrVanilla,
                                                kOptionListCycleRowFnAddrTU);
      runtime_->mod_registry()->RegisterAddress("menu.option_list_update_fn",
                                                kOptionListUpdateFnAddrVanilla,
                                                kOptionListUpdateFnAddrTU);
      runtime_->mod_registry()->RegisterAddress("menu.option_list_enable_row_fn",
                                                kOptionListEnableRowFnAddrVanilla,
                                                kOptionListEnableRowFnAddrTU);
      runtime_->mod_registry()->RegisterAddress("menu.option_list_set_widget_colour_fn",
                                                kOptionListSetWidgetColourFnAddrVanilla,
                                                kOptionListSetWidgetColourFnAddrTU);
      runtime_->mod_registry()->RegisterAddress("menu.option_list_set_widget_text_fn",
                                                kOptionListSetWidgetTextFnAddrVanilla,
                                                kOptionListSetWidgetTextFnAddrTU);
      runtime_->mod_registry()->RegisterAddress("menu.option_list_set_text_scale_fn",
                                                kOptionListSetTextScaleFnAddrVanilla,
                                                kOptionListSetTextScaleFnAddrTU);
      runtime_->mod_registry()->RegisterAddress("menu.option_list_set_widget_text_by_id_fn",
                                                kOptionListSetWidgetTextByIdFnAddrVanilla,
                                                kOptionListSetWidgetTextByIdFnAddrTU);
      runtime_->mod_registry()->RegisterAddress("menu.settings_screen_event_fn",
                                                kSettingsScreenEventFnAddrVanilla,
                                                kSettingsScreenEventFnAddrTU);
      runtime_->mod_registry()->RegisterAddress("menu.settings_screen_activate_fn",
                                                kSettingsScreenActivateFnAddrVanilla,
                                                kSettingsScreenActivateFnAddrTU);
      runtime_->mod_registry()->RegisterAddress("menu.settings_screen_update_fn",
                                                kSettingsScreenUpdateFnAddrVanilla,
                                                kSettingsScreenUpdateFnAddrTU);
      runtime_->mod_registry()->RegisterAddress("menu.settings_screen_build_fn",
                                                kSettingsScreenBuildFnAddrVanilla,
                                                kSettingsScreenBuildFnAddrTU);
      runtime_->mod_registry()->RegisterAddress("menu.ui_manager_watchdog_post_lr",
                                                kUiManagerWatchdogPostLrVanilla,
                                                kUiManagerWatchdogPostLrTU);
      runtime_->mod_registry()->RegisterAddress("menu.post_event_fn", kPostEventFnAddrVanilla,
                                                kPostEventFnAddrTU);
      runtime_->mod_registry()->RegisterAddress("menu.prompt_bar_layout_fn",
                                                kPromptBarLayoutFnAddrVanilla,
                                                kPromptBarLayoutFnAddrTU);
      runtime_->mod_registry()->RegisterAddress("menu.prompt_ctor_fn", kPromptCtorFnAddrVanilla,
                                                kPromptCtorFnAddrTU);
      runtime_->mod_registry()->RegisterAddress("menu.prompt_set_glyph_fn",
                                                kPromptSetGlyphFnAddrVanilla,
                                                kPromptSetGlyphFnAddrTU);
      runtime_->mod_registry()->RegisterAddress("menu.prompt_show_glyph_fn",
                                                kPromptShowGlyphFnAddrVanilla,
                                                kPromptShowGlyphFnAddrTU);
      runtime_->mod_registry()->RegisterAddress("menu.prompt_text_offset_fn",
                                                kPromptTextOffsetFnAddrVanilla,
                                                kPromptTextOffsetFnAddrTU);
      runtime_->mod_registry()->RegisterAddress("menu.prompt_set_pos_fn",
                                                kPromptSetPosFnAddrVanilla,
                                                kPromptSetPosFnAddrTU);
      runtime_->mod_registry()->RegisterAddress("menu.find_image_fn", kFindImageFnAddrVanilla,
                                                kFindImageFnAddrTU);
      runtime_->mod_registry()->RegisterAddress("menu.image_bank_ptr", kImageBankPtrAddrVanilla,
                                                kImageBankPtrAddrTU);
      runtime_->mod_registry()->RegisterAddress("menu.prompt_colour", kPromptColourAddrVanilla);
      runtime_->mod_registry()->RegisterAddress("menu.glyph_name_y", kGlyphNameYAddrVanilla,
                                                kGlyphNameYAddrTU);
    }
  }

 private:
  rex::Runtime* runtime_ = nullptr;
};

}  // namespace

extern "C" REX_MOD_PLUGIN_EXPORT uint32_t rex_mod_abi_version(void) {
  return rex::system::kModPluginAbiVersion;
}

extern "C" REX_MOD_PLUGIN_EXPORT rex::system::IModPlugin* rex_mod_create(
    uint32_t abi_version, const rex::system::ModHostContext* ctx) {
  if (abi_version != rex::system::kModPluginAbiVersion || !ctx) {
    return nullptr;
  }
  return new GameSymbolsMod(ctx->runtime);
}
