// Example mod for Minecraft 1.26.62 (iOS): shows how a mod is initialized, logs, uses
// libc++ and replaces a method of the game through the Objective-C runtime.
//
// Objective-C methods are found by name, so this works without the address of a function
// (for C++ functions of the game use emu_hook with an address from a disassembler: the
// game binary minecraftpe is loaded at emu_game_base() = 0x100000000).
#include "emu_mod.h"

#include <string>

extern "C" {
typedef struct objc_class* Class;
typedef struct objc_object* id;
typedef struct objc_selector* SEL;
typedef struct objc_method* Method;
typedef void (*IMP)(void);
Class objc_getClass(const char* name);
SEL sel_registerName(const char* name);
Method class_getInstanceMethod(Class c, SEL s);
IMP method_setImplementation(Method m, IMP imp);
}

namespace {

// -[minecraftpeAppDelegate applicationDidBecomeActive:] (the app comes to the front)
using DidBecomeActive = void (*)(id self, SEL cmd, id application);
DidBecomeActive g_original = nullptr;

void my_did_become_active(id self, SEL cmd, id application) {
    static int count = 0;
    EMU_LOGF("example mod: the game became active (%d)", ++count);
    g_original(self, cmd, application);
}

}  // namespace

__attribute__((constructor)) static void mod_init() {
    std::string name = "Example Mod";
    EMU_LOGF("%s loaded (game at 0x%llx)", name.c_str(), (unsigned long long)emu_game_base());
    emu_set_title("Minecraft 1.26.62 - modded");

    // (the game's classes exist already: mods are loaded and bound together with the game)
    Class c = objc_getClass("minecraftpeAppDelegate");
    Method m = c ? class_getInstanceMethod(c, sel_registerName("applicationDidBecomeActive:")) : nullptr;
    if (m) g_original = (DidBecomeActive)method_setImplementation(m, (IMP)&my_did_become_active);
    else EMU_LOGF("example mod: minecraftpeAppDelegate not found");
}
