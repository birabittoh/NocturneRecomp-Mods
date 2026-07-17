// xex_patch_red_rust mod - proof of concept: patches the English "Red
// Rust" weapon *name* in guest memory at startup, without touching
// assets/default.xex on disk or running codegen again.
//
// Companion to mods_src/xex_patch_potion: both mods patch a different
// guest address in the same string table, and both can be enabled
// simultaneously with no conflict, unlike shipping two competing
// replacement default.xex files (see extracted/README.md).
//
// The actual patch logic (unlocking the read-only page, zero-filling the
// field, writing the replacement) is shared with that mod via
// <rexmod/text_patch.h> rather than duplicated -- see that header for why
// each of those steps is necessary.

#include <rex/system/mod_plugin.h>

#include <rex/runtime.h>
#include <rexmod/text_patch.h>

namespace {

// "Red Rust" is stored as two separate word-fields, "RED" then "RUST",
// each with its own "big first letter" font encoding (first letter
// stored as ASCII-0x20, rest plain uppercase ASCII) -- not the plain-text
// description field the earlier version of this mod patched. Use
// rexmod::ApplyNameFieldPatch (which applies that encoding), not
// ApplyTextPatch, for both -- confirmed the hard way in
// mods_src/xex_patch_potion first: writing plain ASCII into a name field
// via ApplyTextPatch rendered as garbage in-game. Both guest addresses
// are vanilla build only, and NOT simply REX_IMAGE_BASE + the string's
// offset in the raw assets/default.xex -- the game always applies
// assets/default.xexp (a title-update delta patch) over the base image
// at every launch, which shifts everything from this point on by +0x30
// bytes relative to the unpatched file. See extracted/README.md.

// "RED" is 3 bytes followed by a null terminator (4 bytes total) before
// "RUST" starts right after.
constexpr uint32_t kRedAddrVanilla = 0x82211134u;
constexpr uint32_t kRedFieldLength = 4;
constexpr char kRedReplacement[] = "NEW";
static_assert(sizeof(kRedReplacement) - 1 <= kRedFieldLength, "replacement too long for this field");

// "RUST" is exactly 4 bytes with **no** null terminator -- the very next
// byte is 0xFF (this entry's end marker). So this field really is only 4
// bytes; a longer replacement would overwrite that 0xFF and corrupt the
// next entry's parsing.
constexpr uint32_t kRustAddrVanilla = 0x82211138u;
constexpr uint32_t kRustFieldLength = 4;
constexpr char kRustReplacement[] = "TEST";
static_assert(sizeof(kRustReplacement) - 1 <= kRustFieldLength, "replacement too long for this field");

class XexPatchRedRustMod : public rex::system::IModPlugin {
 public:
  explicit XexPatchRedRustMod(rex::Runtime* runtime) : runtime_(runtime) {}

  void OnModuleLaunched() override {
    // Two independent calls -- ApplyNameFieldPatch only ever touches the
    // exact field it's given, so patching both halves of a two-word name
    // is just calling it twice with no extra bookkeeping.
    rexmod::ApplyNameFieldPatch(runtime_, kRedAddrVanilla, kRedFieldLength, kRedReplacement);
    rexmod::ApplyNameFieldPatch(runtime_, kRustAddrVanilla, kRustFieldLength, kRustReplacement);
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
  return new XexPatchRedRustMod(ctx->runtime);
}
