# Building

The build system is a single C file ([../nob.c](../nob.c) with the single-header
[nob.h](https://github.com/tsoding/nob.h)); no shell scripts, no IDE project
files. All build output goes to `build-nob/`.

## Windows (MSVC)

```powershell
# 1. fetch raylib, pinned to the exact commit this port is verified against
git clone https://github.com/raysan5/raylib vendor/raylib
git -C vendor/raylib checkout 1026cb96452e6cf2ccfe5af5c44b1992d0ffae29  # 6.5-dev
cmake -G "Visual Studio 17 2022" -A x64 -DBUILD_EXAMPLES=OFF vendor/raylib -B vendor/raylib/build
cmake --build vendor/raylib/build --config Release --target raylib

# 2. bootstrap the builder once, from "x64 Native Tools Command Prompt for VS 2022"
cl /nologo /Fe:build-nob\nob.exe nob.c

# 3. build from any shell afterwards (nob.exe locates vcvars64 itself)
build-nob\nob.exe              # all targets -> build-nob\
build-nob\nob.exe munkiki      # one target: munkiki | logic_test | scene_shot | fuzz_loader | web
```

Note: `munkiki.exe` locks itself while running, so kill any running instance
before rebuilding, or the linker silently fails (LNK1168) and the next live
test runs against a stale binary.

## Linux (also from WSL2)

The renderer and logic are plain C; only `main.c` touches raylib.  The same
nob builder compiles natively: `$CC` (default `cc`) drives compile/link
exactly like `cl` does on Windows, and raylib is compiled on demand from
`vendor/raylib` into its own `build-linux/` cache (so the MSVC one stays
untouched):

```sh
sudo apt install build-essential cmake pkg-config libx11-dev libgl1-mesa-dev \
     libglx-dev libxrandr-dev libxcursor-dev libxinerama-dev libxi-dev
git clone https://github.com/raysan5/raylib vendor/raylib
git -C vendor/raylib checkout 1026cb96452e6cf2ccfe5af5c44b1992d0ffae29  # 6.5-dev

cc -O1 -o build-nob/nob nob.c    # bootstrap once
./build-nob/nob                  # all targets -> build-nob/
./build-nob/nob scene_shot
./build-nob/scene_shot           # headless verification gates (rc=0 = ok)
```

Under WSL2 the GUI just works via WSLg (the window shows up on the Windows
desktop); the headless frames are byte-identical to the MSVC build
(checked via md5 of all `scene_shot` dumps).  For scriptable input inside
WSL use `xdotool key --window $(xdotool search --name Munkiki) 2`.

## Web build (HTML5/WASM)

The game code is plain portable C; `main.c` hands the frame loop to the
browser (`emscripten_set_main_loop`) exactly as the
[raylib web guide](https://github.com/raysan5/raylib/wiki/Working-for-Web-(HTML5))
recommends. Build it with the Emscripten SDK:

```powershell
# one-time toolchain setup
git clone --depth 1 https://github.com/emscripten-core/emsdk
cd emsdk
emsdk install latest
emsdk activate latest

# from the repo root (emcc on PATH, or just point EMSDK at the emsdk folder)
set EMSDK=C:\path\to\emsdk
build-nob\nob.exe web          # -> build-web\munkiki.html + .js + .wasm
# (identical on Linux: EMSDK=/path/to/emsdk ./build-nob/nob web)
python -m http.server 8080 --directory build-web
```

Then open <http://localhost:8080/munkiki.html>. `nob.exe web` first compiles
the five raylib modules the game uses (rcore, rshapes, rtextures, rtext,
raudio) into `build-web\libraylib.a` (wiki step 2.1), then links the game
against emscripten's GLFW port with our own page ([../web/shell.html](../web/shell.html)):
centered LCD canvas plus the key legend below it.

Progress saves work in the browser: `web/shell.html` seeds an IndexedDB-backed
`/persist` directory before the game starts and `main.c` runs from there, so
the usual `KKM.rms`/`OPT.rms` round-trip survives reloads (the menu gains its
"Continue" item just like on the phone). Browsers only unlock audio after the
first click on the page - click the canvas once if you want the sound.

## Android build (APK)

Requirements: Android SDK with an NDK, build-tools and a platform installed
(Android Studio's default `%LOCALAPPDATA%\Android\Sdk` is auto-detected, or
set `ANDROID_HOME`), plus a JDK on `PATH` (aapt2/zipalign/apksigner/jar run on
the JVM). Then:

```powershell
build-nob\nob.exe android      # -> build-android\munkiki.apk (arm64-v8a + x86_64)
adb install -r build-android\munkiki.apk
```

No Java/Kotlin: the APK is a single `NativeActivity` (android:hasCode=false)
loading `libmain.so` built from raylib (`-DPLATFORM_ANDROID
-DGRAPHICS_API_OPENGL_ES3`) plus `src\*.c`; raylib's own `android_main` calls
our `main()`. The `-Wl,--wrap=fopen` link flag (required by raylib's Android
backend) redirects the `KKM.rms`/`OPT.rms` saves into the app's private data
directory, so progress persists across launches. Verified on a local x86_64
emulator (API 36, WHPX + SwiftShader). The app runs portrait and fills the
screen; below the LCD a virtual NOKIA-style pad appears, driven by
multitouch: the big 1-9/*0# keypad is the centerpiece (`0` smashes, `5`
pushes, `*` pauses/restarts - the real phone digits, remapped onto the
port's PC codes), with a single-line dpad (< ^ v >) and the two soft keys
squeezed above it; SEL starts the game.
