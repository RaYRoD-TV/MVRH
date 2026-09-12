/*
 * android_boot.c -- log plumbing and ROM discovery for the standalone headset
 * build. See android_boot.h for the contract.
 *
 * Nothing here is VR: it is the part of "put on the headset and play" that has
 * to be true before a single frame is drawn. Both jobs exist because Android
 * takes away something every other platform in this port gives for free -- a
 * place for stdout to go, and a current directory with the ROM sitting in it.
 */
#ifdef __ANDROID__

#include "android_boot.h"

#include <android/log.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <SDL.h>
#include <SDL_system.h>

#define MDKR_ANDROID_TAG "mdkr64"
#define MDKR_ANDROID_PATH_MAX 1024

/* --- stdout and stderr into logcat -------------------------------------- */

/*
 * One pipe, one reader thread. Both streams are dup2'd onto the write end, so
 * the interleaving the player would have seen in a terminal is preserved, and
 * the reader splits on newlines because logcat is line-oriented: writing a
 * partial line per read would shred every message across several entries.
 */
static int s_logPipe[2] = { -1, -1 };

static void *mdkr_android_log_pump(void *unused) {
    char line[512];
    size_t used = 0u;
    (void) unused;
    for (;;) {
        char chunk[256];
        ssize_t got = read(s_logPipe[0], chunk, sizeof(chunk));
        ssize_t i;
        if (got <= 0) {
            if (got < 0 && errno == EINTR) continue;
            break;
        }
        for (i = 0; i < got; i++) {
            if (chunk[i] == '\n' || used + 1u >= sizeof(line)) {
                line[used] = '\0';
                if (used > 0u) {
                    __android_log_write(ANDROID_LOG_INFO, MDKR_ANDROID_TAG, line);
                }
                used = 0u;
                if (chunk[i] != '\n') line[used++] = chunk[i];
            } else {
                line[used++] = chunk[i];
            }
        }
    }
    return NULL;
}

void mdkr_android_log_stdio(void) {
    pthread_t thread;
    if (s_logPipe[0] != -1) return;   /* already running */

    /* Unbuffered, so a line that precedes a crash is already through the pipe
     * when the process dies. The engine sets line buffering for a terminal;
     * here the reader is what does the line splitting, so buffering upstream
     * only delays the one message that matters. */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    if (pipe(s_logPipe) != 0) {
        __android_log_write(ANDROID_LOG_ERROR, MDKR_ANDROID_TAG,
                            "could not open the log pipe; printf output is lost");
        s_logPipe[0] = s_logPipe[1] = -1;
        return;
    }
    dup2(s_logPipe[1], STDOUT_FILENO);
    dup2(s_logPipe[1], STDERR_FILENO);

    if (pthread_create(&thread, NULL, mdkr_android_log_pump, NULL) != 0) {
        __android_log_write(ANDROID_LOG_ERROR, MDKR_ANDROID_TAG,
                            "could not start the log pump; printf output is lost");
        return;
    }
    pthread_detach(thread);
    __android_log_write(ANDROID_LOG_INFO, MDKR_ANDROID_TAG, "stdout and stderr are on logcat");
}

/* --- finding the player's ROM ------------------------------------------- */

/*
 * The names a Diddy Kong Racing dump arrives under. The first is what the hub's
 * Send to Quest writes (games.json romFile), the second is what the desktop
 * build defaults to, and the rest are what a hand-dropped dump is usually
 * called. Anything not on this list is still found by the extension-and-size
 * sweep below, so this is a preference order rather than a gate.
 */
static const char *const kRomNames[] = {
    "dkr.us.v80.z64",
    "baserom.us.v80.z64",
    "dkr.z64",
    "dkr.v64",
    "dkr.n64",
};

/*
 * Where to look, in order. The app's own external files directory comes from
 * SDL at runtime and is checked first: it is where Send to Quest pushes, and
 * an app can read it with no permission granted at all. Everything after it is
 * shared storage, which needs the all-files toggle the activity asks for, and
 * exists only so a dump the player copied over MTP is still found.
 */
static const char *const kSharedDirs[] = {
    "/sdcard/GoldenBalloonVR",
    "/sdcard/DiddyKongRacing",
    "/sdcard/Download",
    "/sdcard/Download/GoldenBalloonVR",
    "/sdcard",
};

/* A retail Diddy Kong Racing image is exactly 12 MB in any of the three byte
 * orders. Checking the size before handing a path to the loader keeps the sweep
 * from picking up a savestate or a texture pack that happens to end in .z64. */
#define MDKR_ROM_BYTES (12 * 1024 * 1024)

static char s_romPath[MDKR_ANDROID_PATH_MAX];

static int mdkr_android_is_rom_file(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    if (!S_ISREG(st.st_mode)) return 0;
    return st.st_size == (off_t) MDKR_ROM_BYTES;
}

static int mdkr_android_has_rom_extension(const char *name) {
    const char *dot = strrchr(name, '.');
    if (dot == NULL) return 0;
    return strcasecmp(dot, ".z64") == 0 ||
           strcasecmp(dot, ".n64") == 0 ||
           strcasecmp(dot, ".v64") == 0;
}

/* Returns 1 and fills s_romPath when this directory holds something usable. */
static int mdkr_android_scan_dir(const char *dir) {
    size_t i;
    DIR *handle;
    struct dirent *entry;

    if (dir == NULL || dir[0] == '\0') return 0;

    /* The known names first, so a directory holding both the hub's push and
     * some other dump resolves the same way every launch. */
    for (i = 0u; i < sizeof(kRomNames) / sizeof(kRomNames[0]); i++) {
        (void) snprintf(s_romPath, sizeof(s_romPath), "%s/%s", dir, kRomNames[i]);
        if (mdkr_android_is_rom_file(s_romPath)) return 1;
    }

    /* Then anything else in the directory that looks like a 12 MB N64 image.
     * One level only: a recursive walk of /sdcard on a headset takes long
     * enough to read as a hang. */
    handle = opendir(dir);
    if (handle == NULL) return 0;
    while ((entry = readdir(handle)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        if (!mdkr_android_has_rom_extension(entry->d_name)) continue;
        (void) snprintf(s_romPath, sizeof(s_romPath), "%s/%s", dir, entry->d_name);
        if (mdkr_android_is_rom_file(s_romPath)) {
            closedir(handle);
            return 1;
        }
    }
    closedir(handle);
    s_romPath[0] = '\0';
    return 0;
}

const char *mdkr_android_find_rom(void) {
    size_t i;
    const char *own;

    if (s_romPath[0] != '\0') return s_romPath;

    own = SDL_AndroidGetExternalStoragePath();
    if (mdkr_android_scan_dir(own)) {
        printf("[mdkr64] rom: %s\n", s_romPath);
        return s_romPath;
    }
    if (mdkr_android_scan_dir(SDL_AndroidGetInternalStoragePath())) {
        printf("[mdkr64] rom: %s\n", s_romPath);
        return s_romPath;
    }
    for (i = 0u; i < sizeof(kSharedDirs) / sizeof(kSharedDirs[0]); i++) {
        if (mdkr_android_scan_dir(kSharedDirs[i])) {
            printf("[mdkr64] rom: %s\n", s_romPath);
            return s_romPath;
        }
    }

    /* Say where it looked. "Could not load ROM: (null)" tells a player nothing
     * they can act on, and this is the one failure a fresh install actually
     * hits. */
    fprintf(stderr, "[mdkr64] no Diddy Kong Racing rom on this headset.\n");
    fprintf(stderr, "[mdkr64] send it from the hub, or copy it to %s\n",
            own != NULL ? own : "the app's files folder");
    return NULL;
}

#endif /* __ANDROID__ */
