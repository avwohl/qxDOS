# iosFreeDOS2 - DOSBox-based FreeDOS for iOS/Mac

## Project Overview

FreeDOS emulator for iOS and Mac, powered by DOSBox Staging.
Full 386+FPU, DPMI, VGA/SVGA, Sound Blaster 16.

## Architecture

```
SwiftUI Views
  └─ DOSEmulator.h/.mm (Objective-C++ bridge)
       └─ DOSBox-staging (git submodule at dosbox-staging/)
            ├─ CPU: 386/486/Pentium with FPU, dynamic recompiler
            ├─ DOS: kernel, DPMI, drives, shell
            ├─ Hardware: VGA, Sound Blaster, keyboard, mouse
            └─ SDL2: video output, audio, input events
```

## Build

1. Prerequisites: Xcode 15+, CMake, XcodeGen
2. `git submodule update --init` to fetch dosbox-staging
3. Build DOSBox as static library via cmake (see dosbox-ios/CMakeLists.txt)
4. `xcodegen` to generate Xcode project
5. Open iosFreeDOS.xcodeproj and build

## Key Directories

- `iosFreeDOS/` - SwiftUI app (Views, Bridge, Assets)
- `dosbox-staging/` - DOSBox source (git submodule)
- `dosbox-ios/` - iOS-specific DOSBox integration layer
- `fd/` - FreeDOS disk images (gitignored, built by scripts/)
- `dos/` - DOS guest utilities (R.COM, W.COM, DPMITEST.COM)
- `release_assets/` - Catalog XML, help files for GitHub releases
- `docs/` - Reference documentation

## Disk Catalog Consistency

Three files must stay in sync when disk images change:
- `fd/*.img` - the actual disk images (gitignored, built by scripts/)
- `iosFreeDOS/Resources/disks.xml` - bundled catalog (fallback in app)
- `release_assets/disks.xml` - catalog published with GitHub releases

When a disk image is rebuilt, check that:
1. The `<size>` in both disks.xml files matches the actual file size
2. The `<sha256>` matches `shasum -a 256 fd/<image>`
3. Both disks.xml copies are identical
4. The `<catalog version>` is bumped so clients re-download

If any are stale, update them. The build script `scripts/build_starter_disk.sh`
runs a consistency check at the end and warns on mismatches.

## DOS Text Files

Files written to DOS disk images (AUTOEXEC.BAT, FDCONFIG.SYS, .BAT, .TXT, etc.)
MUST use DOS line endings (`\r\n`, CR+LF). Shell heredocs produce Unix `\n` which
FreeDOS cannot parse — commands won't run, PATH won't be set, etc.
Use `printf 'line1\r\nline2\r\n'` instead of heredocs when writing DOS text files.

## Bumping the Build Number

1. Edit `CURRENT_PROJECT_VERSION` in `project.yml`
2. Run `xcodegen` to regenerate the Xcode project

Both steps are required — the xcodeproj is gitignored and built from project.yml.

## XcodeGen

After modifying `project.yml`, run `xcodegen` yourself — never ask the user to do it.

## Development Notes

- DOSBox uses C++20, SDL2
- For iOS: disabled debugger, webserver, FluidSynth, MT32
- SDL2 provides iOS support (Metal rendering)
- The bridge writes a temporary dosbox.conf and launches DOSBox
- Frame capture via custom GFX callback → delegate → SwiftUI
- ZIPFoundation SPM dependency for extracting downloaded zip archives
