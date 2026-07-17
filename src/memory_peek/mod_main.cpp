// memory_peek mod - generic, build-agnostic guest-memory hex viewer.
//
// Demonstrates that a mod can read guest memory through the same public API
// the base game's accent-color reader uses (rex::memory::Memory::
// TranslateVirtual + LookupHeap), with zero hardcoded addresses -- the user
// scrolls a movable window over the entire 4 GB guest address space and can
// search it for typed values. Useful as a quick interactive companion to the
// live-memory-scan RE workflow (see scripts/re/scan_guest_memory.py): once a
// candidate address is found, F10 lets you watch it (and its surrounding
// memory) update in real time without a debugger attached.

#include <rex/system/mod_plugin.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <imgui.h>

#include <rex/runtime.h>
#include <rex/system/xmemory.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>

namespace {

constexpr int kBytesPerRow = 16;
// The guest address space is a full 4 GB (0x00000000-0xFFFFFFFF).
constexpr uint64_t kAddressSpaceSize = 0x100000000ull;
constexpr uint32_t kTotalRows = static_cast<uint32_t>(kAddressSpaceSize / kBytesPerRow);
// Rows scrolled per mouse wheel notch (matches the usual "3 lines" OS default).
constexpr int kWheelRowsPerNotch = 3;

// Caps how many hits a single search keeps around, so searching for a common
// byte value (e.g. 0x00) can't grow the result list without bound.
constexpr size_t kMaxSearchResults = 100000;

enum class SearchDataType {
  kInt8,
  kUInt8,
  kInt16,
  kUInt16,
  kInt32,
  kUInt32,
  kInt64,
  kUInt64,
  kFloat,
  kDouble,
  kString,
  kHexBytes,
  kCount,
};

constexpr const char* kSearchDataTypeNames[] = {
    "Int8", "UInt8", "Int16", "UInt16", "Int32", "UInt32", "Int64", "UInt64",
    "Float", "Double", "String (ASCII)", "Hex Bytes",
};
static_assert(sizeof(kSearchDataTypeNames) / sizeof(kSearchDataTypeNames[0]) ==
              static_cast<size_t>(SearchDataType::kCount));

// Guest memory is big-endian (PowerPC); numeric search values are typed in
// host (little-endian, x86/x64) order and must be byte-swapped before being
// compared against guest bytes.
template <typename T>
T ByteSwap(T value) {
  if constexpr (sizeof(T) == 1) {
    return value;
  } else if constexpr (sizeof(T) == 2) {
    uint16_t v;
    std::memcpy(&v, &value, 2);
    v = __builtin_bswap16(v);
    std::memcpy(&value, &v, 2);
    return value;
  } else if constexpr (sizeof(T) == 4) {
    uint32_t v;
    std::memcpy(&v, &value, 4);
    v = __builtin_bswap32(v);
    std::memcpy(&value, &v, 4);
    return value;
  } else if constexpr (sizeof(T) == 8) {
    uint64_t v;
    std::memcpy(&v, &value, 8);
    v = __builtin_bswap64(v);
    std::memcpy(&value, &v, 8);
    return value;
  }
}

// Parses `text` per `type` and appends the resulting big-endian byte pattern
// to `out`. Returns false if `text` doesn't parse as the selected type.
bool ParseSearchPattern(SearchDataType type, const char* text, std::vector<uint8_t>* out) {
  out->clear();
  if (!text || !*text) {
    return false;
  }

  auto push_swapped = [&](auto value) {
    auto swapped = ByteSwap(value);
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&swapped);
    out->insert(out->end(), bytes, bytes + sizeof(swapped));
  };

  char* end = nullptr;
  switch (type) {
    case SearchDataType::kInt8: {
      long v = std::strtol(text, &end, 10);
      if (end == text) return false;
      out->push_back(static_cast<uint8_t>(static_cast<int8_t>(v)));
      return true;
    }
    case SearchDataType::kUInt8: {
      unsigned long v = std::strtoul(text, &end, 10);
      if (end == text) return false;
      out->push_back(static_cast<uint8_t>(v));
      return true;
    }
    case SearchDataType::kInt16: {
      long v = std::strtol(text, &end, 10);
      if (end == text) return false;
      push_swapped(static_cast<int16_t>(v));
      return true;
    }
    case SearchDataType::kUInt16: {
      unsigned long v = std::strtoul(text, &end, 10);
      if (end == text) return false;
      push_swapped(static_cast<uint16_t>(v));
      return true;
    }
    case SearchDataType::kInt32: {
      long long v = std::strtoll(text, &end, 10);
      if (end == text) return false;
      push_swapped(static_cast<int32_t>(v));
      return true;
    }
    case SearchDataType::kUInt32: {
      unsigned long long v = std::strtoull(text, &end, 10);
      if (end == text) return false;
      push_swapped(static_cast<uint32_t>(v));
      return true;
    }
    case SearchDataType::kInt64: {
      long long v = std::strtoll(text, &end, 10);
      if (end == text) return false;
      push_swapped(static_cast<int64_t>(v));
      return true;
    }
    case SearchDataType::kUInt64: {
      unsigned long long v = std::strtoull(text, &end, 10);
      if (end == text) return false;
      push_swapped(static_cast<uint64_t>(v));
      return true;
    }
    case SearchDataType::kFloat: {
      float v = std::strtof(text, &end);
      if (end == text) return false;
      push_swapped(v);
      return true;
    }
    case SearchDataType::kDouble: {
      double v = std::strtod(text, &end);
      if (end == text) return false;
      push_swapped(v);
      return true;
    }
    case SearchDataType::kString: {
      out->assign(reinterpret_cast<const uint8_t*>(text),
                  reinterpret_cast<const uint8_t*>(text) + std::strlen(text));
      return !out->empty();
    }
    case SearchDataType::kHexBytes: {
      // Accepts "DE AD BE EF" or "DEADBEEF" (spaces optional, case-insensitive).
      std::string digits;
      for (const char* p = text; *p; ++p) {
        if (std::isxdigit(static_cast<unsigned char>(*p))) {
          digits.push_back(*p);
        } else if (!std::isspace(static_cast<unsigned char>(*p))) {
          return false;  // stray non-hex, non-space character
        }
      }
      if (digits.empty() || digits.size() % 2 != 0) {
        return false;
      }
      for (size_t i = 0; i < digits.size(); i += 2) {
        out->push_back(static_cast<uint8_t>(std::strtoul(digits.substr(i, 2).c_str(), nullptr, 16)));
      }
      return true;
    }
    default:
      return false;
  }
}

class MemoryPeekDialog : public rex::ui::ImGuiDialog {
 public:
  MemoryPeekDialog(rex::ui::ImGuiDrawer* drawer, rex::Runtime* runtime)
      : ImGuiDialog(drawer), runtime_(runtime) {
    rex::ui::RegisterBind("bind_memory_peek", "F10", "Toggle memory peek overlay",
                          [this] { visible_ = !visible_; });
    std::snprintf(address_buf_, sizeof(address_buf_), "%08X", 0u);
  }

  ~MemoryPeekDialog() override { rex::ui::UnregisterBind("bind_memory_peek"); }

 protected:
  void OnDraw(ImGuiIO& io) override {
    (void)io;
    if (!visible_) {
      return;
    }

    ImGui::SetNextWindowSize(ImVec2(680, 540), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Memory Peek", &visible_)) {
      ImGui::End();
      return;
    }

    ImFont* mono_font = GetMonoFont();

    ImGui::TextUnformatted("Go to guest virtual address (hex):");
    ImGui::SetNextItemWidth(140);
    if (mono_font) ImGui::PushFont(mono_font, 0.0f);
    bool go = ImGui::InputText("##address", address_buf_, sizeof(address_buf_),
                               ImGuiInputTextFlags_CharsHexadecimal |
                                   ImGuiInputTextFlags_CharsUppercase |
                                   ImGuiInputTextFlags_EnterReturnsTrue);
    if (mono_font) ImGui::PopFont();
    ImGui::SameLine();
    go |= ImGui::Button("Go");
    ImGui::SameLine();
    ImGui::Checkbox("Hide empty rows", &hide_empty_rows_);
    ImGui::SameLine();
    ImGui::Checkbox("Hide zero rows", &hide_zero_rows_);

    if (go) {
      uint32_t goto_address = static_cast<uint32_t>(std::strtoul(address_buf_, nullptr, 16));
      pending_scroll_row_ = static_cast<int>(goto_address / kBytesPerRow);
    }

    auto* memory = runtime_ ? runtime_->memory() : nullptr;
    if (!memory) {
      ImGui::TextDisabled("Runtime memory not available.");
      ImGui::End();
      return;
    }

    DrawSearchControls(memory);

    ImGui::Separator();
    // Push the monospace font before measuring row height so the row count
    // below matches the font actually used to draw the hex dump (the host's
    // default font is a proportional serif face, see
    // NocturnerecompApp::OnConfigureFonts).
    if (mono_font) ImGui::PushFont(mono_font, 0.0f);
    const float row_height = ImGui::GetTextLineHeightWithSpacing();
    // Deliberately NOT a single ImGuiListClipper-backed child spanning the
    // whole 4 GB space: at ~19px/row that's ~5 billion pixels of virtual
    // content height, far past float32's exact-integer range (~16.7M). ImGui
    // (and the renderer) can't represent that faithfully -- fast/erratic
    // scrolling would push cursor/scroll math into the region where float
    // precision breaks down, corrupting layout state (observed as random
    // crashes). It also can't cleanly skip hidden rows (skipping desyncs its
    // fixed-item-height bookkeeping). Instead we keep a small, fixed-size
    // child (no ImGui-owned scrolling at all) and page a `view_base_row_`
    // window over the address space ourselves, re-reading only the rows
    // actually on screen every frame.
    if (ImGui::BeginChild("##hex_scroll_region", ImVec2(0, 0), false)) {
      float avail_height = ImGui::GetContentRegionAvail().y;
      int visible_rows = std::max(1, static_cast<int>(avail_height / row_height));
      uint32_t max_base_row =
          kTotalRows > static_cast<uint32_t>(visible_rows) ? kTotalRows - visible_rows : 0;

      if (pending_scroll_row_ >= 0) {
        // Center the target row in the view rather than pinning it to the top.
        uint32_t target_row = static_cast<uint32_t>(pending_scroll_row_);
        uint32_t half = static_cast<uint32_t>(visible_rows) / 2;
        view_base_row_ = target_row > half ? target_row - half : 0;
        pending_scroll_row_ = -1;
      }
      view_base_row_ = std::min(view_base_row_, max_base_row);

      if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
          ScrollRows(-static_cast<int64_t>(wheel) * kWheelRowsPerNotch);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) {
          ScrollRows(visible_rows);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) {
          ScrollRows(-visible_rows);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Home)) {
          view_base_row_ = 0;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_End)) {
          view_base_row_ = max_base_row;
        }
      }

      if (ImGui::BeginChild("##hex_rows", ImVec2(-24.0f, 0.0f), false,
                            ImGuiWindowFlags_HorizontalScrollbar |
                                ImGuiWindowFlags_NoScrollWithMouse)) {
        // With "Hide empty rows" on, a displayed row can be arbitrarily far
        // (in row count) from the previous one if it sits past a huge
        // unmapped span. Checking hidden-ness one 16-byte row at a time would
        // mean up to millions of LookupHeap/QueryRangeAccess calls to cross a
        // large gap (previously capped per frame, but even the cap made
        // scrolling feel frozen for several frames). QueryRegionInfo reports
        // a whole allocation region (uniform state/protection) in one call,
        // so an entire unmapped region -- however large -- can be skipped in
        // a single jump, the same technique RunSearch already uses.
        int drawn = 0;
        uint64_t row = view_base_row_;
        while (drawn < visible_rows && row < kTotalRows) {
          uint32_t address = static_cast<uint32_t>(row * kBytesPerRow);
          if (hide_empty_rows_) {
            auto* heap = memory->LookupHeap(address);
            rex::memory::HeapAllocationInfo info;
            if (heap && heap->QueryRegionInfo(address, &info) && info.region_size > 0) {
              uint64_t region_end_addr = static_cast<uint64_t>(info.base_address) + info.region_size;
              bool region_readable =
                  heap->QueryRangeAccess(info.base_address,
                                         static_cast<uint32_t>(region_end_addr - 1)) !=
                  rex::memory::PageAccess::kNoAccess;
              if (!region_readable) {
                uint64_t region_end_row = region_end_addr / kBytesPerRow;
                row = region_end_row > row ? region_end_row : row + 1;
                continue;
              }
            }
          }
          if (hide_zero_rows_) {
            // Unlike "Hide empty rows" (unmapped), zero-vs-not-zero is byte
            // content, not allocation state -- QueryRegionInfo can't tell us
            // that, so this has to check each row's bytes individually.
            // Cheap in practice: hide_empty_rows_ already fast-jumps past
            // the huge unmapped stretches, so this only runs over actually
            // mapped memory.
            auto* heap = memory->LookupHeap(address);
            bool readable = heap && heap->QueryRangeAccess(address, address + kBytesPerRow - 1) !=
                                        rex::memory::PageAccess::kNoAccess;
            if (readable) {
              const uint8_t* base = memory->TranslateVirtual<const uint8_t*>(address);
              bool all_zero = true;
              for (int i = 0; i < kBytesPerRow; ++i) {
                if (base[i] != 0) {
                  all_zero = false;
                  break;
                }
              }
              if (all_zero) {
                ++row;
                continue;
              }
            }
          }
          DrawRow(memory, address);
          ++drawn;
          ++row;
        }
        if (drawn < visible_rows && row > view_base_row_) {
          // Ran off the end of the address space while skipping hidden
          // regions -- persist how far we got.
          view_base_row_ = static_cast<uint32_t>(std::min<uint64_t>(row, kTotalRows - 1));
        }
      }
      ImGui::EndChild();

      ImGui::SameLine();

      // A manual proxy scrollbar over `[0, max_base_row]` row indices, not
      // pixel offsets -- unlike ImGui's native content-height scrollbar this
      // never has to represent the full 4 GB space in pixels, so it isn't
      // subject to the float-precision issue described above. With "Hide
      // empty rows" on, some slider positions land in a hidden span, but the
      // region-jump above resolves that to the nearest visible content in
      // one step, so it no longer stalls -- it just snaps forward/back.
      const uint32_t kMinRow = 0;
      uint32_t slider_row = view_base_row_;
      if (ImGui::VSliderScalar("##vscrollbar", ImVec2(20.0f, avail_height), ImGuiDataType_U32,
                               &slider_row, &kMinRow, &max_base_row, "")) {
        view_base_row_ = slider_row;
      }
    }
    ImGui::EndChild();
    if (mono_font) ImGui::PopFont();

    ImGui::End();
  }

 private:
  // Moves the visible window by `delta_rows` (negative = toward address 0),
  // clamped to the address space.
  void ScrollRows(int64_t delta_rows) {
    int64_t new_row = static_cast<int64_t>(view_base_row_) + delta_rows;
    if (new_row < 0) {
      new_row = 0;
    } else if (new_row >= static_cast<int64_t>(kTotalRows)) {
      new_row = static_cast<int64_t>(kTotalRows) - 1;
    }
    view_base_row_ = static_cast<uint32_t>(new_row);
  }

  // The host bakes ImGui's embedded fixed-width font (ProggyClean) into the
  // atlas at startup, right after its own proportional serif UI font -- see
  // NocturnerecompApp::OnConfigureFonts. Fonts *cannot* be added to the atlas
  // at runtime (the GPU texture is only built once, before mods load), so
  // this just looks up what the host already baked instead of creating a
  // font here. Returns null (falling back to the default font) if that
  // second slot isn't present for any reason, rather than risk a crash.
  ImFont* GetMonoFont() {
    ImGuiIO& io = ImGui::GetIO();
    return (io.Fonts && io.Fonts->Fonts.Size > 1) ? io.Fonts->Fonts[1] : nullptr;
  }

  //============================================================================
  // Search
  //============================================================================

  void DrawSearchControls(rex::memory::Memory* memory) {
    ImGui::Separator();
    ImGui::TextUnformatted("Search value:");

    ImGui::SetNextItemWidth(160);
    int type_index = static_cast<int>(search_type_);
    if (ImGui::Combo("##search_type", &type_index, kSearchDataTypeNames,
                     static_cast<int>(SearchDataType::kCount))) {
      search_type_ = static_cast<SearchDataType>(type_index);
    }
    ImGui::SameLine();

    ImGui::SetNextItemWidth(200);
    bool enter_pressed = ImGui::InputText("##search_value", search_value_buf_,
                                          sizeof(search_value_buf_),
                                          ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    bool search_clicked = ImGui::Button("Search");

    if (enter_pressed || search_clicked) {
      RunSearch(memory);
    }

    bool has_results = !search_results_.empty();
    ImGui::BeginDisabled(!has_results);
    if (ImGui::Button("Prev")) {
      StepResult(-1);
    }
    ImGui::SameLine();
    if (ImGui::Button("Next")) {
      StepResult(1);
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy Address")) {
      if (current_result_index_ >= 0 &&
         current_result_index_ < static_cast<int>(search_results_.size())) {
        char addr_text[16];
        std::snprintf(addr_text, sizeof(addr_text), "%08X", search_results_[current_result_index_]);
        ImGui::SetClipboardText(addr_text);
      }
    }
    ImGui::EndDisabled();

    if (!search_status_.empty()) {
      ImGui::SameLine();
      ImGui::TextDisabled("%s", search_status_.c_str());
    }
  }

  void StepResult(int delta) {
    if (search_results_.empty()) {
      return;
    }
    int count = static_cast<int>(search_results_.size());
    current_result_index_ = (current_result_index_ + delta + count) % count;
    uint32_t address = search_results_[current_result_index_];
    pending_scroll_row_ = static_cast<int>(address / kBytesPerRow);
    std::snprintf(address_buf_, sizeof(address_buf_), "%08X", address);
  }

  void RunSearch(rex::memory::Memory* memory) {
    search_results_.clear();
    current_result_index_ = -1;
    search_pattern_size_ = 0;

    std::vector<uint8_t> pattern;
    if (!ParseSearchPattern(search_type_, search_value_buf_, &pattern) || pattern.empty()) {
      search_status_ = "Invalid value for the selected type.";
      return;
    }
    search_pattern_size_ = static_cast<uint32_t>(pattern.size());

    // Walk the guest address space heap by heap. The layout documented by the
    // memory subsystem covers 0x00000000-0xFFFFFFFF with no gaps, so once a
    // heap is found we can safely skip straight to the end of its range.
    // Within a heap, QueryRegionInfo lets us jump over whole allocation
    // regions at once instead of touching every unmapped page.
    uint64_t addr = 0;
    bool truncated = false;
    while (addr <= 0xFFFFFFFFull) {
      uint32_t addr32 = static_cast<uint32_t>(addr);
      auto* heap = memory->LookupHeap(addr32);
      if (!heap) {
        addr += 0x10000;  // defensive fallback; the documented layout has no gaps
        continue;
      }

      uint64_t heap_end = static_cast<uint64_t>(heap->heap_base()) + heap->heap_size();
      uint64_t cursor = addr;
      while (cursor < heap_end) {
        uint32_t cursor32 = static_cast<uint32_t>(cursor);
        rex::memory::HeapAllocationInfo info;
        if (!heap->QueryRegionInfo(cursor32, &info) || info.region_size == 0) {
          cursor += heap->page_size();
          continue;
        }

        uint64_t region_end64 = static_cast<uint64_t>(info.base_address) + info.region_size;
        uint32_t region_end = static_cast<uint32_t>(std::min(region_end64, heap_end));
        if (region_end > info.base_address &&
           region_end - info.base_address >= pattern.size()) {
          bool readable = heap->QueryRangeAccess(info.base_address, region_end - 1) !=
                          rex::memory::PageAccess::kNoAccess;
          if (readable) {
            ScanRegion(memory, info.base_address, region_end, pattern);
            if (search_results_.size() >= kMaxSearchResults) {
              truncated = true;
              break;
            }
          }
        }
        cursor = region_end64 > cursor ? region_end64 : cursor + heap->page_size();
      }
      if (truncated) {
        break;
      }
      addr = heap_end;
    }

    if (search_results_.empty()) {
      search_status_ = "No matches found.";
    } else {
      char status[64];
      std::snprintf(status, sizeof(status), "%zu match%s found%s.", search_results_.size(),
                    search_results_.size() == 1 ? "" : "es", truncated ? " (truncated)" : "");
      search_status_ = status;
      current_result_index_ = 0;
      uint32_t address = search_results_[0];
      pending_scroll_row_ = static_cast<int>(address / kBytesPerRow);
      std::snprintf(address_buf_, sizeof(address_buf_), "%08X", address);
    }
  }

  void ScanRegion(rex::memory::Memory* memory, uint32_t region_start, uint32_t region_end,
                  const std::vector<uint8_t>& pattern) {
    const uint8_t* host = memory->TranslateVirtual<const uint8_t*>(region_start);
    size_t region_size = region_end - region_start;
    size_t pattern_size = pattern.size();
    if (pattern_size > region_size) {
      return;
    }
    for (size_t offset = 0; offset + pattern_size <= region_size; ++offset) {
      if (std::memcmp(host + offset, pattern.data(), pattern_size) == 0) {
        search_results_.push_back(region_start + static_cast<uint32_t>(offset));
        if (search_results_.size() >= kMaxSearchResults) {
          return;
        }
      }
    }
  }

  // Marks which bytes of the row starting at `row_address` fall inside a
  // search match, so DrawRow can highlight them.
  void ComputeRowMatchMask(uint32_t row_address, bool mask[kBytesPerRow]) const {
    std::fill(mask, mask + kBytesPerRow, false);
    if (search_results_.empty() || search_pattern_size_ == 0) {
      return;
    }
    uint32_t row_end = row_address + kBytesPerRow - 1;
    uint32_t lower =
        (row_address >= search_pattern_size_ - 1) ? row_address - (search_pattern_size_ - 1) : 0;
    auto it = std::lower_bound(search_results_.begin(), search_results_.end(), lower);
    for (; it != search_results_.end() && *it <= row_end; ++it) {
      uint32_t match_addr = *it;
      for (uint32_t i = 0; i < search_pattern_size_; ++i) {
        uint32_t byte_addr = match_addr + i;
        if (byte_addr >= row_address && byte_addr <= row_end) {
          mask[byte_addr - row_address] = true;
        }
      }
    }
  }

  //============================================================================
  // Row rendering
  //============================================================================

  void DrawRow(rex::memory::Memory* memory, uint32_t address) {
    // LookupHeap only confirms *some* heap covers the start address -- it says
    // nothing about whether that heap's pages are actually committed, and a
    // heap can span reserved-but-uncommitted (or guard) pages that fault on
    // touch. QueryRangeAccess walks the real page table for the row's span so
    // a bad/half-mapped address shows placeholders instead of crashing.
    auto* heap = memory->LookupHeap(address);
    bool readable = heap && heap->QueryRangeAccess(address, address + kBytesPerRow - 1) !=
                                rex::memory::PageAccess::kNoAccess;

    uint8_t bytes[kBytesPerRow] = {};
    if (readable) {
      const uint8_t* base = memory->TranslateVirtual<const uint8_t*>(address);
      std::memcpy(bytes, base, kBytesPerRow);
    }

    bool match_mask[kBytesPerRow];
    ComputeRowMatchMask(address, match_mask);
    bool row_has_match = std::any_of(match_mask, match_mask + kBytesPerRow, [](bool b) { return b; });

    if (!row_has_match) {
      // Fast path: a single text draw per row keeps scrolling through
      // hundreds of millions of rows smooth when nothing needs highlighting.
      char line[16 + 2 + kBytesPerRow * 3 + 2 + kBytesPerRow + 1];
      int pos = std::snprintf(line, sizeof(line), "%08X  ", address);
      for (int col = 0; col < kBytesPerRow; ++col) {
        pos += readable ? std::snprintf(line + pos, sizeof(line) - pos, "%02X ", bytes[col])
                        : std::snprintf(line + pos, sizeof(line) - pos, "?? ");
      }
      pos += std::snprintf(line + pos, sizeof(line) - pos, " ");
      for (int col = 0; col < kBytesPerRow && pos < static_cast<int>(sizeof(line)) - 1; ++col) {
        char c = readable && bytes[col] >= 0x20 && bytes[col] < 0x7F ? static_cast<char>(bytes[col])
                                                                     : '.';
        line[pos++] = c;
      }
      line[pos] = '\0';

      if (readable) {
        ImGui::TextUnformatted(line);
      } else {
        ImGui::TextDisabled("%s", line);
      }
      return;
    }

    // Slow path: this row contains a search match, so render byte-by-byte to
    // highlight the matched bytes in both the hex and ASCII columns.
    const ImVec4 kMatchColor(1.0f, 0.85f, 0.2f, 1.0f);
    ImGui::PushID(static_cast<int>(address));
    ImGui::Text("%08X  ", address);
    for (int col = 0; col < kBytesPerRow; ++col) {
      ImGui::SameLine(0.0f, 0.0f);
      char cell[4];
      if (readable) {
        std::snprintf(cell, sizeof(cell), "%02X ", bytes[col]);
      } else {
        std::snprintf(cell, sizeof(cell), "?? ");
      }
      if (match_mask[col]) {
        ImGui::TextColored(kMatchColor, "%s", cell);
      } else if (readable) {
        ImGui::TextUnformatted(cell);
      } else {
        ImGui::TextDisabled("%s", cell);
      }
    }
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::TextUnformatted(" ");
    for (int col = 0; col < kBytesPerRow; ++col) {
      ImGui::SameLine(0.0f, 0.0f);
      char c = readable && bytes[col] >= 0x20 && bytes[col] < 0x7F ? static_cast<char>(bytes[col])
                                                                   : '.';
      char cell[2] = {c, '\0'};
      if (match_mask[col]) {
        ImGui::TextColored(kMatchColor, "%s", cell);
      } else if (readable) {
        ImGui::TextUnformatted(cell);
      } else {
        ImGui::TextDisabled("%s", cell);
      }
    }
    ImGui::PopID();
  }

  rex::Runtime* runtime_ = nullptr;
  bool visible_ = false;
  char address_buf_[16] = {};
  int pending_scroll_row_ = 0;  // scroll to address 0 on first open
  uint32_t view_base_row_ = 0;  // row index of the first visible row
  bool hide_empty_rows_ = true;
  bool hide_zero_rows_ = true;

  SearchDataType search_type_ = SearchDataType::kInt32;
  char search_value_buf_[128] = {};
  std::vector<uint32_t> search_results_;  // sorted ascending by construction
  uint32_t search_pattern_size_ = 0;
  int current_result_index_ = -1;
  std::string search_status_;
};

class MemoryPeekMod : public rex::system::IModPlugin {
 public:
  explicit MemoryPeekMod(rex::Runtime* runtime) : runtime_(runtime) {}

  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    dialog_ = std::make_unique<MemoryPeekDialog>(drawer, runtime_);
  }

 private:
  rex::Runtime* runtime_ = nullptr;
  std::unique_ptr<MemoryPeekDialog> dialog_;
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
  return new MemoryPeekMod(ctx->runtime);
}
