# Main Thread Checker Warning: UIView bounds

## Symptom

Xcode's Main Thread Checker fires a purple runtime warning:

    UIView bounds must be used from main thread only

This appears during normal emulation, not during init or teardown.

## Root Cause

dosbox_run() executes on a background dispatch queue (_emulatorQueue).
The DOSBox emulation loop calls SDL rendering functions that internally
access UIView bounds, which UIKit requires to happen on the main thread.

## Call Path

    dosbox_run()                          [background thread]
      normal_loop()                       dosbox.cpp:113
        GFX_MaybePresentFrame()           sdl_gui.cpp:2442
          SdlRenderer::PresentFrame()     sdl_renderer.cpp:341
            SDL_RenderPresent(renderer)   sdl_renderer.cpp:357
              UIView bounds access        UIKit violation

## Other Affected SDL Calls

    SdlRenderer::GetCanvasSizeInPixels()  sdl_renderer.cpp:157
      SDL_GetWindowSizeInPixels()

    check_and_handle_dpi_change()         sdl_gui.cpp:643
      SDL_GetWindowSize()

    update_viewport()                     sdl_gui.cpp:886
      calls GetCanvasSizeInPixels()

    handle_sdl_windowevent()              sdl_gui.cpp:2195+
      SDL_WINDOWEVENT_SIZE_CHANGED, DISPLAY_CHANGED, RESIZED
      all call check_and_handle_dpi_change() or update_viewport()

## Why It Happens

DOSBox-staging assumes it owns the process and runs on the main thread.
In our embedded architecture, dosbox_init() runs on the main thread
(SDL/UIKit setup) but dosbox_run() runs on a background queue so the
main thread stays free for SwiftUI.  The emulation loop's SDL rendering
calls then touch UIKit from the wrong thread.

## Impact

- Debug only: Main Thread Checker is a diagnostic tool, disabled in
  release builds.  It will not fire for App Store reviewers or users.
- SDL2's Metal backend manages its own synchronization, so the actual
  risk of corruption or crashes on real devices is low.
- No App Store rejection risk from this issue.

## Fix Options

1. Leave as-is (recommended for now)
   No impact on release builds or App Store review.

2. Disable Main Thread Checker in Xcode scheme
   Edit Scheme > Run > Diagnostics > uncheck Main Thread Checker.
   Hides the warning but also hides any other legitimate violations.

3. Dispatch rendering to main thread (major refactor)
   Split the emulation loop so CPU/DOS work stays on the background
   thread but all SDL_Render* and SDL_GetWindow* calls dispatch to the
   main thread.  This would require:
   - A render queue or main-thread trampoline in GFX_MaybePresentFrame
   - Synchronization to pass frame data between threads
   - Careful handling of window events (already on background thread)
   - Performance testing to ensure frame dispatch latency is acceptable
   Estimated scope: significant, touches sdl_gui.cpp, sdl_renderer.cpp,
   and the normal_loop() in dosbox.cpp.

4. Run dosbox_run() on the main thread
   Would eliminate the threading issue entirely but blocks the main
   thread, preventing SwiftUI from updating (no touch controls overlay,
   no quit button, no app lifecycle handling).  Not viable.

## Decision (2026-03-26)

Option 1: leave as-is.  The warning is debug-only and does not affect
users or App Store review.  Revisit if real-device crashes are observed.
