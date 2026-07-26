// macOS-only (§3.1 per-OS pattern): make the process the ACTIVE application.
// An unbundled binary shows its window and receives MOUSE events, but the
// menu bar keeps the launcher and KEYBOARD events never arrive (only the
// active app's key window gets them). On modern macOS the activation is
// COOPERATIVE: a single call made before the runloop has ticked is simply
// ignored — which is why the one-shot attempt (and SDL_RaiseWindow) failed.
// So this returns whether the app is active, and the caller RETRIES from the
// event pump until it sticks.
#import <AppKit/AppKit.h>

extern "C" bool meadowsMacosActivate() {
    if ([NSApp isActive]) {
        return true;
    }
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    // Both spellings: the modern cooperative one and the deprecated
    // ignore-others one — across macOS 13..26 one of the two lands.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    [[NSRunningApplication currentApplication]
        activateWithOptions:NSApplicationActivateIgnoringOtherApps];
    [NSApp activateIgnoringOtherApps:YES];
#pragma clang diagnostic pop
    return [NSApp isActive];
}
