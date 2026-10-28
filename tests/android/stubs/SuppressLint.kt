// SPDX-License-Identifier: GPL-3.0-or-later
package android.annotation

// Android lint's suppression, which only lint reads.
@Target(AnnotationTarget.CLASS, AnnotationTarget.FUNCTION, AnnotationTarget.PROPERTY, AnnotationTarget.FIELD)
@Retention(AnnotationRetention.BINARY)
annotation class SuppressLint(vararg val value: String)
