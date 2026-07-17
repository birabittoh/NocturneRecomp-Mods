// player_stats mod - overlay (F5) mirroring the character screen's stats
// live: HP/Heart/MP, STR/CON/INT/LCK, level, exp, gold, kills, rooms, and
// playtime. Mirrors live by default; an "Edit mode" checkbox switches every
// field to an editable input that writes straight back to guest memory.
//
// Looks up the shared "player.stats" base address published by
// mods_src/game_symbols (see its mod_main.cpp for the full struct layout and
// how it was found -- an exact-pattern scan for the STR/CON/INT/LCK sequence
// shown consecutively on the character screen) instead of hardcoding it.
// ROOMS is looked up separately, as "player.rooms" -- it lives at its own
// address, unrelated to the "player.stats" struct (see game_symbols's +0x74
// note for the field that was originally, incorrectly, assumed to be it).
//
// Two things shown on the same in-game screen are deliberately NOT shown
// here because game_symbols wasn't able to find them yet: "NEXT" (exp to
// next level) and "STATUS" (ailment indicator). See the offset table in
// game_symbols/mod_main.cpp for the full investigation notes.
//
// Unlike the screen-stretch rect in mods_src/graphics_settings, these fields
// are ordinary save-file stats, not re-derived by the game every frame, so a
// single write on change sticks -- no per-frame reassertion needed.

#include <rex/system/mod_plugin.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>

#include <imgui.h>

#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/mod_registry.h>
#include <rex/system/xmemory.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>

namespace {

// Offsets into the "player.stats" struct published by game_symbols. Only the
// fields game_symbols was able to identify are read here; see that mod's
// mod_main.cpp for the full offset table, including the unidentified gaps.
constexpr uint32_t kHpCurrentOffset = 0x00;
constexpr uint32_t kHpMaxOffset = 0x04;
constexpr uint32_t kHeartCurrentOffset = 0x08;
constexpr uint32_t kHeartMaxOffset = 0x0c;
constexpr uint32_t kMpCurrentOffset = 0x10;
constexpr uint32_t kMpMaxOffset = 0x14;
// First of two identical-so-far STR/CON/INT/LCK copies (+0x18 and +0x38) --
// see game_symbols/mod_main.cpp: which copy is "base" vs "equipped/
// effective" isn't disambiguated yet, so this mirrors copy #1 arbitrarily.
constexpr uint32_t kStrOffset = 0x18;
constexpr uint32_t kConOffset = 0x1c;
constexpr uint32_t kIntOffset = 0x20;
constexpr uint32_t kLckOffset = 0x24;
constexpr uint32_t kLevelOffset = 0x48;
constexpr uint32_t kExpOffset = 0x4c;
constexpr uint32_t kGoldOffset = 0x50;
constexpr uint32_t kKillsOffset = 0x54;
// NOT rooms -- see game_symbols/mod_main.cpp's +0x74 note: this was
// originally assumed to be the on-screen ROOMS counter because it happened
// to read the same value, but writing to it (via this mod's now-removed
// edit-mode field) didn't move ROOMS at all and instead made STR/CON/INT/
// LCK jump around, consistent with some stat-growth/level table index
// instead. Shown read-only, unlabeled as a stat, and never editable until
// it's properly re-identified.
constexpr uint32_t kUnidentifiedOffset0x74 = 0x74;
constexpr uint32_t kPlaytimeHoursOffset = 0x90;
constexpr uint32_t kPlaytimeMinutesOffset = 0x94;
constexpr uint32_t kPlaytimeSecondsOffset = 0x98;
constexpr uint32_t kSpanSize = kPlaytimeSecondsOffset + 4;  // covers every field read here

uint32_t ReadGuestU32BE(rex::memory::Memory* memory, uint32_t guest_address) {
  const uint8_t* host_address = memory->TranslateVirtual<const uint8_t*>(guest_address);
  return rex::memory::load_and_swap<uint32_t>(host_address);
}

void WriteGuestU32BE(rex::memory::Memory* memory, uint32_t guest_address, uint32_t value) {
  uint8_t* host_address = memory->TranslateVirtual<uint8_t*>(guest_address);
  rex::memory::store_and_swap<uint32_t>(host_address, value);
}

class PlayerStatsDialog : public rex::ui::ImGuiDialog {
 public:
  PlayerStatsDialog(rex::ui::ImGuiDrawer* drawer, rex::Runtime* runtime)
      : ImGuiDialog(drawer), runtime_(runtime) {
    rex::ui::RegisterBind("bind_player_stats", "F5", "Toggle player stats overlay",
                          [this] { visible_ = !visible_; });
  }

  ~PlayerStatsDialog() override { rex::ui::UnregisterBind("bind_player_stats"); }

  // Called once KernelState/the executable module are live (see
  // PlayerStatsMod::OnModuleLaunched). Looks up the address published by
  // game_symbols, which resolves vanilla vs TU for us.
  void ResolveAddress() {
    if (runtime_ && runtime_->mod_registry()) {
      if (auto addr = runtime_->mod_registry()->FindAddress("player.stats")) {
        addr_ = *addr;
        addr_resolved_ = true;
      }
      if (auto addr = runtime_->mod_registry()->FindAddress("player.rooms")) {
        rooms_addr_ = *addr;
        rooms_addr_resolved_ = true;
      }
    }
  }

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

    auto* memory = runtime_ ? runtime_->memory() : nullptr;
    auto* heap = (memory && addr_resolved_) ? memory->LookupHeap(addr_) : nullptr;
    bool readable = heap && heap->QueryRangeAccess(addr_, addr_ + kSpanSize - 1) !=
                                rex::memory::PageAccess::kNoAccess;

    if (!addr_resolved_ || !memory || !readable) {
      ImGui::TextDisabled("Start or load a game to view stats.");
      ImGui::End();
      return;
    }

    uint32_t hp_current = ReadGuestU32BE(memory, addr_ + kHpCurrentOffset);
    uint32_t hp_max = ReadGuestU32BE(memory, addr_ + kHpMaxOffset);
    uint32_t heart_current = ReadGuestU32BE(memory, addr_ + kHeartCurrentOffset);
    uint32_t heart_max = ReadGuestU32BE(memory, addr_ + kHeartMaxOffset);
    uint32_t mp_current = ReadGuestU32BE(memory, addr_ + kMpCurrentOffset);
    uint32_t mp_max = ReadGuestU32BE(memory, addr_ + kMpMaxOffset);
    uint32_t str = ReadGuestU32BE(memory, addr_ + kStrOffset);
    uint32_t con = ReadGuestU32BE(memory, addr_ + kConOffset);
    uint32_t intelligence = ReadGuestU32BE(memory, addr_ + kIntOffset);
    uint32_t lck = ReadGuestU32BE(memory, addr_ + kLckOffset);
    uint32_t level = ReadGuestU32BE(memory, addr_ + kLevelOffset);
    uint32_t exp = ReadGuestU32BE(memory, addr_ + kExpOffset);
    uint32_t gold = ReadGuestU32BE(memory, addr_ + kGoldOffset);
    uint32_t kills = ReadGuestU32BE(memory, addr_ + kKillsOffset);
    uint32_t unidentified_0x74 = ReadGuestU32BE(memory, addr_ + kUnidentifiedOffset0x74);
    uint32_t playtime_hours = ReadGuestU32BE(memory, addr_ + kPlaytimeHoursOffset);
    uint32_t playtime_minutes = ReadGuestU32BE(memory, addr_ + kPlaytimeMinutesOffset);
    uint32_t playtime_seconds = ReadGuestU32BE(memory, addr_ + kPlaytimeSecondsOffset);

    // "player.rooms" is a separate address from the "player.stats" struct
    // above (see game_symbols/mod_main.cpp), so it gets its own resolved-ness
    // and readability check rather than sharing addr_'s.
    bool rooms_readable = false;
    uint32_t rooms = 0;
    if (rooms_addr_resolved_) {
      auto* rooms_heap = memory->LookupHeap(rooms_addr_);
      rooms_readable = rooms_heap && rooms_heap->QueryRangeAccess(rooms_addr_, rooms_addr_ + 3) !=
                                          rex::memory::PageAccess::kNoAccess;
      if (rooms_readable) {
        rooms = ReadGuestU32BE(memory, rooms_addr_);
      }
    }

    bool was_edit_mode = edit_mode_;
    ImGui::Checkbox("Edit mode", &edit_mode_);
    if (edit_mode_ && !was_edit_mode) {
      SeedEditFields(hp_current, hp_max, heart_current, heart_max, mp_current, mp_max, str, con,
                    intelligence, lck, level, exp, gold, kills, playtime_hours,
                    playtime_minutes, playtime_seconds);
      if (rooms_readable) {
        edit_rooms_ = static_cast<int>(rooms);
      }
    }
    ImGui::Separator();

    if (!edit_mode_) {
      ImGui::Text("HP    %u/%u", hp_current, hp_max);
      ImGui::Text("HEART %u/%u", heart_current, heart_max);
      ImGui::Text("MP    %u/%u", mp_current, mp_max);
      ImGui::Separator();
      ImGui::Text("STR %u  CON %u  INT %u  LCK %u", str, con, intelligence, lck);
      ImGui::Separator();
      ImGui::Text("LEVEL %u", level);
      ImGui::Text("EXP   %u", exp);
      ImGui::Text("GOLD  %u", gold);
      ImGui::Text("KILLS %u", kills);
      if (rooms_readable) {
        ImGui::Text("ROOMS %u", rooms);
      }
      ImGui::Text("TIME  %02u:%02u:%02u", playtime_hours, playtime_minutes, playtime_seconds);
    } else {
      ImGui::TextUnformatted("HP");
      ImGui::SameLine(60);
      EditableField(memory, "##hp_current", kHpCurrentOffset, &edit_hp_current_);
      ImGui::SameLine();
      EditableField(memory, "##hp_max", kHpMaxOffset, &edit_hp_max_);

      ImGui::TextUnformatted("HEART");
      ImGui::SameLine(60);
      EditableField(memory, "##heart_current", kHeartCurrentOffset, &edit_heart_current_);
      ImGui::SameLine();
      EditableField(memory, "##heart_max", kHeartMaxOffset, &edit_heart_max_);

      ImGui::TextUnformatted("MP");
      ImGui::SameLine(60);
      EditableField(memory, "##mp_current", kMpCurrentOffset, &edit_mp_current_);
      ImGui::SameLine();
      EditableField(memory, "##mp_max", kMpMaxOffset, &edit_mp_max_);

      ImGui::Separator();

      EditableField(memory, "STR", kStrOffset, &edit_str_);
      ImGui::SameLine();
      EditableField(memory, "CON", kConOffset, &edit_con_);
      EditableField(memory, "INT", kIntOffset, &edit_int_);
      ImGui::SameLine();
      EditableField(memory, "LCK", kLckOffset, &edit_lck_);

      ImGui::Separator();

      EditableField(memory, "LEVEL", kLevelOffset, &edit_level_);
      EditableField(memory, "EXP", kExpOffset, &edit_exp_);
      EditableField(memory, "GOLD", kGoldOffset, &edit_gold_);
      EditableField(memory, "KILLS", kKillsOffset, &edit_kills_);
      if (rooms_readable) {
        EditableFieldAbs(memory, "ROOMS", rooms_addr_, &edit_rooms_);
      }

      ImGui::TextUnformatted("TIME");
      ImGui::SameLine(60);
      EditableField(memory, "##playtime_h", kPlaytimeHoursOffset, &edit_playtime_hours_);
      ImGui::SameLine();
      EditableField(memory, "##playtime_m", kPlaytimeMinutesOffset, &edit_playtime_minutes_);
      ImGui::SameLine();
      EditableField(memory, "##playtime_s", kPlaytimeSecondsOffset, &edit_playtime_seconds_);
    }

    ImGui::End();
  }

 private:
  // Seeds every edit_* field from the just-read live values -- called once
  // on the transition into edit mode, not every frame, so typing into a
  // field doesn't get stomped by the next frame's live re-read.
  void SeedEditFields(uint32_t hp_current, uint32_t hp_max, uint32_t heart_current,
                      uint32_t heart_max, uint32_t mp_current, uint32_t mp_max, uint32_t str,
                      uint32_t con, uint32_t intelligence, uint32_t lck, uint32_t level,
                      uint32_t exp, uint32_t gold, uint32_t kills, uint32_t playtime_hours,
                      uint32_t playtime_minutes, uint32_t playtime_seconds) {
    edit_hp_current_ = static_cast<int>(hp_current);
    edit_hp_max_ = static_cast<int>(hp_max);
    edit_heart_current_ = static_cast<int>(heart_current);
    edit_heart_max_ = static_cast<int>(heart_max);
    edit_mp_current_ = static_cast<int>(mp_current);
    edit_mp_max_ = static_cast<int>(mp_max);
    edit_str_ = static_cast<int>(str);
    edit_con_ = static_cast<int>(con);
    edit_int_ = static_cast<int>(intelligence);
    edit_lck_ = static_cast<int>(lck);
    edit_level_ = static_cast<int>(level);
    edit_exp_ = static_cast<int>(exp);
    edit_gold_ = static_cast<int>(gold);
    edit_kills_ = static_cast<int>(kills);
    edit_playtime_hours_ = static_cast<int>(playtime_hours);
    edit_playtime_minutes_ = static_cast<int>(playtime_minutes);
    edit_playtime_seconds_ = static_cast<int>(playtime_seconds);
  }

  // Editable int field that writes straight back to guest memory on every
  // change (InputInt returns true on each keystroke that changes the value,
  // same immediate-apply convention as mods_src/graphics_settings's custom
  // width/height fields). Negative input is clamped to 0 before writing --
  // the field is a uint32 in memory.
  void EditableField(rex::memory::Memory* memory, const char* label, uint32_t offset,
                      int* edit_value) {
    EditableFieldAbs(memory, label, addr_ + offset, edit_value);
  }

  // Same as EditableField, but for an address that isn't an offset into the
  // "player.stats" struct -- e.g. "player.rooms", which is its own
  // standalone address elsewhere in memory (see game_symbols/mod_main.cpp).
  void EditableFieldAbs(rex::memory::Memory* memory, const char* label, uint32_t guest_address,
                        int* edit_value) {
    ImGui::SetNextItemWidth(90);
    if (ImGui::InputInt(label, edit_value)) {
      *edit_value = std::max(*edit_value, 0);
      WriteGuestU32BE(memory, guest_address, static_cast<uint32_t>(*edit_value));
    }
  }

  rex::Runtime* runtime_ = nullptr;
  bool visible_ = false;

  bool addr_resolved_ = false;
  uint32_t addr_ = 0;

  bool rooms_addr_resolved_ = false;
  uint32_t rooms_addr_ = 0;

  bool edit_mode_ = false;
  int edit_hp_current_ = 0;
  int edit_hp_max_ = 0;
  int edit_heart_current_ = 0;
  int edit_heart_max_ = 0;
  int edit_mp_current_ = 0;
  int edit_mp_max_ = 0;
  int edit_str_ = 0;
  int edit_con_ = 0;
  int edit_int_ = 0;
  int edit_lck_ = 0;
  int edit_level_ = 0;
  int edit_exp_ = 0;
  int edit_gold_ = 0;
  int edit_kills_ = 0;
  int edit_rooms_ = 0;
  int edit_playtime_hours_ = 0;
  int edit_playtime_minutes_ = 0;
  int edit_playtime_seconds_ = 0;
};

class PlayerStatsMod : public rex::system::IModPlugin {
 public:
  explicit PlayerStatsMod(rex::Runtime* runtime) : runtime_(runtime) {}

  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    dialog_ = std::make_unique<PlayerStatsDialog>(drawer, runtime_);
  }

  void OnModuleLaunched() override {
    if (dialog_) {
      dialog_->ResolveAddress();
    }
  }

 private:
  rex::Runtime* runtime_ = nullptr;
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
  return new PlayerStatsMod(ctx->runtime);
}
