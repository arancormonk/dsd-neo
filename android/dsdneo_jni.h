// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief In-process view of the JNI lifecycle layer, for the Qt host.
 *
 * The service drives the engine over JNI; the Qt host lives in the same library and
 * reads the state back directly instead of paying for a JNI round trip.
 */

#ifndef DSD_NEO_ANDROID_DSDNEO_JNI_H_
#define DSD_NEO_ANDROID_DSDNEO_JNI_H_

namespace dsd_android {

/** @brief Whether the engine loop is currently turning. */
bool engine_is_running(void);

} // namespace dsd_android

#endif /* DSD_NEO_ANDROID_DSDNEO_JNI_H_ */
