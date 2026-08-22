// graphics_settings mod - overlay (F6) exposing two Settings-screen values
// that the game itself doesn't show as numbers: the screen-stretch viewport
// (preset buttons + custom width/height inputs, fully editable) and the
// Original/Enhanced graphics style (now also editable).
//
// The game's Settings screen has a debug-style overscan/stretch adjustment
// (arrow keys widen/narrow the rendered viewport) that isn't exposed with
// any on-screen numbers and isn't save-file-persisted. Its guest address was
// found via a min/max boundary memory scan.  More info in
// mods_src/game_symbols/mod_main.cpp
//
// The four consecutive big-endian uint32 fields are LEFT, TOP, RIGHT, BOTTOM
// -- *not* (x offset, y offset, width, height) as this file used to claim.
// Confirmed against the game's own initializer at sub_82582360, which fills
// the struct with {v29, v30, display_width - v29, display_height - v30}, and
// against every consumer: sub_82589648, sub_825AEB98, sub_825AEF30 and
// sub_82588EC8 all derive the viewport extent as (RIGHT - LEFT) and
// (BOTTOM - TOP).  The game's own slider (sub_825BB2B0 / sub_825BA678) lerps
// all four edges independently between a min rect at 0x82882CC8 (the 7.5%
// overscan box) and a max rect at 0x82882C98 (= {0, 0, W, H}).
//
// The mod's UI parameter (historically called "width"/"height", and kept
// under those names so existing presets and persisted values keep producing
// the exact same framing) is really the RIGHT/BOTTOM edge: the native engine
// that now owns writing this struct (src/graphics_settings.cpp in
// NocturneRecomp, this mod no longer writes it directly -- see the
// constructor's comment) derives LEFT = max_w - right and RIGHT = right,
// which keeps LEFT + RIGHT == max_w, i.e. a horizontally centered rect --
// matching the game's own invariant and the user's live measurement of
// "offset_x + width == 1920" at 1080p.  The actual rendered extent is
// therefore (2 * right - max_w), not `right`.
//
// That distinction matters for more than pedantry: when 2 * right == max_w
// the extent is exactly zero, and sub_82588EC8 (the pointer/cursor hit test)
// guards its two integer divisions by the extent with compiler-emitted
// `twllei <extent>, 0` traps -- a guest trap, which in a recompiled binary
// hangs rather than crashing.  At 720p that is right == 640 / bottom == 360;
// at 1080p, 960 / 540.  ClampEdge() below keeps this mod's own requests clear
// of that zero-extent case before they're ever published, and the custom
// input boxes commit on Enter/focus-loss rather than on every keystroke, so
// simply typing "3600" doesn't transit the trapping value 360 on the way.
//
// The graphics-style toggle no longer writes guest memory itself -- like the
// stretch rect above, ownership of the applied settings entry byte (which
// the per-frame render conversion, sub_824FB460, reads -- not the derived
// global at dword_828B146C, which gets overwritten every frame) moved to the
// native engine (src/graphics_settings.cpp in NocturneRecomp), which
// persists it as the graphics_style_enhanced cvar and reasserts it every
// frame so it survives the game's own settings/save-data init running at an
// unpredictable point after boot. This overlay's radio buttons just publish
// a "graphics_settings.request_style" request (see RequestGraphicsStyle) and
// read graphics.style live to show the current choice.
//
// Like mods_src/ui_color, this mod looks addresses up by name from the
// shared registry mods_src/game_symbols publishes into, instead of
// hardcoding them, so it keeps working if the vanilla/TU split is ever
// filled in there.

#include <rex/system/mod_plugin.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/graphics/video_mode_util.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/mod_registry.h>
#include <rex/system/xmemory.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>

namespace {

// LTRB, per the header comment above. Only RIGHT/BOTTOM are used directly by
// this file now (for the live display read) -- LEFT/TOP are derived by the
// native engine that owns writing the struct.
constexpr uint32_t kRightOffset = 8;
constexpr uint32_t kBottomOffset = 12;
constexpr uint32_t kSpanSize = kBottomOffset + 4;  // 16 bytes covering all four fields

// Smallest viewport extent ClampEdge() will allow this mod to request, in
// guest pixels. Anything at or below zero makes sub_82588EC8's `twllei`
// divide-by-zero guards trap (see the header comment); a few pixels of slack
// past that costs nothing and keeps the ratios the hit test computes from
// being absurd.
constexpr int64_t kMinExtent = 16;

// How often the per-frame tick is allowed to probe page protections while
// waiting for a guest struct to become mapped, in ticks. Every probe takes
// the process-wide global critical region (see EnsureRectMapped), so this is
// deliberately coarse -- these structs appear once, early, and a ~1s worst
// case before the mod notices is imperceptible.
constexpr uint32_t kProbeIntervalTicks = 60;

// Observed clamp range of the live setting at the game's compiled-in 720p
// default. The viewport rect is anchored to the bottom-right corner, so
// offset_x = max_width - width and offset_y = max_height - height, but
// max_width/max_height themselves track the actual configured render
// resolution (confirmed live at 1080p: offset_x + width == 1920, offset_y +
// height == 1080, i.e. the same struct scales 1:1 with resolution rather
// than staying fixed at 1280x720) -- see GetConfiguredVideoModeWidth/Height
// below, which VideoWidth()/VideoHeight() use instead of a hardcoded
// constant.

// PSX presets
constexpr uint32_t kPsxDefaultWidth = 1052;
constexpr uint32_t kPsxDefaultHeight = 720;
// User-measured at 1080p (1645x1147, ScalePresetWidth/Height's 1.5x factor
// there), divided back down to this table's 720p baseline: 1645/1.5 =
// 1096.67, 1147/1.5 = 764.67, each rounded to the nearest integer. Scaling
// this baseline back up at 1080p reproduces 1646x1148 (ScalePresetWidth/
// Height's std::lround rounds the exact 1097*1.5 = 1645.5 tie up) -- 1px off
// from the original 1080p measurement in each axis, unavoidable since no
// integer 720p baseline scales by exactly 1.5x to land on an odd target
// (1645, 1147) precisely. This replaced the old kPsxBigWidth/Height
// (1098/766) -- close enough to a separate "Huge" preset that having both
// wasn't worth the redundancy.
constexpr uint32_t kPsxBigWidth = 1097;
constexpr uint32_t kPsxBigHeight = 765;

// 16:10 presets
constexpr uint32_t k1610DefaultWidth = 1052;
constexpr uint32_t k1610DefaultHeight = 667;
constexpr uint32_t k1610BigWidth = 1136;
constexpr uint32_t k1610BigHeight = 720;
constexpr uint32_t k1610HugeWidth = 1226;
constexpr uint32_t k1610HugeHeight = 765;
constexpr uint32_t k1610ExtremeWidth = 1282;
constexpr uint32_t k1610ExtremeHeight = 793;

// Other presets
constexpr uint32_t kOtherStretchedWidth = 1280;
constexpr uint32_t kOtherStretchedHeight = 766;

uint32_t ReadGuestU32BE(rex::memory::Memory* memory, uint32_t guest_address) {
  const uint8_t* host_address = memory->TranslateVirtual<const uint8_t*>(guest_address);
  return rex::memory::load_and_swap<uint32_t>(host_address);
}

void WriteGuestU8(rex::memory::Memory* memory, uint32_t guest_address, uint8_t value) {
  uint8_t* host_address = memory->TranslateVirtual<uint8_t*>(guest_address);
  *host_address = value;
}

// True if the guest range starting at |guest_address| is currently mapped/
// readable, instead of blindly dereferencing it.
//
// EXPENSIVE: LookupHeap/QueryRangeAccess take BaseHeap's
// rex::thread::global_critical_region (see rex/system/xmemory.h), the single
// process-wide recursive mutex that also guards guest thread suspension and
// kernel-object state. Its header describes holding it as "disabling
// interrupts in the guest". Calling this from the per-frame tick -- which
// runs on the command-processor thread at GPU swap, not on a guest thread --
// therefore contends with the guest for that lock every single frame, and
// can wedge the whole emulator: guest thread enters the critical region and
// then waits on the GPU, while the command processor blocks entering the
// same region inside a mod tick and so never completes the swap that would
// wake the guest. Freeze, no crash, nothing in the log. Everything below is
// structured to call this a bounded number of times during boot and then
// never again -- see EnsureRectMapped/PollStyleGlobalMapped. Once an address
// is known-mapped, plain TranslateVirtual reads/writes need no lock at all.
bool IsGuestRangeReadable(rex::memory::Memory* memory, uint32_t guest_address, uint32_t size) {
  auto* heap = memory->LookupHeap(guest_address);
  return heap && heap->QueryRangeAccess(guest_address, guest_address + size - 1) !=
                     rex::memory::PageAccess::kNoAccess;
}

// Mirrors GetConfiguredVideoModeWidth/Height in the SDK's
// kernel/xboxkrnl/xboxkrnl_video.cpp: the "resolution" cvar (e.g. "1080p")
// only feeds into that derived value, it's never written back into the
// video_mode_width/video_mode_height cvar storage itself. So checking those
// cvars directly still reads the 1280x720 compiled default even when the
// game is actually told to run at 1080p; the same fallback chain has to be
// replicated here to know the resolution actually in effect.
uint32_t GetConfiguredVideoModeWidth() {
  int32_t configured_width = REXCVAR_QUERY(int32_t, video_mode_width);
  if (!rex::cvar::HasNonDefaultValue("video_mode_width")) {
    if (rex::cvar::HasNonDefaultValue("window_width") && REXCVAR_QUERY(int32_t, window_width) > 0) {
      configured_width = REXCVAR_QUERY(int32_t, window_width);
    } else {
      int32_t preset_width = 0;
      int32_t preset_height = 0;
      if (rex::graphics::video_mode_util::TryGetResolutionPresetFromCVar(preset_width,
                                                                         preset_height)) {
        configured_width = preset_width;
      }
    }
  }
  return static_cast<uint32_t>(std::clamp(configured_width, 640, 0x0FFF));
}

uint32_t GetConfiguredVideoModeHeight() {
  int32_t configured_height = REXCVAR_QUERY(int32_t, video_mode_height);
  if (!rex::cvar::HasNonDefaultValue("video_mode_height")) {
    if (rex::cvar::HasNonDefaultValue("window_height") && REXCVAR_QUERY(int32_t, window_height) > 0) {
      configured_height = REXCVAR_QUERY(int32_t, window_height);
    } else {
      int32_t preset_width = 0;
      int32_t preset_height = 0;
      if (rex::graphics::video_mode_util::TryGetResolutionPresetFromCVar(preset_width,
                                                                         preset_height)) {
        configured_height = preset_height;
      }
    }
  }
  return static_cast<uint32_t>(std::clamp(configured_height, 480, 0x0FFF));
}

// All the preset constants below are baseline pixel values tuned at the
// game's compiled-in 720p default. Scale them to the actual configured
// resolution before applying -- confirmed live that the stretch-rect max
// scales 1:1 with the configured resolution (e.g. 1.5x at 1080p), so scaling
// the presets by the same per-axis ratio keeps their relative framing intact
// at any resolution instead of only working at exactly 1280x720.
uint32_t ScalePresetWidth(uint32_t base_width) {
  double ratio = static_cast<double>(GetConfiguredVideoModeWidth()) / 1280.0;
  return static_cast<uint32_t>(std::lround(base_width * ratio));
}

uint32_t ScalePresetHeight(uint32_t base_height) {
  double ratio = static_cast<double>(GetConfiguredVideoModeHeight()) / 720.0;
  return static_cast<uint32_t>(std::lround(base_height * ratio));
}

// Padlock icon + fixed-size icon button, lifted from mods_src/music_player
// (see audio_player.cpp's "Lock" transport toggle) so this mod's lock button
// matches that visual convention instead of being a plain text button.
namespace icons {

constexpr float kButtonSize = 22.0f;

void DrawLock(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImU32 col, bool locked) {
  ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
  float body_w = (p1.x - p0.x) * 0.30f;
  float body_h = (p1.y - p0.y) * 0.34f;
  ImVec2 body_min(c.x - body_w, c.y - body_h * 0.1f);
  ImVec2 body_max(c.x + body_w, c.y + body_h);
  dl->AddRectFilled(body_min, body_max, col, 1.5f);

  float shackle_r = body_w * 0.85f;
  float thickness = 2.0f;
  ImVec2 hinge = locked ? ImVec2(c.x, body_min.y) : ImVec2(c.x + shackle_r * 0.5f, body_min.y);
  constexpr float kPi = 3.14159265f;
  dl->PathArcTo(hinge, shackle_r, kPi, 2.0f * kPi, 16);
  dl->PathStroke(col, 0, thickness);
}

}  // namespace icons

template <typename DrawIconFn>
bool IconButton(const char* str_id, DrawIconFn&& draw_icon, bool active = false) {
  ImGui::PushID(str_id);
  ImVec2 size(icons::kButtonSize, icons::kButtonSize);
  bool pressed = ImGui::InvisibleButton(str_id, size);
  bool hovered = ImGui::IsItemHovered();
  bool held = ImGui::IsItemActive();

  ImGuiCol bg = active || held ? ImGuiCol_ButtonActive
                : hovered      ? ImGuiCol_ButtonHovered
                               : ImGuiCol_Button;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p0 = ImGui::GetItemRectMin();
  ImVec2 p1 = ImGui::GetItemRectMax();
  dl->AddRectFilled(p0, p1, ImGui::GetColorU32(bg), ImGui::GetStyle().FrameRounding);
  draw_icon(dl, p0, p1, ImGui::GetColorU32(ImGuiCol_Text));

  ImGui::PopID();
  return pressed;
}

class GraphicsSettingsDialog : public rex::ui::ImGuiDialog {
 public:
  GraphicsSettingsDialog(rex::ui::ImGuiDrawer* drawer, rex::Runtime* runtime)
      : ImGuiDialog(drawer), runtime_(runtime) {
    rex::ui::RegisterBind(
        "bind_graphics_settings", "F6", "Toggle graphics settings overlay", [this] { visible_ = !visible_; });
    // Polls guest-struct mapping state every frame regardless of whether
    // this overlay's window is open, so the live-display reads below (for
    // both the stretch rect and the graphics style) become accurate as soon
    // as the game's settings struct is mapped, without probing page
    // protections from the UI thread. Runs on the command-processor thread
    // (see RegisterTick's docs), not the UI thread, but neither touches
    // ImGui, so that's fine.
    //
    // What is emphatically NOT fine on that thread is taking the global
    // critical region, which is what probing page protections does -- see
    // IsGuestRangeReadable's comment for the deadlock that causes.
    // EnsureRectMapped/PollStyleGlobalMapped below are written to probe only
    // until their addresses latch as mapped (rate-limited to one probe per
    // kProbeIntervalTicks while they wait), and to use lock-free
    // TranslateVirtual accesses from then on. Steady state, this tick takes
    // no locks at all.
    if (runtime_ && runtime_->mod_registry()) {
      runtime_->mod_registry()->RegisterTick([this] { PollMapping(); });
      // The base game's own "Change Screen Size..." screen (src/
      // graphics_settings.cpp, formerly the resolution_preset_native mod)
      // owns applying and reasserting graphics.stretch_rect, and (as of the
      // graphics-style persistence fix) graphics.style/graphics.style_menu
      // too. This mod used to maintain its own competing per-frame reassert
      // of those same guest bytes (ReassertOverride/Apply/SetOverride for
      // the rect, TryRestoreStyle for the style, all now removed) --
      // independently-tracked values racing to write the same guest memory
      // every frame, whichever ran last that frame "winning" until the
      // other stomped it back next frame. That's structurally unfixable by
      // arbitrating *who* writes (tried and abandoned on the native side
      // too, per its own comments); the fix is to have only one writer. This
      // mod is now a pure display+request client: it publishes what it wants
      // ("graphics_settings.request_preset"/"request_custom_ratio"/
      // "request_style", see the preset buttons, custom width/height commit
      // path, and style radio buttons in OnDraw below) and simply reads live
      // guest state to display -- with native as the sole writer, that live
      // read is always accurate, so the
      // "graphics_settings.preset_applied" subscription this used to adopt
      // as a competing override is gone too; nothing to adopt anymore.
    }
  }

  ~GraphicsSettingsDialog() override { rex::ui::UnregisterBind("bind_graphics_settings"); }

  // Called once KernelState/the executable module are live (see
  // GraphicsSettingsMod::OnModuleLaunched). Looks up the addresses published
  // by game_symbols (see mods_src/game_symbols/mod_main.cpp), which resolves
  // vanilla vs TU for us.
  void ResolveAddress() {
    if (runtime_ && runtime_->mod_registry()) {
      if (auto addr = runtime_->mod_registry()->FindAddress("graphics.stretch_rect")) {
        addr_ = *addr;
        addr_resolved_ = true;
      }
      if (auto addr = runtime_->mod_registry()->FindAddress("graphics.style")) {
        style_addr_ = *addr;
        style_addr_resolved_ = true;
      }
    }

    // Snapshot the configured render resolution once. VideoWidth()/
    // VideoHeight() need it on every request; GetConfiguredVideoModeWidth/
    // Height do several by-name cvar lookups each, which has no business
    // being on a hot path. The resolution can't change without a relaunch,
    // so a launch-time snapshot is exact rather than merely close.
    cached_video_width_ = GetConfiguredVideoModeWidth();
    cached_video_height_ = GetConfiguredVideoModeHeight();

    // Stretch restore-on-launch is entirely owned by src/graphics_settings.cpp
    // in the base game now (same persisted config file/keys this mod used to
    // read directly, so an existing player's saved preference still carries
    // over) -- it applies the restored ratio to the live guest rect itself at
    // boot, which this mod's live OnDraw read then just picks up. No restore
    // path or "graphics_settings.preset_applied" subscription is needed here
    // anymore; see that file's Bind() for the restore side.

    // Style restore-on-launch is now owned by src/graphics_settings.cpp in
    // the base game, the same way stretch restore already was: it reasserts
    // the persisted graphics_style_enhanced cvar into the live guest struct
    // every frame, indefinitely, so it survives the game's own settings/
    // save-data init running at an unpredictable point after boot. This
    // mod's own restore used to duplicate that (a second, independent
    // writer of the same two guest bytes, racing the native engine once it
    // took over the same job) -- gone now; OnDraw's radio buttons below just
    // request a value, they no longer write memory or a config file
    // themselves.
  }

  // Latches whether the stretch rect is mapped. Probes at most once every
  // kProbeIntervalTicks until it succeeds, then never probes again -- see
  // IsGuestRangeReadable's comment for why per-frame probing is a deadlock
  // hazard rather than merely slow. Tick-thread only; OnDraw reads the
  // latched flag instead of probing on the UI thread.
  bool EnsureRectMapped(rex::memory::Memory* memory) {
    if (rect_mapped_.load(std::memory_order_acquire)) {
      return true;
    }
    if (!addr_resolved_ || !memory) {
      return false;
    }
    if (rect_probe_countdown_ > 0) {
      --rect_probe_countdown_;
      return false;
    }
    rect_probe_countdown_ = kProbeIntervalTicks;
    if (!IsGuestRangeReadable(memory, addr_, kSpanSize)) {
      return false;
    }
    rect_mapped_.store(true, std::memory_order_release);
    return true;
  }

  // Same, for the derived graphics-style global OnDraw displays.
  void PollStyleGlobalMapped(rex::memory::Memory* memory) {
    if (style_global_mapped_.load(std::memory_order_acquire) || !style_addr_resolved_ || !memory) {
      return;
    }
    if (style_global_probe_countdown_ > 0) {
      --style_global_probe_countdown_;
      return;
    }
    style_global_probe_countdown_ = kProbeIntervalTicks;
    if (IsGuestRangeReadable(memory, style_addr_, 4)) {
      style_global_mapped_.store(true, std::memory_order_release);
    }
  }

  // Polls whether the stretch-rect/style-global guest structs are mapped
  // yet, so OnDraw's enabled/disabled state (and, for style, its restore
  // path) stays current without probing page protections from the UI
  // thread. Called from the per-frame tick. No longer writes guest memory
  // itself -- this mod never touches graphics.stretch_rect directly
  // anymore, see the constructor's comment.
  void PollMapping() {
    if (!runtime_) {
      return;
    }
    auto* memory = runtime_->memory();
    if (!memory) {
      return;
    }
    EnsureRectMapped(memory);
    PollStyleGlobalMapped(memory);
  }

 protected:
  void OnDraw(ImGuiIO& io) override {
    (void)io;
    if (!visible_) {
      return;
    }

    ImGui::SetNextWindowSize(ImVec2(320, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Graphics Settings", &visible_, ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::End();
      return;
    }

    auto* memory = runtime_ ? runtime_->memory() : nullptr;

    // Both readability flags below are latched by the per-frame tick. This
    // thread deliberately never probes page protections itself: that takes
    // the global critical region (see IsGuestRangeReadable), and doing it from
    // the UI thread every frame just adds a second non-guest contender for
    // the lock the guest needs to make progress.
    if (memory && style_global_mapped_.load(std::memory_order_acquire)) {
      uint32_t style = ReadGuestU32BE(memory, style_addr_);
      bool enhanced = style != 0;
      ImGui::TextUnformatted("Graphics style:");
      if (ImGui::RadioButton("Original", !enhanced)) {
        RequestGraphicsStyle(false);
      }
      ImGui::SameLine();
      if (ImGui::RadioButton("Enhanced", enhanced)) {
        RequestGraphicsStyle(true);
      }
      ImGui::Separator();
    }

    if (!memory || !rect_mapped_.load(std::memory_order_acquire)) {
      ImGui::TextDisabled("Start or load a game to edit the screen stretch.");
      ImGui::End();
      return;
    }

    // These are the RIGHT/BOTTOM edges, which is what this overlay's
    // "width"/"height" parameter has always actually been -- see the header
    // comment. The rendered extent is (right - left).
    uint32_t width = ReadGuestU32BE(memory, addr_ + kRightOffset);
    uint32_t height = ReadGuestU32BE(memory, addr_ + kBottomOffset);

    // Deliberately not gated on width/height already looking sane: this
    // struct reads as garbage/zero until the game's own Settings ->
    // stretch screen has been opened at least once this session (it's
    // presumably lazily initialized there). Since we can write it
    // unconditionally, don't force the user to visit that screen first --
    // just let the buttons below stomp whatever garbage is here.

    // Live guest read, always -- native (src/graphics_settings.cpp) is the
    // sole writer of this struct now, so there's no override state of this
    // mod's own to prefer over it, and no torn-read risk from two writers
    // racing.

    // Preset indices below must match native's kPresetNames ordering (PSX
    // Default/Big, 16:10 Default/Big/Huge/Extreme, Stretched); see
    // RequestPreset's comment.
    ImGui::TextUnformatted("PSX:");
    if (ImGui::Button("Default##psx")) {
      RequestPreset(0, ScalePresetWidth(kPsxDefaultWidth), ScalePresetHeight(kPsxDefaultHeight));
    }
    ImGui::SameLine();
    if (ImGui::Button("Big##psx")) {
      RequestPreset(1, ScalePresetWidth(kPsxBigWidth), ScalePresetHeight(kPsxBigHeight));
    }

    ImGui::TextUnformatted("16:10:");
    if (ImGui::Button("Default##1610")) {
      RequestPreset(2, ScalePresetWidth(k1610DefaultWidth), ScalePresetHeight(k1610DefaultHeight));
    }
    ImGui::SameLine();
    if (ImGui::Button("Big##1610")) {
      RequestPreset(3, ScalePresetWidth(k1610BigWidth), ScalePresetHeight(k1610BigHeight));
    }
    ImGui::SameLine();
    if (ImGui::Button("Huge##1610")) {
      RequestPreset(4, ScalePresetWidth(k1610HugeWidth), ScalePresetHeight(k1610HugeHeight));
    }

    ImGui::TextUnformatted("Other:");
    if (ImGui::Button("Extreme##1610")) {
      RequestPreset(5, ScalePresetWidth(k1610ExtremeWidth), ScalePresetHeight(k1610ExtremeHeight));
    }
    ImGui::SameLine();
    if (ImGui::Button("Stretched")) {
      RequestPreset(6, ScalePresetWidth(kOtherStretchedWidth), ScalePresetHeight(kOtherStretchedHeight));
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Custom:");

    // Seed the boxes from the live value exactly once, then leave them
    // alone -- these are a staging area for a value to type and Apply, not
    // a live mirror, so continuously resyncing them from memory (like the
    // preset/current display above does) would overwrite every keystroke.
    if (!custom_seeded_) {
      custom_width_ = static_cast<int>(width);
      custom_height_ = static_cast<int>(height);
      custom_seeded_ = true;
    }

    // "Height" is the wider of the two labels; align both input boxes to
    // start right after it so the two rows line up.
    float label_column = ImGui::CalcTextSize("Height").x + ImGui::GetStyle().ItemSpacing.x;

    // Commit on Enter, on the +/- step buttons, or on focus loss -- NOT on
    // every keystroke, which is what this did before. Per-keystroke apply
    // meant typing a four-digit number pushed every prefix of it into the
    // guest rect: typing "3600" into Height transited 3, 36 and then 360,
    // and at 720p a bottom edge of exactly 360 is a zero-height viewport,
    // which trips the `twllei` divide-by-zero trap in sub_82588EC8 and hangs
    // the game. ClampEdge() in RequestCustomRatio() is the real backstop for
    // that, but there is no reason to be shoving half-typed numbers at the
    // guest in the first place -- it also wrote the config file on every
    // keystroke.
    //
    // InputInt with a non-zero step wraps itself in BeginGroup/EndGroup, and
    // EndGroup propagates the Deactivated/Edited status flags out to the
    // group, so IsItemDeactivatedAfterEdit() is valid here.
    constexpr ImGuiInputTextFlags kCommitFlags = ImGuiInputTextFlags_EnterReturnsTrue;
    ImVec2 group_start = ImGui::GetCursorPos();
    ImGui::BeginGroup();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Width");
    ImGui::SameLine(label_column);
    ImGui::SetNextItemWidth(100);
    bool width_changed = ImGui::InputInt("##width", &custom_width_, 1, 100, kCommitFlags);
    width_changed |= ImGui::IsItemDeactivatedAfterEdit();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Height");
    ImGui::SameLine(label_column);
    ImGui::SetNextItemWidth(100);
    bool height_changed = ImGui::InputInt("##height", &custom_height_, 1, 100, kCommitFlags);
    height_changed |= ImGui::IsItemDeactivatedAfterEdit();
    ImGui::EndGroup();
    ImVec2 group_size = ImGui::GetItemRectSize();

    // Lock button, vertically centered next to the two-line Width/Height
    // group above.
    ImGui::SameLine();
    ImGui::SetCursorPosY(group_start.y + (group_size.y - icons::kButtonSize) * 0.5f);
    bool locked = aspect_locked_;
    bool lock_clicked = IconButton(
        "##lock",
        [locked](ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImU32 col) {
          icons::DrawLock(dl, p0, p1, col, locked);
        },
        locked);
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("%s aspect ratio while adjusting width/height",
                        aspect_locked_ ? "Locked" : "Click to lock");
    }
    if (lock_clicked) {
      aspect_locked_ = !aspect_locked_;
      // Capture the ratio from the live, freshly-read value -- not from
      // custom_width_/custom_height_, which only track whatever this
      // overlay last set and can be stale if the actual current aspect got
      // there some other way (e.g. the vanilla in-game controls, or simply
      // not having touched the custom fields yet this session).
      if (aspect_locked_ && height != 0) {
        locked_aspect_ratio_ = static_cast<double>(width) / height;
      }
    }

    // Apply on commit (see kCommitFlags above) rather than needing a
    // separate Apply button. With the aspect ratio locked, changing one
    // field recomputes the other from the ratio captured when the lock was
    // engaged, instead of both moving independently.
    if (width_changed || height_changed) {
      if (aspect_locked_ && locked_aspect_ratio_ > 0.0) {
        if (width_changed && !height_changed) {
          custom_height_ = static_cast<int>(std::lround(custom_width_ / locked_aspect_ratio_));
        } else if (height_changed) {
          custom_width_ = static_cast<int>(std::lround(custom_height_ * locked_aspect_ratio_));
        }
      }
      RequestCustomRatio(static_cast<uint32_t>(std::max(custom_width_, 0)),
                        static_cast<uint32_t>(std::max(custom_height_, 0)));
    }

    ImGui::End();
  }

 private:
  // Asks the native engine (src/graphics_settings.cpp in NocturneRecomp) to
  // apply and own persisting/reasserting the graphics style, instead of this
  // mod writing the guest bytes and its own config file directly -- same
  // "publish the request, engine owns the write" shape as RequestPreset/
  // RequestCustomRatio below, needed for the same reason: two independent
  // writers of the same guest bytes (this mod's old TryRestoreStyle and the
  // native engine's own reassert) raced every frame, whichever ran last
  // "winning" until the other stomped it back.
  void RequestGraphicsStyle(bool enhanced) {
    if (!runtime_ || !runtime_->mod_registry()) {
      return;
    }
    rex::system::ModRegistry::EventPayload payload;
    payload.u64 = enhanced ? 1 : 0;
    runtime_->mod_registry()->Publish("graphics_settings.request_style", payload);
  }

  // Stages a clicked preset's edges into the UI (so the boxes/lock ratio
  // update immediately, same as the old SetOverride did) and asks the native
  // engine (src/graphics_settings.cpp in NocturneRecomp) to actually apply
  // and own reasserting it -- see the constructor's comment for why this mod
  // no longer writes graphics.stretch_rect itself. |preset_index| must match
  // that file's kPresetNames ordering (PSX Default/Big, 16:10 Default/Big/
  // Huge/Extreme, Stretched -- this mod's own preset catalog is already
  // index-identical to it, solved from these very constants).
  void RequestPreset(int32_t preset_index, uint32_t width, uint32_t height) {
    StageCustomFields(width, height);
    if (!runtime_ || !runtime_->mod_registry()) {
      return;
    }
    rex::system::ModRegistry::EventPayload payload;
    payload.u64 = static_cast<uint64_t>(preset_index);
    runtime_->mod_registry()->Publish("graphics_settings.request_preset", payload);
  }

  // Same, for a hand-typed width/height that doesn't match any catalog
  // preset -- converts to the width_ratio/height_ratio convention
  // src/graphics_settings.cpp's restore/persistence path already uses, and
  // publishes it as a two-double payload (EventPayload only has one f64
  // slot). The struct is copied synchronously by the native subscriber
  // before Publish() returns, so it's safe to point at a local.
  void RequestCustomRatio(uint32_t width, uint32_t height) {
    StageCustomFields(width, height);
    if (!runtime_ || !runtime_->mod_registry()) {
      return;
    }
    struct CustomRatioPayload {
      double width_ratio;
      double height_ratio;
    };
    CustomRatioPayload data{static_cast<double>(width) / VideoWidth(),
                            static_cast<double>(height) / VideoHeight()};
    rex::system::ModRegistry::EventPayload payload;
    payload.bytes = {reinterpret_cast<const uint8_t*>(&data), sizeof(data)};
    runtime_->mod_registry()->Publish("graphics_settings.request_custom_ratio", payload);
  }

  // Shared staging step for both request helpers above: clamps (so the
  // number echoed back into the input boxes and the locked aspect ratio
  // agree with what the native engine will actually apply -- it clamps the
  // same way, via the same ClampEdge-shaped floor on the custom-ratio path),
  // keeps the custom fields in sync so clicking a preset shows what it set,
  // and updates the locked aspect ratio to whatever was just explicitly
  // picked.
  void StageCustomFields(uint32_t& width, uint32_t& height) {
    width = ClampEdge(width, VideoWidth());
    height = ClampEdge(height, VideoHeight());
    custom_width_ = static_cast<int>(width);
    custom_height_ = static_cast<int>(height);
    custom_seeded_ = true;
    if (aspect_locked_ && height != 0) {
      locked_aspect_ratio_ = static_cast<double>(width) / height;
    }
  }

  // Lower bound on an edge value, so a request this mod publishes never
  // asks the native engine for a degenerate extent. |edge| is the RIGHT (or
  // BOTTOM) coordinate and the opposite
  // edge is derived as |max| - |edge|, so the rendered extent works out to
  // (2 * edge - max): it collapses to zero at edge == max / 2 and inverts
  // below that. Zero is the dangerous one -- sub_82588EC8 divides by the
  // extent under a `twllei <extent>, 0` guard, and a guest trap in a
  // recompiled binary hangs the process instead of raising anything anyone
  // can see. At 720p that is edge 640 horizontally / 360 vertically; at
  // 1080p, 960 / 540. Trivially reachable, and previously reachable by
  // simply typing a number that happens to pass through one of them.
  //
  // The UPPER end stays deliberately unclamped, as before: the game's own
  // settings screen limits are a UI convention, not a hardware or format
  // limit, and the user confirmed going past them works. The opposite edge
  // just goes (hugely) unsigned there, which every consumer handles fine
  // because they all compute the extent with signed subtraction.
  static uint32_t ClampEdge(uint32_t edge, uint32_t max) {
    // Smallest edge with (2 * edge - max) >= kMinExtent, rounding up.
    int64_t floor_edge = (static_cast<int64_t>(max) + kMinExtent + 1) / 2;
    return static_cast<uint32_t>(std::max<int64_t>(edge, floor_edge));
  }

  // Launch-time snapshot of the configured render resolution, with a lazy
  // fallback in case anything reaches these before ResolveAddress ran.
  uint32_t VideoWidth() {
    if (cached_video_width_ == 0) {
      cached_video_width_ = GetConfiguredVideoModeWidth();
    }
    return cached_video_width_;
  }
  uint32_t VideoHeight() {
    if (cached_video_height_ == 0) {
      cached_video_height_ = GetConfiguredVideoModeHeight();
    }
    return cached_video_height_;
  }

  rex::Runtime* runtime_ = nullptr;
  bool visible_ = false;

  bool addr_resolved_ = false;
  uint32_t addr_ = 0;

  bool style_addr_resolved_ = false;
  uint32_t style_addr_ = 0;

  bool custom_seeded_ = false;
  int custom_width_ = static_cast<int>(k1610DefaultWidth);
  int custom_height_ = static_cast<int>(k1610DefaultHeight);

  bool aspect_locked_ = false;
  double locked_aspect_ratio_ = 1.0;

  // Launch-time snapshot; see VideoWidth/VideoHeight.
  uint32_t cached_video_width_ = 0;
  uint32_t cached_video_height_ = 0;

  // Readability latches. Written by the per-frame tick, read by the UI
  // thread, hence atomic; once set they are never cleared, which is the
  // whole point -- it means no thread probes page protections (and so takes
  // the global critical region) after boot. The countdowns beside them are
  // touched only by the tick thread.
  std::atomic<bool> rect_mapped_{false};
  uint32_t rect_probe_countdown_ = 0;
  std::atomic<bool> style_global_mapped_{false};
  uint32_t style_global_probe_countdown_ = 0;
};

class GraphicsSettingsMod : public rex::system::IModPlugin {
 public:
  explicit GraphicsSettingsMod(rex::Runtime* runtime) : runtime_(runtime) {}

  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    dialog_ = std::make_unique<GraphicsSettingsDialog>(drawer, runtime_);
  }

  void OnModuleLaunched() override {
    if (dialog_) {
      dialog_->ResolveAddress();
    }
  }

 private:
  rex::Runtime* runtime_ = nullptr;
  std::unique_ptr<GraphicsSettingsDialog> dialog_;
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
  return new GraphicsSettingsMod(ctx->runtime);
}
