/*
 * android_boot.h -- the two things a standalone headset build needs before the
 * engine can start, and which no other platform has to think about.
 *
 * Compiled only on Android (see the ANDROID arm in CMakeLists.txt). Both calls
 * are safe to make more than once.
 */
#ifndef MDKR_ANDROID_BOOT_H
#define MDKR_ANDROID_BOOT_H

#ifdef __ANDROID__

/*
 * Point stdout and stderr at logcat.
 *
 * An Android app's stdout and stderr go nowhere, so every printf and fprintf in
 * the port -- the boot banner, the VR log, the crash marker -- is thrown away,
 * and a run that failed looks exactly like a run that never started. Call this
 * first thing in main, before anything prints.
 */
void mdkr_android_log_stdio(void);

/*
 * The player's ROM, or NULL when there is not one on the device yet.
 *
 * Returns storage owned by this module; it stays valid for the life of the
 * process and must not be freed. The search order is deliberate: the app's own
 * external files directory first, because that is where the hub's Send to Quest
 * writes and reading it needs no permission at all, then the handful of shared
 * storage folders a hand-dropped dump actually lands in.
 */
const char *mdkr_android_find_rom(void);

#endif /* __ANDROID__ */

#endif /* MDKR_ANDROID_BOOT_H */
