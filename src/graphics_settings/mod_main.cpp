// graphics_settings mod - overlay (F6) exposing two Settings-screen values
// that the game itself doesn't show as numbers: the screen-stretch viewport
// (preset buttons + custom width/height inputs, fully editable) and the
// Original/Enhanced graphics style (read-only display).
//
// The game's Settings screen has a debug-style overscan/stretch adjustment
// (arrow keys widen/narrow the rendered viewport) that isn't exposed with
// any on-screen numbers and isn't save-file-persisted. Its guest address
// (four consecutive big-endian uint32 fields: x offset, y offset, width,
// height) was found via a min/max boundary memory scan. More info in
// mods_src/game_symbols/mod_main.cpp
//
// The graphics-style value mirrors live, but is NOT editable from here:
// writing it (tried several different guest addresses and combinations)
// never rendered correctly; switching styles appears to run an actual
// game code path (likely a render-resource reload), not just flip a memory
// value. See mods_src/game_symbols/mod_main.cpp's kGraphicsStyleAddrVanilla
// comment for the full investigation. Switching must be done via the real
// in-game Settings menu.
//
// Like mods_src/ui_color, this mod looks both addresses up by name
// ("graphics.stretch_rect", "graphics.style") from the shared registry
// mods_src/game_symbols publishes into, instead of hardcoding them, so it
// keeps working if the vanilla/TU split is ever filled in there.

#include <rex/system/mod_plugin.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/graphics/video_mode_util.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/mod_registry.h>
#include <rex/system/xmemory.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>

namespace {

constexpr uint32_t kOffsetXOffset = 0;
constexpr uint32_t kOffsetYOffset = 4;
constexpr uint32_t kWidthOffset = 8;
constexpr uint32_t kHeightOffset = 12;
constexpr uint32_t kSpanSize = kHeightOffset + 4;  // 16 bytes covering all four fields

// Observed clamp range of the live setting at the game's compiled-in 720p
// default. The viewport rect is anchored to the bottom-right corner, so
// offset_x = max_width - width and offset_y = max_height - height, but
// max_width/max_height themselves track the actual configured render
// resolution (confirmed live at 1080p: offset_x + width == 1920, offset_y +
// height == 1080, i.e. the same struct scales 1:1 with resolution rather
// than staying fixed at 1280x720) -- see GetConfiguredVideoModeWidth/Height
// below, which Apply() uses instead of a hardcoded constant.

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

void WriteGuestU32BE(rex::memory::Memory* memory, uint32_t guest_address, uint32_t value) {
  uint8_t* host_address = memory->TranslateVirtual<uint8_t*>(guest_address);
  rex::memory::store_and_swap<uint32_t>(host_address, value);
}

// Persists the custom stretch as a fraction of the configured render
// resolution, not raw pixels -- the stretch rect isn't save-file-persisted
// (see kScreenStretchRectAddrVanilla's comment in game_symbols), so nothing
// else remembers it across a restart, and switching resolution between
// sessions (e.g. 720p one launch, 1080p the next) means raw pixel values
// from a previous session wouldn't even be valid for the new max. Storing
// width/GetConfiguredVideoModeWidth() and height/GetConfiguredVideoModeHeight()
// instead means "restore to roughly this same framing" works at any
// resolution. Lives under Runtime::user_data_root() (same per-game folder
// the game's own saves go under, per the user -- Documents/nocturnerecomp
// on Windows -- rather than hardcoding that folder name here, which
// wouldn't survive a rename and wouldn't be portable to other platforms;
// user_data_root() is however the app sets it up on this run, cross-
// platform), not the shared nocturnerecomp.toml cvar config -- keeps this
// mod's persistence independent of that file's own save/load lifecycle
// (which otherwise requires the user to hit "Save to config" in the
// unrelated built-in Settings overlay).
std::filesystem::path ConfigFilePath(rex::Runtime* runtime) {
  return runtime->user_data_root() / "mods" / "graphics_settings.cfg";
}

bool LoadPersistedRatios(rex::Runtime* runtime, double* out_width_ratio, double* out_height_ratio) {
  std::ifstream file(ConfigFilePath(runtime));
  if (!file) {
    return false;
  }
  double width_ratio = 0.0;
  double height_ratio = 0.0;
  bool have_width = false;
  bool have_height = false;
  std::string line;
  while (std::getline(file, line)) {
    if (line.rfind("width_ratio=", 0) == 0) {
      have_width = rex::cvar::ParseDouble(line.substr(12), width_ratio);
    } else if (line.rfind("height_ratio=", 0) == 0) {
      have_height = rex::cvar::ParseDouble(line.substr(13), height_ratio);
    }
  }
  if (!have_width || !have_height || width_ratio <= 0.0 || height_ratio <= 0.0) {
    return false;
  }
  *out_width_ratio = width_ratio;
  *out_height_ratio = height_ratio;
  return true;
}

void SavePersistedRatios(rex::Runtime* runtime, double width_ratio, double height_ratio) {
  std::filesystem::path path = ConfigFilePath(runtime);
  rex::filesystem::CreateParentFolder(path);
  std::ofstream file(path, std::ios::trunc);
  if (!file) {
    return;
  }
  file << "width_ratio=" << width_ratio << "\n";
  file << "height_ratio=" << height_ratio << "\n";
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
    rex::ui::RegisterBind("bind_graphics_settings", "F6", "Toggle graphics settings overlay",
                          [this] { visible_ = !visible_; });
    // Reasserts the last-requested stretch every guest frame regardless of
    // whether this overlay's window is open -- needed so a persisted value
    // (see ResolveAddress below) actually takes effect on a fresh launch
    // even if the player never presses F6. Runs on the command-processor
    // thread (see RegisterTick's docs), not the UI thread, but Apply() below
    // only touches guest memory, not ImGui, so that's fine.
    if (runtime_ && runtime_->mod_registry()) {
      runtime_->mod_registry()->RegisterTick([this] { ReassertOverride(); });
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

    // Restore a persisted custom stretch, if any, scaled to whatever
    // resolution this launch is actually configured for -- see
    // SavePersistedRatios/LoadPersistedRatios above for why this is stored
    // as a ratio rather than raw pixels. Sets the override fields directly
    // (not via SetOverride) so restoring on launch doesn't immediately
    // rewrite the same ratio back to disk.
    double width_ratio = 0.0;
    double height_ratio = 0.0;
    if (runtime_ && LoadPersistedRatios(runtime_, &width_ratio, &height_ratio)) {
      override_width_ =
          static_cast<uint32_t>(std::lround(width_ratio * GetConfiguredVideoModeWidth()));
      override_height_ =
          static_cast<uint32_t>(std::lround(height_ratio * GetConfiguredVideoModeHeight()));
      override_active_ = true;
      custom_width_ = static_cast<int>(override_width_);
      custom_height_ = static_cast<int>(override_height_);
      custom_seeded_ = true;
    }
  }

  // Writes the current override to guest memory if one is active and the
  // struct is currently readable. Shared by the per-frame tick (registered
  // in the constructor, for when this overlay isn't open) and OnDraw's own
  // per-frame reassertion (for immediate feedback while it is).
  void ReassertOverride() {
    if (!override_active_ || !addr_resolved_ || !runtime_) {
      return;
    }
    auto* memory = runtime_->memory();
    auto* heap = memory ? memory->LookupHeap(addr_) : nullptr;
    bool readable = heap && heap->QueryRangeAccess(addr_, addr_ + kSpanSize - 1) !=
                                rex::memory::PageAccess::kNoAccess;
    if (!readable) {
      return;
    }
    Apply(memory, override_width_, override_height_);
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

    if (style_addr_resolved_ && memory) {
      auto* style_heap = memory->LookupHeap(style_addr_);
      bool style_readable = style_heap && style_heap->QueryRangeAccess(style_addr_, style_addr_ + 3) !=
                                               rex::memory::PageAccess::kNoAccess;
      if (style_readable) {
        // Read-only: writing this from the overlay was tried extensively
        // (menu-selection address, applied-flag address, a companion scale
        // float, and a one-shot "trigger" pulse address, alone and in
        // various combinations) and never rendered correctly -- it looks
        // like the real Settings menu runs an actual code path (likely a
        // render-resource reload) on a genuine selection change that can't
        // be replicated by writing memory values alone. Just mirror the
        // live value here; switching must be done via the real Settings
        // menu. See mods_src/game_symbols/mod_main.cpp's
        // kGraphicsStyleAddrVanilla comment for the full story.
        uint32_t style = ReadGuestU32BE(memory, style_addr_);
        bool enhanced = style != 0;
        ImGui::TextUnformatted("Graphics style:");
        ImGui::BeginDisabled();
        ImGui::RadioButton("Original", !enhanced);
        ImGui::SameLine();
        ImGui::RadioButton("Enhanced", enhanced);
        ImGui::EndDisabled();
        ImGui::Separator();
      }
    }

    auto* heap = memory ? memory->LookupHeap(addr_) : nullptr;
    bool readable = heap && heap->QueryRangeAccess(addr_, addr_ + kSpanSize - 1) !=
                                rex::memory::PageAccess::kNoAccess;

    if (!memory || !readable) {
      ImGui::TextDisabled("Start or load a game to edit the screen stretch.");
      ImGui::End();
      return;
    }

    uint32_t width = ReadGuestU32BE(memory, addr_ + kWidthOffset);
    uint32_t height = ReadGuestU32BE(memory, addr_ + kHeightOffset);

    // Deliberately not gated on width/height already looking sane: this
    // struct reads as garbage/zero until the game's own Settings ->
    // stretch screen has been opened at least once this session (it's
    // presumably lazily initialized there). Since we can write it
    // unconditionally, don't force the user to visit that screen first --
    // just let the buttons below stomp whatever garbage is here.

    // The game re-derives this rect from something else every frame while
    // this menu is open (a one-shot write gets stomped within a frame or
    // two), so keep re-asserting the last requested value every frame
    // instead of writing once on click. The constructor's tick already does
    // this in the background regardless of this window's visibility (that's
    // what makes a persisted value take effect without needing F6 pressed),
    // but call it again here too so a just-clicked preset/edit is reflected
    // in this frame's display immediately rather than one frame later.
    if (override_active_) {
      ReassertOverride();
      width = ReadGuestU32BE(memory, addr_ + kWidthOffset);
      height = ReadGuestU32BE(memory, addr_ + kHeightOffset);
    }
    
    ImGui::TextUnformatted("PSX:");
    if (ImGui::Button("Default##psx")) {
      SetOverride(ScalePresetWidth(kPsxDefaultWidth), ScalePresetHeight(kPsxDefaultHeight));
    }
    ImGui::SameLine();
    if (ImGui::Button("Big##psx")) {
      SetOverride(ScalePresetWidth(kPsxBigWidth), ScalePresetHeight(kPsxBigHeight));
    }

    ImGui::TextUnformatted("16:10:");
    if (ImGui::Button("Default##1610")) {
      SetOverride(ScalePresetWidth(k1610DefaultWidth), ScalePresetHeight(k1610DefaultHeight));
    }
    ImGui::SameLine();
    if (ImGui::Button("Big##1610")) {
      SetOverride(ScalePresetWidth(k1610BigWidth), ScalePresetHeight(k1610BigHeight));
    }
    ImGui::SameLine();
    if (ImGui::Button("Huge##1610")) {
      SetOverride(ScalePresetWidth(k1610HugeWidth), ScalePresetHeight(k1610HugeHeight));
    }

    ImGui::TextUnformatted("Other:");
    if (ImGui::Button("Extreme##1610")) {
      SetOverride(ScalePresetWidth(k1610ExtremeWidth), ScalePresetHeight(k1610ExtremeHeight));
    }
    ImGui::SameLine();
    if (ImGui::Button("Stretched")) {
      SetOverride(ScalePresetWidth(kOtherStretchedWidth), ScalePresetHeight(kOtherStretchedHeight));
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

    ImVec2 group_start = ImGui::GetCursorPos();
    ImGui::BeginGroup();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Width");
    ImGui::SameLine(label_column);
    ImGui::SetNextItemWidth(100);
    bool width_changed = ImGui::InputInt("##width", &custom_width_);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Height");
    ImGui::SameLine(label_column);
    ImGui::SetNextItemWidth(100);
    bool height_changed = ImGui::InputInt("##height", &custom_height_);
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

    // Apply immediately on every edit rather than waiting for a button --
    // InputInt returns true on every keystroke that changes the value. With
    // the aspect ratio locked, dragging one field recomputes the other from
    // the ratio captured when the lock was engaged, instead of both moving
    // independently.
    if (width_changed || height_changed) {
      if (aspect_locked_ && locked_aspect_ratio_ > 0.0) {
        if (width_changed && !height_changed) {
          custom_height_ = static_cast<int>(std::lround(custom_width_ / locked_aspect_ratio_));
        } else if (height_changed) {
          custom_width_ = static_cast<int>(std::lround(custom_height_ * locked_aspect_ratio_));
        }
      }
      SetOverride(static_cast<uint32_t>(std::max(custom_width_, 0)),
                  static_cast<uint32_t>(std::max(custom_height_, 0)));
    }

    ImGui::End();
  }

 private:
  void SetOverride(uint32_t width, uint32_t height) {
    override_width_ = width;
    override_height_ = height;
    override_active_ = true;
    // Keep the custom fields in sync so clicking a preset shows what it set,
    // and re-typing a custom value starts from there rather than a stale one.
    custom_width_ = static_cast<int>(width);
    custom_height_ = static_cast<int>(height);
    custom_seeded_ = true;
    // Any override (preset click or custom edit) becomes the new locked
    // ratio while locked -- otherwise clicking e.g. Full while locked would
    // keep whatever ratio was captured back when the lock was first turned
    // on, instead of adopting the ratio you just explicitly picked.
    if (aspect_locked_ && height != 0) {
      locked_aspect_ratio_ = static_cast<double>(width) / height;
    }
    // Persist as a fraction of the current resolution so this restores
    // sanely even if the next launch is configured for a different one.
    uint32_t configured_width = GetConfiguredVideoModeWidth();
    uint32_t configured_height = GetConfiguredVideoModeHeight();
    if (runtime_ && configured_width != 0 && configured_height != 0) {
      SavePersistedRatios(runtime_, static_cast<double>(width) / configured_width,
                          static_cast<double>(height) / configured_height);
    }
  }

  // Deliberately unclamped: these are just what the game's own settings
  // screen happens to allow, not a hardware or format limit, and the user
  // found going beyond them works. offset_x/offset_y wrap (as huge unsigned
  // values) if width/height exceed the configured resolution -- harmless in
  // practice, matches what pressing the in-game controls far enough would
  // eventually compute anyway.
  void Apply(rex::memory::Memory* memory, uint32_t width, uint32_t height) {
    uint32_t offset_x = GetConfiguredVideoModeWidth() - width;
    uint32_t offset_y = GetConfiguredVideoModeHeight() - height;
    WriteGuestU32BE(memory, addr_ + kOffsetXOffset, offset_x);
    WriteGuestU32BE(memory, addr_ + kOffsetYOffset, offset_y);
    WriteGuestU32BE(memory, addr_ + kWidthOffset, width);
    WriteGuestU32BE(memory, addr_ + kHeightOffset, height);
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

  bool override_active_ = false;
  uint32_t override_width_ = k1610DefaultWidth;
  uint32_t override_height_ = k1610DefaultHeight;

  bool aspect_locked_ = false;
  double locked_aspect_ratio_ = 1.0;
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
