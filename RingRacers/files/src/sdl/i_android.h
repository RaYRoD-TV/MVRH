// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// Copyright (C) 2026 by Kart Krew.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file
/// \brief Android platform glue: logcat output routing and the gl4es bootstrap.

#ifndef __I_ANDROID_H__
#define __I_ANDROID_H__

#ifdef __ANDROID__

#ifdef __cplusplus
extern "C" {
#endif

// Redirects stdout/stderr into a pipe pumped to logcat (tag "ringracers").
// Without this every printf on Android vanishes. Call once, first thing.
void I_AndroidStartLogcat(void);

// One-time gl4es bootstrap, called with a live GLES context current. The
// fbsize callback reports the drawable size gl4es uses for the default
// framebuffer (required because gl4es is built without its own EGL).
void I_AndroidInitGL(void (*fbsize)(int *w, int *h));

// Resolves GL entry points through gl4es so the whole renderer sees desktop
// GL 2.1 on top of the real GLES context.
void *I_AndroidGLGetProc(const char *name);

#ifdef __cplusplus
}
#endif

#endif // __ANDROID__

#endif // __I_ANDROID_H__
