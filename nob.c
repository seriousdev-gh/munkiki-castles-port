// nob.c - single-file build of munkiki-castle (https://github.com/tsoding/nob.h, v3.10).
// Bootstrap once per platform:
//   Windows (from "x64 Native Tools Command Prompt for VS 2022"):
//     cl /nologo /Fe:build-nob\nob.exe nob.c
//   Linux/POSIX:
//     cc -o build-nob/nob nob.c
// After that run build-nob/nob from ANY shell - on Windows it locates
// vcvars64.bat and imports the MSVC environment itself (same-language rule:
// no shell wrapper scripts). It also rebuilds itself when nob.c changes.
#define NOB_IMPLEMENTATION
#include "nob.h"

#define COUNT(a) (sizeof(a)/sizeof((a)[0]))

#ifdef _WIN32
#  define SEP    "\\"
#  define EXE    ".exe"
#  define OBJEXT ".obj"
#else
#  define SEP    "/"
#  define EXE    ""
#  define OBJEXT ".o"
#endif
#define BUILD_FOLDER "build-nob" SEP
#define RAYLIB_DIR   "vendor" SEP "raylib" SEP

#include <stdlib.h>
#ifndef _WIN32
#include <unistd.h>   /* sysconf: parallel raylib build */
#endif
#ifdef _WIN32

// If cl.exe is not on PATH, find vcvars64.bat, capture the environment it
// exports and apply it to this process. This keeps nob usable from a plain
// PowerShell/cmd without launching a Developer Prompt first.
static void setup_msvc_env(void)
{
    if (system("where cl.exe >nul 2>nul") == 0) return;

    static const char *editions[] = {
        "Community", "Professional", "Enterprise", "BuildTools", "Preview",
    };
    const char *pf[] = { getenv("ProgramFiles"), getenv("ProgramFiles(x86)") };
    static char vcvars[1024] = {0};
    for (int p = 0; p < 2 && !vcvars[0]; p++) {
        if (!pf[p]) continue;
        for (size_t e = 0; e < COUNT(editions); e++) {
            char cand[1024];
            snprintf(cand, sizeof(cand),
                "%s\\Microsoft Visual Studio\\2022\\%s\\VC\\Auxiliary\\Build\\vcvars64.bat",
                pf[p], editions[e]);
            if (nob_file_exists(cand)) {
                snprintf(vcvars, sizeof(vcvars), "%s", cand);
                break;
            }
        }
    }
    if (!vcvars[0]) {
        nob_log(ERROR, "cl.exe not on PATH and vcvars64.bat was not found; "
                       "install VS2022 C++ workload or run from a Developer Prompt");
        exit(1);
    }

    // "call vcvars64.bat && set" prints the resulting environment; feed each
    // KEY=VALUE line back via _putenv_s.
    const char *dump = BUILD_FOLDER "vcvars.env";
    if (!nob_mkdir_if_not_exists(BUILD_FOLDER)) exit(1);
    char syscmd[1400];
    snprintf(syscmd, sizeof(syscmd),
        "call \"%s\" >nul 2>nul && set > \"%s\"", vcvars, dump);
    nob_log(INFO, "importing MSVC environment from %s", vcvars);
    if (system(syscmd) != 0) {
        nob_log(ERROR, "failed to run vcvars64.bat");
        exit(1);
    }
    Nob_String_Builder sb = {0};
    if (!nob_read_entire_file(dump, &sb)) { nob_log(ERROR, "cannot read %s", dump); exit(1); }
    nob_sb_append_null(&sb);
    char *line = sb.items;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char *cr = strchr(line, '\r');
        if (cr) *cr = 0;
        char *eq = strchr(line, '=');
        if (eq && line != eq && line[0] != ' ') {
            *eq = 0;
            _putenv_s(line, eq + 1);
            *eq = '=';
        }
        line = nl ? nl + 1 : NULL;
    }
    nob_sb_free(sb);
    if (!nob_delete_file(dump)) exit(1);
    if (system("where cl.exe >nul 2>nul") != 0) {
        nob_log(ERROR, "vcvars64.bat ran but cl.exe is still not on PATH");
        exit(1);
    }
}
#endif // _WIN32

// --- web (emscripten) target ------------------------------------------------------------
// Requires the Emscripten SDK (emsdk install/activate latest); emcc must be on
// PATH. On Windows run emsdk\emsdk_env.bat first, or point EMSDK at the emsdk
// folder and we will find emcc under %EMSDK%\upstream\emscripten ourselves.
#define WEB_FOLDER "build-web" SEP

static const char *find_emcc(const char *tool)
{
    char probe[512];
#ifdef _WIN32
    snprintf(probe, sizeof(probe), "where %s >nul 2>nul", tool);
#else
    snprintf(probe, sizeof(probe), "command -v %s >/dev/null 2>&1", tool);
#endif
    if (system(probe) == 0) return tool;
    const char *emsdk = getenv("EMSDK");
    if (emsdk && *emsdk) {
        static char full[4][1024];   // 4 tools alive at once; never more
        static int slot = 0;
        char *res = full[slot = (slot + 1) % 4];
#ifdef _WIN32
        snprintf(res, sizeof(full[0]), "%s\\upstream\\emscripten\\%s.exe", emsdk, tool);
#else
        snprintf(res, sizeof(full[0]), "%s/upstream/emscripten/%s", emsdk, tool);
#endif
        if (nob_file_exists(res)) return res;
    }
    return NULL;
}

#include <sys/stat.h>
static int file_newer(const char *a, const char *b)
{
    struct stat sa, sb;
    if (stat(a, &sa) != 0) return 1;   // missing b -> rebuild
    if (stat(b, &sb) != 0) return 1;
    return sa.st_mtime > sb.st_mtime;
}

// Compile the raylib subset the game needs to build-web\libraylib.a
// (raylib "Working for Web" wiki, step 2.1 - we skip rmodels, we never use it).
static bool build_web_raylib(Nob_Cmd *cmd)
{
    const char *lib = WEB_FOLDER "libraylib.a";
    static const char *const mods[] = {"rcore", "rshapes", "rtextures", "rtext", "raudio"};
    const char *emcc = find_emcc("emcc"), *emar = find_emcc("emar");
    if (!emcc || !emar) {
        nob_log(ERROR, "emcc/emar not found; install Emscripten SDK:");
        nob_log(ERROR, "  git clone --depth 1 https://github.com/emscripten-core/emsdk && cd emsdk");
        nob_log(ERROR, "  emsdk install latest && emsdk activate latest && emsdk_env.bat");
        return false;
    }
    if (!nob_mkdir_if_not_exists(WEB_FOLDER)) return false;
    // A header edit must rebuild the objects too, not just a .c edit:
    // compare the public raylib headers against each cached .o.
    static const char *const hdrs[] = {"raylib.h", "rlgl.h", "raymath.h", "rcamera.h"};
    for (size_t i = 0; i < COUNT(mods); i++) {
        char *obj = nob_temp_sprintf(WEB_FOLDER "%s.o", mods[i]);
        bool stale = !nob_file_exists(obj) ||
                     file_newer(nob_temp_sprintf(RAYLIB_DIR "src" SEP "%s.c", mods[i]), obj);
        for (size_t k = 0; !stale && k < COUNT(hdrs); k++)
            stale = file_newer(nob_temp_sprintf(RAYLIB_DIR "src" SEP "%s", hdrs[k]), obj);
        if (!stale) continue;
        nob_cmd_append(cmd, emcc, "-c", nob_temp_sprintf(RAYLIB_DIR "src" SEP "%s.c", mods[i]),
            "-Os", "-DPLATFORM_WEB", "-DGRAPHICS_API_OPENGL_ES2",
            "-I" RAYLIB_DIR "src", nob_temp_sprintf("-o%s", obj));
        if (!nob_cmd_run_sync_and_reset(cmd)) return false;
    }
    if (!nob_delete_file(lib) && nob_file_exists(lib)) return false;
    nob_cmd_append(cmd, emar, "rcs", lib);
    for (size_t i = 0; i < COUNT(mods); i++)
        nob_cmd_append(cmd, nob_temp_sprintf(WEB_FOLDER "%s.o", mods[i]));
    return nob_cmd_run_sync_and_reset(cmd);
}

// emcc + the .a link the game itself straight to build-web\munkiki.html
// (wiki step 5.1; no ASYNCIFY - main.c hands the loop to the browser, see frame()).
static bool build_web(Nob_Cmd *cmd)
{
    if (!build_web_raylib(cmd)) return false;
    const char *emcc = find_emcc("emcc");   // already validated by build_web_raylib
    nob_cmd_append(cmd, emcc,
        nob_temp_sprintf("-o%s", WEB_FOLDER "munkiki.html"),
        "-Os",
        "src" SEP "engine.c", "src" SEP "game.c", "src" SEP "render.c", "src" SEP "m3d.c", "src" SEP "sound.c", "src" SEP "main.c",
        "-DPLATFORM_WEB", "-DGRAPHICS_API_OPENGL_ES2", "-Isrc", "-I" RAYLIB_DIR "src",
        WEB_FOLDER "libraylib.a",
        "-sUSE_GLFW=3", "-sINITIAL_MEMORY=67108864", "-sFORCE_FILESYSTEM=1", "-sMINIFY_HTML=1",
        "--shell-file", "web" SEP "shell.html");
    return nob_cmd_run_sync_and_reset(cmd);
}

static const char *core_srcs[] = {
    "src" SEP "engine.c", "src" SEP "game.c", "src" SEP "render.c", "src" SEP "m3d.c",
};

static const char *munkiki_srcs[]   = {"src" SEP "main.c", "src" SEP "sound.c"};
static const char *logic_srcs[]     = {"tests" SEP "logic_test.c"};
static const char *shot_srcs[]      = {"tests" SEP "scene_shot.c"};
static const char *fuzz_srcs[]      = {"tests" SEP "fuzz_loader.c"};

// --- android target ---------------------------------------------------------
// Uses the installed Android SDK/NDK (Android Studio's default layout) and the
// JDK on PATH (jar/keytool/apksigner are Java tools). Packages a NativeActivity
// APK (android/AndroidManifest.xml, no Java code): raylib + src/ become
// lib/<abi>/libmain.so, entry android_main() in main.c. Windows host only.
#define ANDROID_FOLDER "build-android\\"

#ifdef _WIN32
#include <windows.h>

// Newest version-named subdirectory (lexicographic max: majors are 2+ digits).
static const char *latest_subdir(const char *dir)
{
    static char out[4][4096];
    static int slot = 0;
    char pat[4096];
    snprintf(pat, sizeof pat, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    char best[1024] = {0};
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == '.') continue;
        if (strcmp(fd.cFileName, best) > 0) strcpy(best, fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    if (!best[0]) return NULL;
    char *res = out[slot = (slot + 1) % 4];
    snprintf(res, sizeof out[0], "%s\\%s", dir, best);
    return res;
}

static bool build_android(Nob_Cmd *cmd)
{
    char sdkbuf[4096] = {0};
    const char *sdk = getenv("ANDROID_HOME");
    if ((!sdk || !*sdk) && (sdk = getenv("ANDROID_SDK_ROOT")) == NULL) {
        const char *la = getenv("LOCALAPPDATA");
        if (!la) { nob_log(ERROR, "no ANDROID_HOME/ANDROID_SDK_ROOT/LOCALAPPDATA"); return false; }
        snprintf(sdkbuf, sizeof sdkbuf, "%s\\Android\\Sdk", la);
        sdk = sdkbuf;
    }
    const char *ndk  = latest_subdir(nob_temp_sprintf("%s\\ndk", sdk));
    const char *bt   = latest_subdir(nob_temp_sprintf("%s\\build-tools", sdk));
    const char *plat = latest_subdir(nob_temp_sprintf("%s\\platforms", sdk));
    if (!ndk || !bt || !plat) {
        nob_log(ERROR, "Android SDK incomplete under %s (need ndk\\*, build-tools\\*, platforms\\*);", sdk);
        nob_log(ERROR, "  install with: sdkmanager \"ndk;r27\" \"platforms;android-36\" \"build-tools;36.0.0\"");
        return false;
    }
    const char *tc  = nob_temp_sprintf("%s\\toolchains\\llvm\\prebuilt\\windows-x86_64\\bin", ndk);
    const char *mag = nob_temp_sprintf("%s\\sources\\android\\native_app_glue", ndk);
    const char *magc = nob_temp_sprintf("%s\\android_native_app_glue.c", mag);
    if (!nob_file_exists(magc)) {
        nob_log(ERROR, "native_app_glue not found at %s (NDK too new/old for this recipe)", magc);
        return false;
    }
    const char *jar_api = nob_temp_sprintf("%s\\android.jar", plat);
    if (!nob_mkdir_if_not_exists(ANDROID_FOLDER)) return false;

    static const char *const abis[] = {"arm64-v8a", "x86_64"};
    static const char *const triples[] = {"aarch64-linux-android", "x86_64-linux-android"};
    const char *raymods[] = {
        RAYLIB_DIR "src\\rcore.c", RAYLIB_DIR "src\\rshapes.c",
        RAYLIB_DIR "src\\rtextures.c", RAYLIB_DIR "src\\rtext.c", RAYLIB_DIR "src\\raudio.c",
    };

    for (size_t a = 0; a < COUNT(abis); a++) {
        const char *abi = abis[a];
        const char *target = nob_temp_sprintf("--target=%s24", triples[a]);  // min API 24
        const char *objdir = nob_temp_sprintf(ANDROID_FOLDER "obj_%s\\", abi);
        if (!nob_mkdir_if_not_exists(objdir)) return false;
        if (!nob_mkdir_if_not_exists(ANDROID_FOLDER "lib")) return false;
        if (!nob_mkdir_if_not_exists(nob_temp_sprintf(ANDROID_FOLDER "lib\\%s", abi))) return false;

        const char *objs[16];
        size_t n = 0;
        struct { const char *src; const char *stem; } units[] = {
            {magc, "android_native_app_glue"},
            {raymods[0], "rcore"}, {raymods[1], "rshapes"}, {raymods[2], "rtextures"},
            {raymods[3], "rtext"},  {raymods[4], "raudio"},
            {core_srcs[0], "engine"}, {core_srcs[1], "game"},
            {core_srcs[2], "render"}, {core_srcs[3], "m3d"},
            {munkiki_srcs[0], "main"}, {munkiki_srcs[1], "sound"},
        };
        for (size_t i = 0; i < COUNT(units); i++) {
            char *obj = nob_temp_sprintf("%s%s.o", objdir, units[i].stem);
            nob_cmd_append(cmd,
                nob_temp_sprintf("%s\\clang.exe", tc), target, "-c", "-fPIC", "-Os",
                "-DPLATFORM_ANDROID", "-DGRAPHICS_API_OPENGL_ES3",
                "-Isrc", "-I" RAYLIB_DIR "src", nob_temp_sprintf("-I%s", mag),
                nob_temp_sprintf("-o%s", obj), units[i].src);
            if (!nob_cmd_run_sync_and_reset(cmd)) return false;
            objs[n++] = obj;
        }
        nob_cmd_append(cmd,
            nob_temp_sprintf("%s\\clang.exe", tc), target, "-shared",
            nob_temp_sprintf("-o" ANDROID_FOLDER "lib\\%s\\libmain.so", abi));
        for (size_t i = 0; i < n; i++) nob_cmd_append(cmd, objs[i]);
        nob_cmd_append(cmd,
            "-Wl,--wrap=fopen",  // raylib android: fopen -> internal storage (rcore_android.c)
            "-lEGL", "-lGLESv1_CM", "-lGLESv2", "-landroid", "-llog", "-lOpenSLES", "-lm");
        if (!nob_cmd_run_sync_and_reset(cmd)) return false;
    }

    // ---- package the APK: aapt2 (compile res, link manifest) + jar (add libs)
    // + zipalign + apksigner (debug key, generated once).
    if (!nob_mkdir_if_not_exists(ANDROID_FOLDER)) return false;
    nob_cmd_append(cmd, nob_temp_sprintf("%s\\aapt2.exe", bt),
        "compile", "--dir", "android\\res",
        "-o", ANDROID_FOLDER "res.zip");
    if (!nob_cmd_run_sync_and_reset(cmd)) return false;
    nob_cmd_append(cmd, nob_temp_sprintf("%s\\aapt2.exe", bt),
        "link", "-o", ANDROID_FOLDER "base.apk",
        "--manifest", "android\\AndroidManifest.xml",
        "-I", jar_api, ANDROID_FOLDER "res.zip");
    if (!nob_cmd_run_sync_and_reset(cmd)) return false;
    // jar stores forward-slash entries and works on existing zips; cwd is the
    // repo root, so -C picks the lib/ tree relative to the APK root.
    nob_cmd_append(cmd, "jar", "uf", ANDROID_FOLDER "base.apk",
        "-C", ANDROID_FOLDER, "lib");
    if (!nob_cmd_run_sync_and_reset(cmd)) return false;
    if (nob_file_exists(ANDROID_FOLDER "aligned.apk") && !nob_delete_file(ANDROID_FOLDER "aligned.apk")) return false;
    nob_cmd_append(cmd, nob_temp_sprintf("%s\\zipalign.exe", bt),
        "-f", "4", ANDROID_FOLDER "base.apk", ANDROID_FOLDER "aligned.apk");
    if (!nob_cmd_run_sync_and_reset(cmd)) return false;
    if (!nob_file_exists(ANDROID_FOLDER "debug.keystore")) {
        nob_cmd_append(cmd, "keytool", "-genkeypair",
            "-keystore", ANDROID_FOLDER "debug.keystore",
            "-alias", "munkiki", "-keyalg", "RSA", "-keysize", "2048", "-validity", "10000",
            "-storepass", "android", "-keypass", "android",
            "-dname", "CN=Munkiki Debug,O=Munkiki,C=US");
        if (!nob_cmd_run_sync_and_reset(cmd)) return false;
    }
    if (nob_file_exists(ANDROID_FOLDER "munkiki.apk") && !nob_delete_file(ANDROID_FOLDER "munkiki.apk")) return false;
    nob_cmd_append(cmd, nob_temp_sprintf("%s\\apksigner.bat", bt), "sign",
        "--ks", ANDROID_FOLDER "debug.keystore",
        "--ks-pass", "pass:android", "--key-pass", "pass:android",
        "--out", ANDROID_FOLDER "munkiki.apk", ANDROID_FOLDER "aligned.apk");
    if (!nob_cmd_run_sync_and_reset(cmd)) return false;
    nob_log(INFO, "APK: " ANDROID_FOLDER "munkiki.apk (install: adb install -r " ANDROID_FOLDER "munkiki.apk)");
    return true;
}
#endif // _WIN32

typedef struct {
    const char *name;
    const char *const *srcs;
    size_t count;
    bool raylib;
} Target;

// Objects mirror the source basename under objdir.
static char *obj_path(const char *objdir, const char *src)
{
    const char *base = src;
    for (const char *p = src; *p; p++)
        if (*p == '\\' || *p == '/') base = p + 1;
    char *dot = strrchr(base, '.');
    size_t stem = dot ? (size_t)(dot - base) : strlen(base);
    return nob_temp_sprintf("%s%.*s" OBJEXT, objdir, (int)stem, base);
}

#ifndef _WIN32
#define POSIX_RAYLIB_LIB RAYLIB_DIR "build-linux" SEP "raylib" SEP "libraylib.a"
// On POSIX raylib is compiled from vendor/raylib on demand as a static lib;
// the cache dir is build-linux/ so the MSVC one stays untouched.
static bool ensure_raylib_posix(Nob_Cmd *cmd)
{
    if (nob_file_exists(POSIX_RAYLIB_LIB)) return true;
    const char *b = RAYLIB_DIR "build-linux";
    nob_log(INFO, "raylib static lib missing - building it with cmake in %s", b);
    nob_cmd_append(cmd, "cmake", "-S", RAYLIB_DIR, "-B", b,
        "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_SHARED_LIBS=OFF");
    if (!nob_cmd_run_sync_and_reset(cmd)) return false;
    long nj = sysconf(_SC_NPROCESSORS_ONLN);
    nob_cmd_append(cmd, "cmake", "--build", b, "--target", "raylib",
        "-j", nob_temp_sprintf("%ld", nj > 0 ? nj : 1));
    if (!nob_cmd_run_sync_and_reset(cmd)) return false;
    if (!nob_file_exists(POSIX_RAYLIB_LIB)) {
        nob_log(ERROR, "cmake finished but %s is still missing", POSIX_RAYLIB_LIB);
        return false;
    }
    return true;
}
#endif // !_WIN32

static bool build_target(Nob_Cmd *cmd, Target t)
{
    char *objdir = nob_temp_sprintf(BUILD_FOLDER "obj_%s" SEP, t.name);
    if (!nob_mkdir_if_not_exists(objdir)) return false;

    const char *all_srcs[8] = {0};
    size_t n = 0;
    for (size_t i = 0; i < t.count; i++) all_srcs[n++] = t.srcs[i];
    for (size_t i = 0; i < COUNT(core_srcs); i++) all_srcs[n++] = core_srcs[i];

#ifdef _WIN32
    // Compile step. NOTE: keep /TC on this step only - passing .lib inputs to
    // cl with /TC makes it try to compile them as C sources (C2018).
    nob_cmd_append(cmd,
        "cl", "/nologo", "/W4", "/O2", "/TC", "/c",
        "/DNDEBUG", "/D_CRT_SECURE_NO_WARNINGS", "/Isrc");
    if (t.raylib) nob_cmd_append(cmd, "/I" RAYLIB_DIR "src");
    nob_cmd_append(cmd, nob_temp_sprintf("/Fo:%s", objdir));
    for (size_t i = 0; i < n; i++) nob_cmd_append(cmd, all_srcs[i]);
    if (!nob_cmd_run_sync_and_reset(cmd)) return false;

    // Link step.
    // Link via cl (like tsoding/randomart): the compiler driver supplies the
    // matching CRT defaults, unlike bare `link`. No /TC on this step or cl
    // tries to compile the .lib inputs as C.
    nob_cmd_append(cmd, "cl", "/nologo",
        nob_temp_sprintf("/Fe:" BUILD_FOLDER "%s" EXE, t.name));
    for (size_t i = 0; i < n; i++)
        nob_cmd_append(cmd, obj_path(objdir, all_srcs[i]));

    if (t.raylib) {
        const char *raylib_lib = RAYLIB_DIR "build" SEP "raylib" SEP "Release" SEP "raylib.lib";
        if (!nob_file_exists(raylib_lib)) {
            nob_log(ERROR, "raylib static lib not found at %s; build it first:", raylib_lib);
            nob_log(ERROR, "  cmake -S vendor\\raylib -B vendor\\raylib\\build && cmake --build vendor\\raylib\\build --config Release --target raylib");
            return false;
        }
        nob_cmd_append(cmd, raylib_lib,
            "winmm.lib", "gdi32.lib", "opengl32.lib", "user32.lib", "shell32.lib");
        // raylib.lib carries /DEFAULTLIB:MSVCRT (legacy CRT) while cl objects
        // reference the UCRT; mixing both yields unresolved __imp_ imports.
        // Link against the UCRT import lib and drop the legacy CRT (options
        // after /link go to link.exe; cl itself ignores them - D9002).
        nob_cmd_append(cmd, "ucrt.lib", "/link", "/NODEFAULTLIB:MSVCRT",
            "/NODEFAULTLIB:libucrt.lib");
    }
#else
    // Native POSIX toolchain: $CC (default "cc") drives compile and link,
    // mirroring how cl is used as the driver on Windows.
    const char *cc = getenv("CC");
    if (!cc || !*cc) cc = "cc";
    if (t.raylib && !ensure_raylib_posix(cmd)) return false;
    for (size_t i = 0; i < n; i++) {
        nob_cmd_append(cmd, cc, "-std=c11", "-O2", "-DNDEBUG", "-Isrc");
        if (t.raylib) nob_cmd_append(cmd, "-I" RAYLIB_DIR "src");
        nob_cmd_append(cmd, "-c", all_srcs[i], "-o", obj_path(objdir, all_srcs[i]));
        if (!nob_cmd_run_sync_and_reset(cmd)) return false;
    }
    nob_cmd_append(cmd, cc, "-o", nob_temp_sprintf(BUILD_FOLDER "%s" EXE, t.name));
    for (size_t i = 0; i < n; i++)
        nob_cmd_append(cmd, obj_path(objdir, all_srcs[i]));
    if (t.raylib)
        nob_cmd_append(cmd, POSIX_RAYLIB_LIB, "-lGL", "-lpthread", "-ldl", "-lrt",
            "-lX11", "-lXrandr", "-lXi", "-lXcursor", "-lXinerama");
    nob_cmd_append(cmd, "-lm");
#endif
    if (!nob_cmd_run_sync_and_reset(cmd)) return false;
    return true;
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    setup_msvc_env();
#endif
    NOB_GO_REBUILD_URSELF(argc, argv);

    const char *want = "all";
    if (argc > 1) want = argv[1];

    Nob_Cmd cmd = {0};

    if (strcmp(want, "web") == 0) {
        if (!build_web(&cmd)) return 1;
        nob_log(INFO, "web build in " WEB_FOLDER "; serve it with: python -m http.server 8080");
        return 0;
    }

#ifdef _WIN32
    if (strcmp(want, "android") == 0) {
        if (!build_android(&cmd)) return 1;
        return 0;
    }
#endif
    const Target targets[] = {
        {"munkiki",    munkiki_srcs, COUNT(munkiki_srcs), true},
        {"logic_test", logic_srcs,   COUNT(logic_srcs),   false},
        {"scene_shot", shot_srcs,    COUNT(shot_srcs),    false},
        {"fuzz_loader", fuzz_srcs,   COUNT(fuzz_srcs),    false},
    };

    bool run = false;
    if (strcmp(want, "run") == 0) { want = "munkiki"; run = true; }

    size_t built = 0;
    for (size_t i = 0; i < COUNT(targets); i++) {
        if (!(strcmp(want, "all") == 0 || strcmp(want, targets[i].name) == 0)) continue;
        if (!build_target(&cmd, targets[i])) return 1;
        built += 1;
    }
    if (built == 0) {
        nob_log(ERROR, "unknown target \"%s\" (want munkiki|logic_test|scene_shot|fuzz_loader|web|android|all|run)", want);
        return 1;
    }

    if (run) {
        // Live-test convention: the game runs from the repo root
        // (OPT.rms/KKM.rms saves live there).
        nob_cmd_append(&cmd, BUILD_FOLDER "munkiki" EXE);
        for (int i = 2; i < argc; i++) nob_cmd_append(&cmd, argv[i]);
        if (!nob_cmd_run_sync_and_reset(&cmd)) return 1;
    }
    return 0;
}
