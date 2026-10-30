// player_stats mod: overlay (F6) mirroring the character screen's stats
// live: HP/Heart/MP, STR/CON/INT/LCK, level, exp, gold, kills, rooms, and
// playtime. Mirrors live by default; an "Edit mode" checkbox switches every
// field to an editable input that writes straight back to the game.
//
// Nothing here knows a guest address: everything goes through the game's
// Player API, rexmod/nocturne_player_api.h, copied verbatim from the game
// repo's src/api/. The entry points are resolved out of the host executable
// at load time and null-checked, so the mod still loads (and says what is
// missing) on a host too old to have them.
//
// STR/CON/INT/LCK are shown the way the status screen shows them, base plus
// equipment bonus ("38+14"). Edit mode edits the base value; the bonus comes
// from the equipment.
//
// The "Level Up" button calls NocturneRequestLevelUp, which runs the game's
// own level-up routine on the guest thread's next frame.
//
// A "Game Cleared" checkbox toggles the in-memory game-clear flag, which must
// be non-zero for Richter mode to activate when the player name is
// "richter ".
//
// Two things shown on the same in-game screen are deliberately NOT shown
// here because they haven't been found yet: "NEXT" (exp to next level) and
// "STATUS" (ailment indicator). See the offset table in
// game_symbols/mod_main.cpp for the investigation notes.

#include <rex/system/mod_plugin.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>

#include <imgui.h>

#include <rex/runtime.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>

#include <rexmod/nocturne_player_api.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

struct PlayerApi {
  NocturneGetPlayerStatusFn get_status = nullptr;
  NocturneSetPlayerStatFn set_stat = nullptr;
  NocturneRequestLevelUpFn request_level_up = nullptr;
  NocturneGetGameClearedFn game_cleared = nullptr;
  NocturneSetGameClearedFn set_game_cleared = nullptr;

  bool ok() const { return get_status && set_stat; }
};

void* HostSymbol(const char* symbol) {
#if defined(_WIN32)
  return reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA(nullptr), symbol));
#else
  return dlsym(RTLD_DEFAULT, symbol);
#endif
}

template <typename Fn>
void Resolve(Fn& out, const char* symbol) {
  out = reinterpret_cast<Fn>(HostSymbol(symbol));
}

PlayerApi LoadPlayerApi() {
  PlayerApi api;
  Resolve(api.get_status, "NocturneGetPlayerStatus");
  Resolve(api.set_stat, "NocturneSetPlayerStat");
  Resolve(api.request_level_up, "NocturneRequestLevelUp");
  Resolve(api.game_cleared, "NocturneGetGameCleared");
  Resolve(api.set_game_cleared, "NocturneSetGameCleared");
  return api;
}

class PlayerStatsDialog : public rex::ui::ImGuiDialog {
 public:
  PlayerStatsDialog(rex::ui::ImGuiDrawer* drawer, const PlayerApi& api)
      : ImGuiDialog(drawer), api_(api) {
    rex::ui::RegisterBind(
        "bind_player_stats", "F6", "Toggle player stats overlay", [this] { visible_ = !visible_; },
        [this] { return visible_; }, "Player Stats");
  }

  ~PlayerStatsDialog() override { rex::ui::UnregisterBind("bind_player_stats"); }

 protected:
  void OnDraw(ImGuiIO& io) override {
    (void)io;
    if (!visible_) {
      return;
    }

    ImGui::SetNextWindowSize(ImVec2(280, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Player Stats", &visible_, ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::End();
      return;
    }

    if (!api_.ok()) {
      ImGui::TextWrapped(
          "This version of the game has no Player API (NocturneGetPlayerStatus). Update "
          "NocturneRecomp.");
      ImGui::End();
      return;
    }

    NocturnePlayerStatus status;
    if (api_.get_status(&status) != NOCTURNE_PLAYER_OK) {
      ImGui::TextDisabled("Start or load a game to view stats.");
      ImGui::End();
      return;
    }
    const NocturnePlayerStats& st = status.stats;
    int cleared = api_.game_cleared ? api_.game_cleared() : NOCTURNE_PLAYER_ERR_UNAVAILABLE;

    bool was_edit_mode = edit_mode_;
    ImGui::Checkbox("Edit mode", &edit_mode_);
    if (edit_mode_ && !was_edit_mode) {
      // Seeded once on the way into edit mode, not every frame, so typing into
      // a field doesn't get stomped by the next frame's live re-read.
      edit_ = st;
    }
    if (api_.request_level_up) {
      ImGui::SameLine();
      ImGui::BeginDisabled(st.level >= NOCTURNE_LEVEL_MAX);
      if (ImGui::Button("Level Up")) {
        api_.request_level_up();
      }
      ImGui::EndDisabled();
    }
    ImGui::Separator();

    if (!edit_mode_) {
      DrawPool("HP", "Max##hp", st.hp, st.hp_max, NOCTURNE_STAT_HP);
      DrawPool("MP", "Max##mp", st.mp, st.mp_max, NOCTURNE_STAT_MP);
      DrawPool("HEART", "Max##heart", st.hearts, st.hearts_max, NOCTURNE_STAT_HEARTS);
      ImGui::Separator();
      DrawAttribute("STR", st.str_base, st.str);
      ImGui::SameLine(140);
      DrawAttribute("CON", st.con_base, st.con);
      DrawAttribute("INT", st.int_base, st.intl);
      ImGui::SameLine(140);
      DrawAttribute("LCK", st.lck_base, st.lck);
      ImGui::Separator();
      ImGui::Text("LEVEL %d", st.level);
      ImGui::Text("EXP   %d", st.exp);
      ImGui::Text("GOLD  %d", st.gold);
      ImGui::Text("KILLS %d", st.kills);
      ImGui::Text("ROOMS %d (%.1f%%)", st.rooms, st.rooms * 100.0 / NOCTURNE_ROOM_TOTAL);
      ImGui::Text("TIME  %02d:%02d:%02d", st.play_hours, st.play_minutes, st.play_seconds);
      if (cleared >= 0) {
        ImGui::Separator();
        ImGui::Text("CLEAR %s", cleared ? "YES" : "NO");
      }
    } else {
      ImGui::TextUnformatted("HP");
      ImGui::SameLine(60);
      EditableField("##hp_current", NOCTURNE_STAT_HP, &edit_.hp);
      ImGui::SameLine();
      EditableField("##hp_max", NOCTURNE_STAT_HP_MAX, &edit_.hp_max);

      ImGui::TextUnformatted("MP");
      ImGui::SameLine(60);
      EditableField("##mp_current", NOCTURNE_STAT_MP, &edit_.mp);
      ImGui::SameLine();
      EditableField("##mp_max", NOCTURNE_STAT_MP_MAX, &edit_.mp_max);

      ImGui::TextUnformatted("HEART");
      ImGui::SameLine(60);
      EditableField("##heart_current", NOCTURNE_STAT_HEARTS, &edit_.hearts);
      ImGui::SameLine();
      EditableField("##heart_max", NOCTURNE_STAT_HEARTS_MAX, &edit_.hearts_max);

      ImGui::Separator();

      EditableField("STR", NOCTURNE_STAT_STR_BASE, &edit_.str_base);
      ImGui::SameLine();
      EditableField("CON", NOCTURNE_STAT_CON_BASE, &edit_.con_base);
      EditableField("INT", NOCTURNE_STAT_INT_BASE, &edit_.int_base);
      ImGui::SameLine();
      EditableField("LCK", NOCTURNE_STAT_LCK_BASE, &edit_.lck_base);

      ImGui::Separator();

      EditableField("LEVEL", NOCTURNE_STAT_LEVEL, &edit_.level);
      EditableField("EXP", NOCTURNE_STAT_EXP, &edit_.exp);
      EditableField("GOLD", NOCTURNE_STAT_GOLD, &edit_.gold);
      EditableField("KILLS", NOCTURNE_STAT_KILLS, &edit_.kills);
      EditableField("ROOMS", NOCTURNE_STAT_ROOMS, &edit_.rooms);

      ImGui::TextUnformatted("TIME");
      ImGui::SameLine(60);
      EditableField("##playtime_h", NOCTURNE_STAT_PLAY_HOURS, &edit_.play_hours);
      ImGui::SameLine();
      EditableField("##playtime_m", NOCTURNE_STAT_PLAY_MINUTES, &edit_.play_minutes);
      ImGui::SameLine();
      EditableField("##playtime_s", NOCTURNE_STAT_PLAY_SECONDS, &edit_.play_seconds);
      if (cleared >= 0 && api_.set_game_cleared) {
        bool is_cleared = cleared != 0;
        if (ImGui::Checkbox("CLEAR", &is_cleared)) {
          api_.set_game_cleared(is_cleared ? 1 : 0);
        }
      }
    }

    ImGui::End();
  }

 private:
  // "STR 38+14": base, then the equipment bonus when there is one.
  void DrawAttribute(const char* label, int32_t base, int32_t total) {
    int32_t bonus = total - base;
    if (bonus == 0) {
      ImGui::Text("%-5s %d", label, base);
    } else {
      ImGui::Text("%-5s %d%+d", label, base, bonus);
    }
  }

  // "current/max" with a button that tops current up to max.
  void DrawPool(const char* label, const char* button, int32_t current, int32_t max,
                int current_stat) {
    const float kValueX = 100.0f;
    char value[32];
    std::snprintf(value, sizeof(value), "%4d/%-4d", current, max);
    ImGui::Text("%s", label);
    ImGui::SameLine(kValueX);
    ImGui::TextUnformatted(value);
    ImGui::SameLine();
    ImGui::BeginDisabled(current >= max);
    if (ImGui::Button(button)) {
      api_.set_stat(current_stat, max);
    }
    ImGui::EndDisabled();
  }

  // Editable int field that writes straight back on every change (InputInt
  // returns true on each keystroke that changes the value, same
  // immediate-apply convention as graphics_settings's custom width/height
  // fields). Negative input is clamped to 0, which the API requires.
  void EditableField(const char* label, int stat, int32_t* edit_value) {
    ImGui::SetNextItemWidth(90);
    int value = *edit_value;
    if (ImGui::InputInt(label, &value)) {
      *edit_value = std::max(value, 0);
      api_.set_stat(stat, *edit_value);
    }
  }

  const PlayerApi& api_;
  bool visible_ = false;
  bool edit_mode_ = false;
  NocturnePlayerStats edit_{};
};

class PlayerStatsMod : public rex::system::IModPlugin {
 public:
  PlayerStatsMod() : api_(LoadPlayerApi()) {}

  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    dialog_ = std::make_unique<PlayerStatsDialog>(drawer, api_);
  }

 private:
  PlayerApi api_;
  std::unique_ptr<PlayerStatsDialog> dialog_;
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
  return new PlayerStatsMod();
}
