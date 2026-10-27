// fuzz_loader.c - fuzz harness for the KKM.rms save path.
// The shipped product entry for a hostile save file is mk_resume_run():
// it reads ./KKM.rms, parses the 9-byte header raw into the run state,
// hands the rest to mk_decode_level(), then places the monkey at the
// decoded spawn. This harness feeds arbitrary bytes through BOTH entries:
//   A) raw level-stream:  bytes -> mk_decode_level() directly (decoder core)
//   B) full save file:    bytes -> ./KKM.rms -> mk_resume_run() (header,
//      fallback paths and spawn placement included)
// After each entry several physics ticks run (falling blocks land, crush
// recovery and debris paths fire), then the software rasterizer paints the
// crafted scene (r_frame) - all under ASan/UBSan.
//
// Builds two ways:
//   clang ... -DLIBFUZZER -fsanitize=fuzzer,address,undefined  (libFuzzer)
//   cc  ... (default main: one file per run, for seed/regression repros)
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef _WIN32
#include <direct.h>
#endif
#include "engine.h"
#include "render.h"

extern int mk_decode_level(const uint8_t *p, int len);

// sound hook normally provided by the platform main/tests
void platform_sound(int id) { (void)id; }

static uint8_t fb[96 * 65];

// Physics + raster over whatever world the entry just decoded. mk_tick100
// drives the camera chain (r_set_view_chain); 8 ticks is enough for a cell
// of falling debris to land and trigger crush/debris handling.
static void exercise_world(void)
{
    for (int t = 0; t < 8; t++) mk_tick100();
    r_frame(fb);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static int inited;
    if (!inited) { mk_load_tables(); r_init(); inited = 1; }

    // A) raw level-stream entry: decoder core + physics + raster
    mk_load_level(mk_level_ids[0]);        // valid baseline world state
    mk_decode_level(data, (int)size);      // arbitrary bytes as level stream
    exercise_world();

    // B) product save-file entry: crafted KKM.rms -> mk_resume_run()
    FILE *f = fopen("KKM.rms", "wb");
    if (f) {
        fwrite(data, 1, size, f);
        fclose(f);
        mk_resume_run();                   // 9-byte header + stream + spawn
        remove("KKM.rms");                 // also covers the delete path
        exercise_world();
    }
    return 0;
}

#ifndef LIBFUZZER
static int run_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "open %s failed\n", path); return 2; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    if (n < 0) { fclose(f); fprintf(stderr, "unseekable %s\n", path); return 2; }
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n ? (size_t)n : 1);
    if (!buf) { fclose(f); fprintf(stderr, "oom %s\n", path); return 2; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); return 2; }
    fclose(f);
    printf("fuzz %s: %ld bytes\n", path, n);
    LLVMFuzzerTestOneInput(buf, (size_t)n);
    free(buf);
    return 0;
}

// --gen <dir>: emit one real saved run (9-byte header + level stream, i.e.
// the exact product KKM.rms shape) per level id as corpus seeds.
static int gen_corpus(const char *dir)
{
#ifdef _WIN32
    _mkdir(dir);
#else
    mkdir(dir, 0755);
#endif
    mk_load_tables();
    for (int i = 0; i < MK_LEVELS; i++) {
        mk_load_level(mk_level_ids[i]);
        mk_save_run();                       // writes ./KKM.rms
        char to[256];
        snprintf(to, sizeof(to), "%s/lvl%02d.bin", dir, mk_level_ids[i]);
        rename("KKM.rms", to);
    }
    printf("corpus: %d seeds in %s\n", MK_LEVELS, dir);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "--gen") == 0) return gen_corpus(argv[2]);
    if (argc < 2) { fprintf(stderr, "usage: %s FILE | --gen DIR\n", argv[0]); return 1; }
    for (int i = 1; i < argc; i++) { int rc = run_file(argv[i]); if (rc) return rc; }
    return 0;
}
#endif
