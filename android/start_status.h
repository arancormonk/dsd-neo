// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief What handing a start to the decoder service came to (#786 B2): DecoderService.startDecoder()'s status, and the
 * host's own reasons it never got that far.
 *
 * Free of JNI and Qt, so the host test can return each one from its platformStartDecoder() and check what the host
 * shows. The values DecoderService answers must equal DecoderService.kt's START_* constants (the engine_stop JVM
 * suite's start_status_contract checks them by name).
 */

#ifndef DSD_NEO_ANDROID_START_STATUS_H_
#define DSD_NEO_ANDROID_START_STATUS_H_

namespace dsd_android {

/** @brief The answer of a JNI call that failed (Qt clears the exception and returns 0): never a handover. */
constexpr int kStartCallFailed = 0;
/** @brief DecoderService.START_HANDED_OVER: the START went to the platform; the service decides it on arrival. */
constexpr int kStartHandedOver = 1;
/** @brief DecoderService.START_NOT_ALLOWED: Android refused a foreground service start from the background. */
constexpr int kStartNotAllowed = 2;
/** @brief DecoderService.START_ILLEGAL_STATE: Android refused the start in the app's current state. */
constexpr int kStartIllegalState = 3;
/** @brief DecoderService.START_SECURITY: Android refused the start for want of a permission. */
constexpr int kStartSecurity = 4;
/** @brief DecoderService.START_NO_SERVICE: the platform found no service to start. */
constexpr int kStartNoService = 5;
/** @brief DecoderService.START_UNEXPECTED: the platform failed the start with another exception. */
constexpr int kStartUnexpected = 6;
/** @brief The host's own: no Android context to start the service from. */
constexpr int kStartNoContext = -1;
/** @brief The host's own: the arguments could not be handed to Java. */
constexpr int kStartBadArguments = -2;

} // namespace dsd_android

#endif // DSD_NEO_ANDROID_START_STATUS_H_
