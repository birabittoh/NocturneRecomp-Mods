// jumpscare mod - fullscreen chroma-keyed video + audio jumpscare.
//
// Once per real-world second, rolls a 1-in-N chance (N configurable via the
// foxy_jumpscare_chance cvar, default 5000) of playing a short green-screened
// clip fullscreen over the game, with its own audio track. The clip is
// pre-baked at mod build time (see assets/): ffmpeg chroma-keyed the green
// background to alpha and split out a plain frame-sequence of RGBA PNGs
// plus a WAV audio track.
//
// All the actual clip playback (frame decode/texture upload, WAV parsing,
// timing, drawing fullscreen, playing audio through the engine's own SDL
// audio backend) is rex::ui::BakedClipPlayer, an SDK-side helper added
// alongside this mod specifically to cover this "play a pre-baked clip"
// case -- this file is just the roll-the-dice logic and cvar. Frame
// texture creation is deferred to ImGuiDrawer::OnImmediateDrawerReady
// (also new) since IModPlugin::OnCreateDialogs runs before a presenter/
// ImmediateDrawer is attached.

#include <rex/system/mod_plugin.h>

#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <memory>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/baked_clip_player.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>

REXCVAR_DEFINE_INT32(foxy_jumpscare_chance, 5000, "Jumpscare",
                     "1-in-N chance, rolled once per real-world second, of a "
                     "fullscreen jumpscare. 0 or negative disables it.");

namespace {

// Matches the source clip's native frame rate (ffmpeg extracted
// frame_0001.png.. at 30000/1001 fps).
constexpr double kVideoFps = 30000.0 / 1001.0;

class JumpscareDialog : public rex::ui::ImGuiDialog {
 public:
  JumpscareDialog(rex::ui::ImGuiDrawer* drawer, std::filesystem::path mod_root)
      : ImGuiDialog(drawer), mod_root_(std::move(mod_root)) {
    std::srand(static_cast<unsigned>(std::time(nullptr)));
    bool audio_loaded = clip_.LoadAudio(mod_root_ / "assets" / "audio.wav");
    REXLOG_INFO("foxy_jumpscare: mod_root='{}' audio_loaded={}", mod_root_.string(), audio_loaded);
  }

 protected:
  void OnDraw(ImGuiIO& io) override {
    // Retried every frame (not a one-shot ImmediateDrawer-ready callback)
    // because a headless/placeholder ImmediateDrawer is attached first in
    // native rendering mode, whose CreateTexture legitimately returns
    // nullptr (no real GPU backing yet) -- LoadFrames sees 0 frames from
    // that one -- and the real Vulkan/D3D12-backed drawer isn't attached
    // until ~1 second later. Polling until frames actually load handles any
    // number of such attach/replace events instead of assuming exactly one.
    if (!frames_loaded_) {
      if (auto* immediate_drawer = imgui_drawer()->immediate_drawer()) {
        size_t frame_count = clip_.LoadFrames(immediate_drawer, mod_root_ / "assets" / "frames");
        REXLOG_INFO("foxy_jumpscare: loaded {} frame(s)", frame_count);
        if (frame_count > 0) {
          frames_loaded_ = true;
        }
      }
    }

    if (clip_.is_playing()) {
      clip_.Update(io.DeltaTime, ImVec2(0.0f, 0.0f), io.DisplaySize);
      return;
    }

    // Roll once per elapsed real-world second, in a loop so a long stall
    // (e.g. a debugger pause) doesn't skip rolls, just does them all at once
    // on the next frame that catches up.
    seconds_accumulator_ += io.DeltaTime;
    while (seconds_accumulator_ >= 1.0f) {
      seconds_accumulator_ -= 1.0f;
      int32_t chance = REXCVAR_GET(foxy_jumpscare_chance);
      bool hit = chance > 0 && (std::rand() % chance) == 0;
      REXLOG_INFO("foxy_jumpscare: roll chance={} hit={}", chance, hit);
      if (hit) {
        clip_.Play(kVideoFps);
        break;
      }
    }
  }

 private:
  std::filesystem::path mod_root_;
  rex::ui::BakedClipPlayer clip_;
  float seconds_accumulator_ = 0.0f;
  bool frames_loaded_ = false;
};

class JumpscareMod : public rex::system::IModPlugin {
 public:
  explicit JumpscareMod(std::filesystem::path mod_root) : mod_root_(std::move(mod_root)) {}

  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    dialog_ = std::make_unique<JumpscareDialog>(drawer, mod_root_);
  }

 private:
  std::filesystem::path mod_root_;
  std::unique_ptr<JumpscareDialog> dialog_;
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
  return new JumpscareMod(ctx->mod_root ? ctx->mod_root : "");
}
