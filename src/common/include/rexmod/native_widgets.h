#pragma once
// Shared helpers for mods that want to draw using the game's own native UI
// widgets (plain text labels and prompt/glyph "sprite" icons) instead of
// (or alongside) an ImGui overlay, so the result is parented to a live guest
// screen and rendered in the game's own font/scale, indistinguishable from
// anything the base game draws itself. Generalizes the private CallGuest/
// StagePresetName/EnsurePresetLabel helpers NocturneRecomp's own
// src/graphics_settings.cpp uses for its preset-name toast, and the
// CreateCustomPrompt helper src/native_options.cpp uses for its Preset row's
// glyph, so mods don't have to re-derive that plumbing from scratch.
//
// Every guest function this header calls is looked up by name from the
// shared mod registry (see mods_src/game_symbols), never hardcoded; resolve
// once per session (e.g. in OnModuleLaunched) via ResolveTextWidgetFns/
// ResolvePromptGlyphFns below, and reuse the resolved struct for the rest of
// it. Requires `requires = "game_symbols"` in the calling mod's mod.toml,
// same as any other FindAddress consumer (see docs/making-mods.md).
//
// ## The PPCContext requirement
//
// Every function below takes a `PPCContext& ctx` that must be *live*, i.e.
// control must currently be inside a guest thread's call stack. Two places
// that is NOT true, despite looking like they might be:
//
//   - A mod's RegisterTick callback: runs on the command-processor thread at
//     GPU swap, with no guest context at all (see ModRegistry::RegisterTick's
//     own doc comment).
//   - Any ImGui overlay callback (OnDraw, a button's if-block, etc.): runs on
//     the UI thread, likewise no guest context.
//
// The established way to get a live one is to override some guest function
// that runs every frame *on the guest thread*, which is exactly what
// src/graphics_settings.cpp does in NocturneRecomp, purely to have a valid
// ctx/base pair handy every frame (see its GraphicsSettings_PerFrame, hooked
// onto the address published as "app.fixed_timestep_tick_fn"). A mod wanting
// to spawn/update a widget from its own per-frame logic can do the same:
// FindAddress("app.fixed_timestep_tick_fn"), OverrideFunction it, and call
// through to the previous handler so the base game's own per-frame work
// still happens.
//
// OverrideFunction is exclusive per address (see docs/making-mods.md,
// "Overriding a recompiled function"): only one mod can hold the override
// on "app.fixed_timestep_tick_fn" at a time. A mod that already hooks some
// other frequently-called guest function (a screen's own event handler, a
// gameplay tick, etc.) can just as well use the ctx it's handed there instead
// of contending for that specific address.
//
// ## Lifetime
//
// Every widget spawned here lives as long as its parent `screen` does; the
// game frees the whole tree when the screen itself is torn down (leaving the
// screen without also cleaning these up is exactly what would leak). There is
// no destroy/free call in this header: none of NocturneRecomp's own callers
// need one (their labels/glyphs live exactly as long as the screen that owns
// them), so there was nothing existing to generalize. A mod needing to remove
// a widget before its screen closes needs its own guest-side free path,
// which is out of scope here.

#include <cstdint>
#include <cstring>
#include <string_view>

#include <rex/memory/utils.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/mod_registry.h>

namespace rexmod {

// Calls a guest function with up to four register arguments, saving and
// restoring the caller's context around it. Generalizes the private
// CallGuest() helper NocturneRecomp's graphics_settings.cpp/native_options.cpp
// each keep their own private copy of. `ctx` must be live (see the header
// comment above). Returns r3 on return (every widget-ctor/find-image call
// below returns a guest address or 0 in r3, matching PPC calling convention),
// or 0 if `fn` is null.
inline uint32_t CallGuestFunction(PPCContext& ctx, uint8_t* base, PPCFunc* fn, uint32_t r3 = 0,
                                  uint32_t r4 = 0, uint32_t r5 = 0, uint32_t r6 = 0) {
  if (!fn) {
    return 0;
  }
  PPCContext saved = ctx;
  ctx.r3.u32 = r3;
  ctx.r4.u32 = r4;
  ctx.r5.u32 = r5;
  ctx.r6.u32 = r6;
  fn(ctx, base);
  const uint32_t result = ctx.r3.u32;
  ctx = saved;
  return result;
}

// Some guest setters (e.g. the option-list text-scale setter) take a float in
// f1 rather than an integer in a GPR. Not common enough to fold into
// CallGuestFunction's signature, but frequent enough among prompt/list
// widgets to be worth its own helper rather than inlining the save/restore
// dance at every call site.
inline void CallGuestFunctionF1(PPCContext& ctx, uint8_t* base, PPCFunc* fn, uint32_t r3,
                                double f1) {
  if (!fn) {
    return;
  }
  PPCContext saved = ctx;
  ctx.r3.u32 = r3;
  ctx.f1.f64 = f1;
  fn(ctx, base);
  ctx = saved;
}

// A guest scratch buffer for staging UTF-16 text, shared across however many
// Stage/Spawn calls a mod makes. One instance is enough per mod; the setter
// functions below copy out of it immediately so nothing needs to retain the
// staged text past the call that used it. Zero-valued until first use;
// StageUtf16 allocates it lazily.
struct GuestTextScratch {
  uint32_t address = 0;
  uint32_t capacity_chars = 0;
};

// Widens `text` (plain ASCII; every current caller's content is, and
// non-ASCII would need its own conversion before this) into `scratch`'s guest
// buffer as UTF-16BE, allocating the buffer via `alloc_fn` on first use.
// Returns the buffer's guest address, or 0 if allocation failed. Truncates
// silently at `capacity_chars` (including the NUL terminator) if `text` is
// longer, the same tradeoff NocturneRecomp's own StagePresetName makes
// for its (short, known-length) preset names.
inline uint32_t StageUtf16(PPCContext& ctx, uint8_t* base, PPCFunc* alloc_fn,
                           GuestTextScratch& scratch, std::string_view text,
                           uint32_t capacity_chars = 64) {
  if (!alloc_fn) {
    return 0;
  }
  if (!scratch.address) {
    scratch.address = CallGuestFunction(ctx, base, alloc_fn, capacity_chars * 2);
    if (!scratch.address) {
      return 0;
    }
    scratch.capacity_chars = capacity_chars;
  }
  const uint32_t max_length = scratch.capacity_chars > 0 ? scratch.capacity_chars - 1 : 0;
  uint32_t length = 0;
  while (length < text.size() && length < max_length) {
    rex::memory::store_and_swap<uint16_t>(
        base + scratch.address + length * 2,
        static_cast<uint16_t>(static_cast<unsigned char>(text[length])));
    ++length;
  }
  rex::memory::store_and_swap<uint16_t>(base + scratch.address + length * 2, 0);
  return scratch.address;
}

//=============================================================================
// Text widgets: plain labels, e.g. NocturneRecomp's own preset-name toast
// and persistent height/width readout (see graphics_settings.cpp).
//=============================================================================

// Resolved function pointers for the text-widget group. Resolve once (e.g.
// OnModuleLaunched) with ResolveTextWidgetFns and reuse the result.
struct TextWidgetFns {
  PPCFunc* alloc_fn = nullptr;
  PPCFunc* ctor_fn = nullptr;
  PPCFunc* set_text_fn = nullptr;
  PPCFunc* width_fn = nullptr;        // optional: only needed for TextWidgetWidth
  PPCFunc* set_colour_fn = nullptr;   // optional: only needed to colour a widget

  // False if any function SpawnTextWidget actually needs (alloc/ctor/
  // set_text) failed to resolve; width_fn/set_colour_fn are optional.
  bool IsValid() const { return alloc_fn && ctor_fn && set_text_fn; }
};

// Looks up and resolves the text-widget function group ("menu.alloc_fn",
// "menu.text_widget_ctor_fn", "menu.set_text_widget_literal_fn",
// "menu.text_widget_width_fn", "menu.set_text_widget_colour_fn"; see
// mods_src/game_symbols) from the shared mod registry.
inline TextWidgetFns ResolveTextWidgetFns(rex::Runtime* runtime) {
  TextWidgetFns fns;
  if (!runtime || !runtime->mod_registry() || !runtime->function_dispatcher()) {
    return fns;
  }
  auto* registry = runtime->mod_registry();
  auto* dispatcher = runtime->function_dispatcher();
  auto resolve = [&](const char* name) -> PPCFunc* {
    auto addr = registry->FindAddress(name);
    return addr ? dispatcher->GetFunction(*addr) : nullptr;
  };
  fns.alloc_fn = resolve("menu.alloc_fn");
  fns.ctor_fn = resolve("menu.text_widget_ctor_fn");
  fns.set_text_fn = resolve("menu.set_text_widget_literal_fn");
  fns.width_fn = resolve("menu.text_widget_width_fn");
  fns.set_colour_fn = resolve("menu.set_text_widget_colour_fn");
  return fns;
}

// Allocates a text widget, parents it to `screen` (the screen's own guest
// widget address; that screen must already be constructed/open), and sets
// its text, mirroring the recipe NocturneRecomp's EnsurePresetLabel/
// SetPresetLabelText use for its preset-name toast. `colour` is ARGB (alpha
// in the high byte, e.g. 0xFF000000 for opaque black); pass 0 to leave the
// game's own default. Returns the new widget's guest address, or 0 on
// failure (fns not fully resolved, allocation failed, or `screen` is 0).
//
// Position the returned widget with SetTextWidgetPosition below; this
// doesn't position it itself since callers commonly want to measure it
// first (TextWidgetWidth) to centre it, same as NocturneRecomp's own
// SetPresetLabelText does.
inline uint32_t SpawnTextWidget(PPCContext& ctx, uint8_t* base, const TextWidgetFns& fns,
                                GuestTextScratch& scratch, uint32_t screen, std::string_view text,
                                uint32_t colour = 0) {
  if (!fns.IsValid() || screen == 0) {
    return 0;
  }
  // Matches NocturneRecomp graphics_settings.cpp's kTextWidgetSize: the
  // fixed allocation size the game's own text-widget constructor expects.
  constexpr uint32_t kTextWidgetSize = 4668;
  const uint32_t memory = CallGuestFunction(ctx, base, fns.alloc_fn, kTextWidgetSize);
  if (!memory) {
    return 0;
  }
  const uint32_t widget = CallGuestFunction(ctx, base, fns.ctor_fn, memory, screen);
  if (!widget) {
    return 0;
  }
  const uint32_t staged = StageUtf16(ctx, base, fns.alloc_fn, scratch, text);
  if (!staged) {
    return 0;
  }
  CallGuestFunction(ctx, base, fns.set_text_fn, widget, staged);
  if (colour != 0 && fns.set_colour_fn) {
    CallGuestFunction(ctx, base, fns.set_colour_fn, widget, colour);
  }
  return widget;
}

// Re-sets an already-spawned text widget's text (e.g. updating a live
// counter/status label every frame), using the same staging path SpawnTextWidget
// uses, just without re-allocating or re-parenting.
inline bool SetTextWidgetText(PPCContext& ctx, uint8_t* base, const TextWidgetFns& fns,
                              GuestTextScratch& scratch, uint32_t widget, std::string_view text) {
  if (!fns.IsValid() || widget == 0) {
    return false;
  }
  const uint32_t staged = StageUtf16(ctx, base, fns.alloc_fn, scratch, text);
  if (!staged) {
    return false;
  }
  CallGuestFunction(ctx, base, fns.set_text_fn, widget, staged);
  return true;
}

// Measures a text widget's rendered width in pixels, or 0 if width_fn wasn't
// resolved.
inline uint32_t TextWidgetWidth(PPCContext& ctx, uint8_t* base, const TextWidgetFns& fns,
                                uint32_t widget) {
  if (!fns.width_fn || widget == 0) {
    return 0;
  }
  return CallGuestFunction(ctx, base, fns.width_fn, widget);
}

// Text widgets have no dedicated "set position" guest function; every
// current caller (NocturneRecomp's own included) just writes the X/Y offset
// fields directly, so this does too. No live ctx needed, just a mapped
// widget address.
inline void SetTextWidgetPosition(uint8_t* base, uint32_t widget, int32_t x, int32_t y) {
  if (widget == 0) {
    return;
  }
  constexpr uint32_t kWidgetXOffset = 4;
  constexpr uint32_t kWidgetYOffset = 8;
  rex::memory::store_and_swap<uint32_t>(base + widget + kWidgetXOffset, static_cast<uint32_t>(x));
  rex::memory::store_and_swap<uint32_t>(base + widget + kWidgetYOffset, static_cast<uint32_t>(y));
}

//=============================================================================
// Prompt/glyph widgets: an icon (optionally with a text label next to it),
// e.g. NocturneRecomp's own native_options.cpp CreateCustomPrompt, which is
// what this group's Spawn call directly generalizes.
//=============================================================================

// Resolved function pointers (and one data address) for the prompt/glyph
// group. Resolve once with ResolvePromptGlyphFns and reuse.
struct PromptGlyphFns {
  PPCFunc* alloc_fn = nullptr;
  PPCFunc* ctor_fn = nullptr;
  PPCFunc* find_image_fn = nullptr;      // optional: only needed to set a glyph by name
  PPCFunc* set_glyph_fn = nullptr;       // optional: only needed to set a glyph
  PPCFunc* show_glyph_fn = nullptr;      // optional
  PPCFunc* text_offset_fn = nullptr;     // optional: only needed alongside a text label
  PPCFunc* set_pos_fn = nullptr;         // optional
  PPCFunc* set_widget_text_fn = nullptr; // optional: only needed for a text label
  PPCFunc* set_text_scale_fn = nullptr;  // optional
  uint32_t image_bank_ptr = 0;           // optional: data address, only needed with find_image_fn

  // False if alloc/ctor (the only two SpawnPromptGlyph unconditionally
  // needs) failed to resolve. Everything else degrades gracefully (a
  // prompt with no glyph, or no label) if unresolved/unset.
  bool IsValid() const { return alloc_fn && ctor_fn; }
};

// Looks up and resolves the prompt/glyph function group ("menu.alloc_fn",
// "menu.prompt_ctor_fn", "menu.find_image_fn", "menu.prompt_set_glyph_fn",
// "menu.prompt_show_glyph_fn", "menu.prompt_text_offset_fn",
// "menu.prompt_set_pos_fn", "menu.option_list_set_widget_text_fn",
// "menu.option_list_set_text_scale_fn", "menu.image_bank_ptr"; see
// mods_src/game_symbols) from the shared mod registry.
inline PromptGlyphFns ResolvePromptGlyphFns(rex::Runtime* runtime) {
  PromptGlyphFns fns;
  if (!runtime || !runtime->mod_registry() || !runtime->function_dispatcher()) {
    return fns;
  }
  auto* registry = runtime->mod_registry();
  auto* dispatcher = runtime->function_dispatcher();
  auto resolve = [&](const char* name) -> PPCFunc* {
    auto addr = registry->FindAddress(name);
    return addr ? dispatcher->GetFunction(*addr) : nullptr;
  };
  fns.alloc_fn = resolve("menu.alloc_fn");
  fns.ctor_fn = resolve("menu.prompt_ctor_fn");
  fns.find_image_fn = resolve("menu.find_image_fn");
  fns.set_glyph_fn = resolve("menu.prompt_set_glyph_fn");
  fns.show_glyph_fn = resolve("menu.prompt_show_glyph_fn");
  fns.text_offset_fn = resolve("menu.prompt_text_offset_fn");
  fns.set_pos_fn = resolve("menu.prompt_set_pos_fn");
  fns.set_widget_text_fn = resolve("menu.option_list_set_widget_text_fn");
  fns.set_text_scale_fn = resolve("menu.option_list_set_text_scale_fn");
  if (auto addr = registry->FindAddress("menu.image_bank_ptr")) {
    fns.image_bank_ptr = *addr;
  }
  return fns;
}

// Allocates a prompt widget and parents it to `screen`, mirroring
// native_options.cpp's CreateCustomPrompt: construct, point it at a glyph
// looked up from the image bank by name (skipped if `image_name_addr` is 0,
// or if find_image_fn/set_glyph_fn/image_bank_ptr weren't resolved), show
// the glyph, then optionally set a text label next to it (skipped if
// `label_text` is empty or set_widget_text_fn wasn't resolved) offset by
// (text_dx, text_dy) from the glyph.
//
// `image_name_addr` is the guest address of a NUL-terminated name string the
// game's image bank recognizes (e.g. NocturneRecomp's kYGlyphNameAddr, "menu.
// glyph_name_y" in the registry, is one such name already known to exist).
// This header has no way to enumerate what names the bank holds; that has to
// come from the same kind of live RE game_symbols' other addresses did.
//
// Returns the new prompt's guest address, or 0 on failure (alloc/ctor
// unresolved or failed, or `screen` is 0).
inline uint32_t SpawnPromptGlyph(PPCContext& ctx, uint8_t* base, const PromptGlyphFns& fns,
                                 GuestTextScratch& scratch, uint32_t screen,
                                 uint32_t image_name_addr = 0, std::string_view label_text = {},
                                 int32_t text_dx = 30, int32_t text_dy = 5) {
  if (!fns.IsValid() || screen == 0) {
    return 0;
  }
  // Matches NocturneRecomp native_options.cpp's kPromptSize.
  constexpr uint32_t kPromptSize = 552;
  const uint32_t memory = CallGuestFunction(ctx, base, fns.alloc_fn, kPromptSize);
  if (!memory) {
    return 0;
  }
  const uint32_t prompt = CallGuestFunction(ctx, base, fns.ctor_fn, memory, screen, 0);
  if (!prompt) {
    return 0;
  }

  if (image_name_addr != 0 && fns.find_image_fn && fns.set_glyph_fn && fns.image_bank_ptr != 0) {
    const uint32_t bank = rex::memory::load_and_swap<uint32_t>(base + fns.image_bank_ptr);
    const uint32_t image = CallGuestFunction(ctx, base, fns.find_image_fn, bank, image_name_addr);
    if (image) {
      CallGuestFunction(ctx, base, fns.set_glyph_fn, prompt, image);
    }
  }
  if (fns.show_glyph_fn) {
    CallGuestFunction(ctx, base, fns.show_glyph_fn, prompt);
  }

  if (!label_text.empty() && fns.set_widget_text_fn) {
    const uint32_t staged = StageUtf16(ctx, base, fns.alloc_fn, scratch, label_text);
    if (staged) {
      CallGuestFunction(ctx, base, fns.set_widget_text_fn, prompt, staged);
      if (fns.text_offset_fn) {
        CallGuestFunction(ctx, base, fns.text_offset_fn, prompt, static_cast<uint32_t>(text_dx),
                          static_cast<uint32_t>(text_dy));
      }
    }
  }

  return prompt;
}

// Sets a prompt's screen position, if set_pos_fn was resolved.
inline void SetPromptPosition(PPCContext& ctx, uint8_t* base, const PromptGlyphFns& fns,
                              uint32_t prompt, int32_t x, int32_t y) {
  if (!fns.set_pos_fn || prompt == 0) {
    return;
  }
  CallGuestFunction(ctx, base, fns.set_pos_fn, prompt, static_cast<uint32_t>(x),
                    static_cast<uint32_t>(y));
}

// Sets a prompt's text scale (its label, not its glyph), if set_text_scale_fn
// was resolved; native_options.cpp's CreateCustomPrompt uses this to match
// a stock prompt's scale rather than a hardcoded one; a mod with no stock
// prompt to match can just pass a literal (1.0f is the game's own common
// case).
inline void SetPromptTextScale(PPCContext& ctx, uint8_t* base, const PromptGlyphFns& fns,
                               uint32_t prompt, float scale) {
  if (!fns.set_text_scale_fn || prompt == 0) {
    return;
  }
  CallGuestFunctionF1(ctx, base, fns.set_text_scale_fn, prompt, static_cast<double>(scale));
}

}  // namespace rexmod
