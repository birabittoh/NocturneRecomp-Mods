// item_inspector - an overlay (F7) giving a live view of the player-status
// struct for verifying its layout in-game: relic flags, item counts and order,
// equipment slots, stats, plus a log of the player.* events. Relic flags and
// item counts can be edited in place.
//
// It used to be built into NocturneRecomp (src/player_inspector.cpp). Nothing
// here knows a guest address: everything goes through the game's Player API,
// rexmod/nocturne_player_api.h, copied verbatim from the game repo's
// src/api/. The entry points are resolved out of the host executable at load
// time and null-checked, so the mod still loads (and says what is missing) on
// a host too old to have them.

#include <rex/system/mod_plugin.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

#include <imgui.h>

#include <rex/runtime.h>
#include <rex/system/mod_registry.h>
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

constexpr const char* kWindowTitle = "Item inspector##item_inspector";
constexpr size_t kMaxLogLines = 64;

const char* const kSlotNames[NOCTURNE_SLOT_COUNT] = {
    "Right hand", "Left hand", "Head", "Body", "Cape", "Accessory 1", "Accessory 2"};

struct PlayerApi {
  NocturnePlayerAbiVersionFn abi_version = nullptr;
  NocturneGetPlayerStatusFn get_status = nullptr;
  NocturneGetRelicNameFn relic_name = nullptr;
  NocturneGetItemNameFn item_name = nullptr;
  NocturneSetRelicFlagsFn set_relic = nullptr;
  NocturneSetItemCountFn set_item_count = nullptr;

  bool ok() const { return abi_version && get_status && relic_name && item_name; }
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
  Resolve(api.abi_version, "NocturnePlayerAbiVersion");
  Resolve(api.get_status, "NocturneGetPlayerStatus");
  Resolve(api.relic_name, "NocturneGetRelicName");
  Resolve(api.item_name, "NocturneGetItemName");
  Resolve(api.set_relic, "NocturneSetRelicFlags");
  Resolve(api.set_item_count, "NocturneSetItemCount");
  return api;
}

// Event log shared between the registry subscribers (command-processor thread)
// and the dialog (UI thread). Static, so the subscriptions never outlive it.
std::mutex g_log_mutex;
std::deque<std::string> g_log;

void LogLine(std::string line) {
  std::lock_guard lock(g_log_mutex);
  g_log.push_back(std::move(line));
  if (g_log.size() > kMaxLogLines) {
    g_log.pop_front();
  }
}

class ItemInspectorDialog : public rex::ui::ImGuiDialog {
 public:
  ItemInspectorDialog(rex::ui::ImGuiDrawer* drawer, const PlayerApi& api)
      : ImGuiDialog(drawer), api_(api) {
    rex::ui::RegisterBind(
        "bind_item_inspector", "F7", "Toggle item inspector overlay",
        [this] { visible_ = !visible_; }, [this] { return visible_; }, kWindowTitle);
  }
  ~ItemInspectorDialog() override { rex::ui::UnregisterBind("bind_item_inspector"); }

 protected:
  void OnDraw(ImGuiIO&) override {
    if (!visible_) {
      return;
    }
    ImGui::SetNextWindowSize(ImVec2(520, 640), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(kWindowTitle, &visible_)) {
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
      ImGui::TextUnformatted("No save loaded.");
      DrawLog();
      ImGui::End();
      return;
    }

    if (ImGui::BeginTabBar("##tabs")) {
      if (ImGui::BeginTabItem("Relics")) {
        DrawRelics(status);
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Items")) {
        DrawItems(status);
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Equipment / stats")) {
        DrawEquipment(status);
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }
    DrawLog();
    ImGui::End();
  }

 private:
  void DrawRelics(const NocturnePlayerStatus& status) {
    ImGui::TextUnformatted("Bit 0 = found, bit 1 = active. Click a box to toggle.");
    ImGui::BeginDisabled(!api_.set_relic);
    for (int i = 0; i < NOCTURNE_RELIC_COUNT; ++i) {
      uint8_t flags = status.relics[i];
      ImGui::PushID(i);
      bool found = flags & NOCTURNE_RELIC_FOUND;
      bool active = flags & NOCTURNE_RELIC_ACTIVE;
      if (ImGui::Checkbox("##found", &found)) {
        api_.set_relic(i, (flags & ~NOCTURNE_RELIC_FOUND) | (found ? NOCTURNE_RELIC_FOUND : 0));
      }
      ImGui::SameLine();
      if (ImGui::Checkbox("##active", &active)) {
        api_.set_relic(i, (flags & ~NOCTURNE_RELIC_ACTIVE) | (active ? NOCTURNE_RELIC_ACTIVE : 0));
      }
      ImGui::SameLine();
      ImGui::Text("%2d  %-14s  0x%02X", i, api_.relic_name(i), flags);
      ImGui::PopID();
    }
    ImGui::EndDisabled();
    ImGui::Separator();
    ImGui::Text("+0x1E bytes:");
    for (uint8_t b : status.unknown_1e) {
      ImGui::SameLine();
      ImGui::Text("%02X", b);
    }
  }

  void DrawItems(const NocturnePlayerStatus& status) {
    ImGui::SetNextItemWidth(220);
    ImGui::InputTextWithHint("##search", "Search name or id", search_, sizeof(search_));
    ImGui::SameLine();
    // A search covers every item, held or not, so it can find one to give.
    ImGui::BeginDisabled(search_[0] != '\0');
    ImGui::Checkbox("Show all", &show_all_);
    ImGui::EndDisabled();
    ImGui::Text("Raw writes only: the game's order list is not updated (see the API header).");
    if (!ImGui::BeginChild("##items", ImVec2(0, -150))) {
      ImGui::EndChild();
      return;
    }
    ImGui::BeginDisabled(!api_.set_item_count);
    for (int id = 0; id < NOCTURNE_INVENTORY_ITEM_COUNT; ++id) {
      bool hand = id < NOCTURNE_HAND_ITEM_COUNT;
      int index = hand ? id : id - NOCTURNE_HAND_ITEM_COUNT;
      int count = hand ? status.hand_count[index] : status.body_count[index];
      if (search_[0] != '\0' ? !MatchesSearch(id) : count == 0 && !show_all_) {
        continue;
      }
      int order = hand ? status.hand_order[index] : status.body_order[index];
      ImGui::PushID(id);
      if (ImGui::SmallButton("-") && count > 0) {
        api_.set_item_count(id, count - 1);
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("+") && count < NOCTURNE_ITEM_STACK_MAX) {
        api_.set_item_count(id, count + 1);
      }
      ImGui::SameLine();
      ImGui::Text("%3d  %-20s x%-3d order %d", id, api_.item_name(id), count, order);
      ImGui::PopID();
    }
    ImGui::EndDisabled();
    ImGui::EndChild();
  }

  // Case-insensitive substring of the name, or of the id written in decimal.
  bool MatchesSearch(int id) const {
    auto contains = [](const char* text, const char* query) {
      for (; *text; ++text) {
        const char* t = text;
        const char* q = query;
        while (*t && *q &&
               std::tolower(static_cast<unsigned char>(*t)) ==
                   std::tolower(static_cast<unsigned char>(*q))) {
          ++t, ++q;
        }
        if (!*q) {
          return true;
        }
      }
      return false;
    };
    char id_text[8];
    std::snprintf(id_text, sizeof(id_text), "%d", id);
    return contains(api_.item_name(id), search_) || contains(id_text, search_);
  }

  void DrawEquipment(const NocturnePlayerStatus& status) {
    ImGui::TextUnformatted("Slots (dwords from +0x29C):");
    for (int s = 0; s < NOCTURNE_SLOT_COUNT; ++s) {
      int id = status.equipped[s];
      ImGui::Text("  %-12s %3d  %s", kSlotNames[s], id, api_.item_name(id));
    }
    ImGui::Text("Raw dwords from +0x294:");
    for (int i = 0; i < 10; ++i) {
      ImGui::SameLine(0, 8);
      ImGui::Text("%d", status.equip_raw[i]);
    }
    ImGui::Separator();
    const auto& st = status.stats;
    ImGui::Text("HP %d/%d  Hearts %d/%d  MP %d/%d", st.hp, st.hp_max, st.hearts, st.hearts_max,
                st.mp, st.mp_max);
    ImGui::Text("STR %d%+d  CON %d%+d  INT %d%+d  LCK %d%+d  (base+bonus)", st.str_base,
                st.str - st.str_base, st.con_base, st.con - st.con_base, st.int_base,
                st.intl - st.int_base, st.lck_base, st.lck - st.lck_base);
    ImGui::Text("Level %d  Exp %d  Gold %d  Kills %d  Rooms %d", st.level, st.exp, st.gold,
                st.kills, st.rooms);
    ImGui::Text("Play time %d:%02d:%02d.%02d", st.play_hours, st.play_minutes, st.play_seconds,
                st.play_frames);
  }

  void DrawLog() {
    ImGui::Separator();
    ImGui::TextUnformatted("Events");
    if (ImGui::BeginChild("##log", ImVec2(0, 110), ImGuiChildFlags_Borders)) {
      std::lock_guard lock(g_log_mutex);
      for (const auto& line : g_log) {
        ImGui::TextUnformatted(line.c_str());
      }
      if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
        ImGui::SetScrollHereY(1.0f);
      }
    }
    ImGui::EndChild();
  }

  const PlayerApi& api_;
  bool visible_ = false;
  bool show_all_ = false;
  char search_[64] = {};
};

class ItemInspectorMod : public rex::system::IModPlugin {
 public:
  explicit ItemInspectorMod(rex::Runtime* runtime) : runtime_(runtime), api_(LoadPlayerApi()) {}

  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    if (runtime_ && runtime_->mod_registry()) {
      Subscribe(runtime_->mod_registry());
    }
    dialog_ = std::make_unique<ItemInspectorDialog>(drawer, api_);
  }

 private:
  void Subscribe(rex::system::ModRegistry* registry) {
    using Payload = rex::system::ModRegistry::EventPayload;
    auto relic_name = api_.relic_name;
    auto item_name = api_.item_name;
    registry->Subscribe(NOCTURNE_PLAYER_EVENT_SAVE_LOADED,
                        [](const Payload&) { LogLine("save loaded"); });
    registry->Subscribe(NOCTURNE_PLAYER_EVENT_SAVE_UNLOADED,
                        [](const Payload&) { LogLine("save unloaded"); });
    registry->Subscribe(NOCTURNE_PLAYER_EVENT_RELIC_CHANGED, [relic_name](const Payload& p) {
      if (p.bytes.size() < 2) return;
      char buf[96];
      std::snprintf(buf, sizeof(buf), "relic %s: %u -> %u",
                    relic_name ? relic_name(static_cast<int>(p.u64)) : "?", p.bytes[0],
                    p.bytes[1]);
      LogLine(buf);
    });
    registry->Subscribe(NOCTURNE_PLAYER_EVENT_ITEM_CHANGED, [item_name](const Payload& p) {
      if (p.bytes.size() < 2) return;
      char buf[96];
      std::snprintf(buf, sizeof(buf), "item %s: x%u -> x%u",
                    item_name ? item_name(static_cast<int>(p.u64)) : "?", p.bytes[0], p.bytes[1]);
      LogLine(buf);
    });
  }

  rex::Runtime* runtime_ = nullptr;
  PlayerApi api_;
  std::unique_ptr<ItemInspectorDialog> dialog_;
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
  return new ItemInspectorMod(ctx->runtime);
}
