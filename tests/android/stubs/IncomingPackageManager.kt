// SPDX-License-Identifier: GPL-3.0-or-later
package android.content.pm

class ProviderInfo(@JvmField val packageName: String)

// Resolves the authorities a test lists to the package that declares each, and keeps the API level of the overload the
// last lookup used: 33 for the ComponentInfoFlags one.
class PackageManager {
    class ComponentInfoFlags private constructor(val value: Long) {
        companion object {
            @JvmStatic
            fun of(value: Long) = ComponentInfoFlags(value)
        }
    }

    companion object {
        const val PERMISSION_GRANTED = 0
        const val PERMISSION_DENIED = -1
    }

    val providers = mutableMapOf<String, String>()
    val lookups = mutableListOf<String>()
    var lastOverloadSdk = 0

    fun resolveContentProvider(name: String, flags: Int): ProviderInfo? = answer(name, 32)

    fun resolveContentProvider(name: String, flags: ComponentInfoFlags): ProviderInfo? = answer(name, 33)

    private fun answer(name: String, sdk: Int): ProviderInfo? {
        lookups += name
        lastOverloadSdk = sdk
        return providers[name]?.let(::ProviderInfo)
    }
}
