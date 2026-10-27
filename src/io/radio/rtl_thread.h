// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DSD_NEO_IO_RADIO_RTL_THREAD_H
#define DSD_NEO_IO_RADIO_RTL_THREAD_H
#include <dsd-neo/platform/threading.h>

#if DSD_PLATFORM_WIN_NATIVE
#define DSD_RTL_THREAD_CALL __stdcall
#else
#define DSD_RTL_THREAD_CALL
#endif
#endif
