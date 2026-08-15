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

#ifdef __ANDROID__

#include <android/log.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <SDL3/SDL.h>
#include <gl4esinit.h>

#include "i_android.h"

#define LOG_TAG "ringracers"

static void *logcat_pump(void *arg)
{
	int fd = (int)(intptr_t)arg;
	char buf[1024];
	size_t used = 0;

	for (;;)
	{
		ssize_t n = read(fd, buf + used, sizeof(buf) - 1 - used);
		if (n <= 0)
			break;
		used += (size_t)n;
		buf[used] = '\0';

		char *start = buf;
		char *nl;
		while ((nl = strchr(start, '\n')) != NULL)
		{
			*nl = '\0';
			if (*start)
				__android_log_write(ANDROID_LOG_INFO, LOG_TAG, start);
			start = nl + 1;
		}

		used = strlen(start);
		memmove(buf, start, used + 1);

		// A line longer than the buffer would wedge the reader; flush it whole.
		if (used >= sizeof(buf) - 1)
		{
			__android_log_write(ANDROID_LOG_INFO, LOG_TAG, buf);
			used = 0;
		}
	}
	return NULL;
}

void I_AndroidStartLogcat(void)
{
	int fds[2];
	pthread_t t;

	if (pipe(fds) != 0)
		return;

	dup2(fds[1], STDOUT_FILENO);
	dup2(fds[1], STDERR_FILENO);
	setvbuf(stdout, NULL, _IOLBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);

	if (pthread_create(&t, NULL, logcat_pump, (void *)(intptr_t)fds[0]) == 0)
		pthread_detach(t);
}

static void (*s_fbsize_cb)(int *w, int *h);

static void fbsize_thunk(int *w, int *h)
{
	if (s_fbsize_cb)
		s_fbsize_cb(w, h);
	else
	{
		*w = 1280;
		*h = 720;
	}
}

void I_AndroidInitGL(void (*fbsize)(int *w, int *h))
{
	static int done = 0;

	if (done)
		return;
	done = 1;

	s_fbsize_cb = fbsize;
	set_getprocaddress((void *(*)(const char *))SDL_GL_GetProcAddress);
	set_getmainfbsize(fbsize_thunk);
	initialize_gl4es();
	__android_log_write(ANDROID_LOG_INFO, LOG_TAG, "gl4es initialized");
}

void *I_AndroidGLGetProc(const char *name)
{
	return gl4es_GetProcAddress(name);
}

#endif // __ANDROID__
