#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#endif

#include "stb_vorbis.c"

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_FLAC
#include "miniaudio.h"
