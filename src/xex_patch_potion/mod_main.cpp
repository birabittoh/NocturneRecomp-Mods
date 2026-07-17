// xex_patch_potion mod - proof of concept: patches the English "Potion"
// (regular Potion, not "High Potion") item *description* in guest memory
// at startup, without touching assets/default.xex on disk or running
// codegen again.
//
// This exists to answer a limitation of the "ship a replacement
// default.xex" approach documented in extracted/README.md: only one mod
// can win that file (mod load order picks a single winner, there's no
// merge). Patching guest memory directly from a small code mod instead
// means any number of mods can each own a different string/byte range with
// no conflict, as long as they don't target the same bytes. See
// mods_src/xex_patch_red_rust for a second, independent patch that
// coexists with this one -- both can be enabled together.
//
// The actual patch logic (unlocking the read-only page, zero-filling the
// field, writing the replacement) is shared with that mod via
// <rexmod/text_patch.h> rather than duplicated -- see that header for why
// each of those steps is necessary.
//
// NOTE: this mod originally targeted the item's *name* field ("POTION")
// instead of its description. That attempt is a cautionary tale left in
// extracted/README.md's "Retargeting to item names" section: name fields
// use a restrictive uppercase-only glyph set (fixed once, via
// ApplyNameFieldPatch), but this specific name field also turned out not
// to end the same way other, safely-patchable names do (no 0xFF
// entry-end marker after it, just a double-null straight into the next
// item's description) -- live testing showed that renders as garbled
// trailing text pulled from whatever memory follows, not the clean
// "Potion"-length text it should be. Retargeted to the description field
// instead, which is plain ASCII with no such issue, matching how the
// original two PoC mods (Pozione/Ruggine Rossa descriptions) worked
// cleanly from the start.
//
// Background on how this string table was found and how it's laid out
// (flat run of null-terminated fields, no length prefix, one block per
// localization) is in extracted/README.md in the main repo.

#include <rex/system/mod_plugin.h>

#include <rex/runtime.h>
#include <rexmod/text_patch.h>

namespace {

// Guest address of the English "Potion" description field, vanilla build
// only. NOTE: this is NOT simply REX_IMAGE_BASE + the string's offset in
// the raw assets/default.xex -- the game always applies assets/default.xexp
// (a title-update delta patch, "XEX patch applied successfully" in the
// startup log) on top of the base image at every launch, which shifts
// everything from this point on by +0x30 bytes relative to the unpatched
// file. See extracted/README.md for the full story.
constexpr uint32_t kPotionDescAddrVanilla = 0x8220fc54u;

// Original text is "Restores some HP \x81muse\x81n" ("Restores some HP
// (use)", where \x81m/\x81n are this font's "(" / ")" glyphs), padded
// with null bytes up to exactly where the item's name field starts right
// after -- 28 bytes total. Plain ASCII, no font-encoding quirks (that's
// only a name-field thing) -- rexmod::ApplyTextPatch is correct here.
constexpr uint32_t kFieldLength = 28;
constexpr char kReplacement[] = "Test1";
static_assert(sizeof(kReplacement) - 1 <= kFieldLength, "replacement too long for this field");

class XexPatchPotionMod : public rex::system::IModPlugin {
 public:
  explicit XexPatchPotionMod(rex::Runtime* runtime) : runtime_(runtime) {}

  // The guest module (default.xex) is fully loaded into guest memory by the
  // time this fires, so the original string is already in place to
  // overwrite -- see docs/making-mods.md's OnModuleLaunched doc comment.
  void OnModuleLaunched() override {
    rexmod::ApplyTextPatch(runtime_, kPotionDescAddrVanilla, kFieldLength, kReplacement);
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
  return new XexPatchPotionMod(ctx->runtime);
}
