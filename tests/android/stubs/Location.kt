// SPDX-License-Identifier: GPL-3.0-or-later
package android.location
import android.content.Context
import android.os.Bundle
import android.os.CancellationSignal
import android.os.Looper
import android.os.SystemClock
import java.util.concurrent.Executor
import java.util.function.Consumer
class Location {
    var latitude = 41.0
    var longitude = -91.0
    var accuracy = 25.0f
    var time = 123456L
    var elapsedRealtimeNanos = SystemClock.elapsedRealtimeNanos()
    var accuracyAvailable = true
    fun hasAccuracy() = accuracyAvailable
}
interface LocationListener {
    fun onLocationChanged(location: Location)
    fun onProviderDisabled(provider: String)
    fun onProviderEnabled(provider: String)
    fun onStatusChanged(provider: String?, status: Int, extras: Bundle?)
}
class LocationManager {
    companion object {
        const val NETWORK_PROVIDER = "network"
        const val FUSED_PROVIDER = "fused"
    }
    var enabled = true
    val disabledProviders = mutableSetOf<String>()
    val failingProviders = mutableSetOf<String>()
    val lastKnown = mutableMapOf<String, Location>()
    val cacheReads = mutableListOf<String>()
    val callbacks = mutableMapOf<String, Consumer<Location?>>()
    val signals = mutableMapOf<String, CancellationSignal>()
    val listeners = mutableMapOf<String, LocationListener>()
    val removedListeners = mutableListOf<LocationListener>()
    var callback: Consumer<Location?>? = null
    var signal: CancellationSignal? = null
    fun isProviderEnabled(provider: String) = enabled && provider !in disabledProviders
    fun getLastKnownLocation(provider: String): Location? { cacheReads.add(provider); return lastKnown[provider] }
    fun getCurrentLocation(provider: String, cancellation: CancellationSignal?, executor: Executor, receiver: Consumer<Location?>) {
        if (provider in failingProviders) throw IllegalArgumentException("unavailable provider")
        signal = cancellation
        callback = receiver
        if (cancellation != null) signals[provider] = cancellation
        callbacks[provider] = receiver
    }
    fun requestSingleUpdate(provider: String, listener: LocationListener, looper: Looper) {
        callback = Consumer { if (it != null) listener.onLocationChanged(it) }
        callbacks[provider] = callback!!
        listeners[provider] = listener
    }
    fun removeUpdates(listener: LocationListener) { removedListeners.add(listener) }
}
class Address(var postalCode: String? = "52240", var countryCode: String? = "US")
class Geocoder(context: Context) {
    companion object {
        @Volatile var lookup: (Double, Double) -> List<Address>? = { _, _ -> listOf(Address()) }
        fun isPresent() = true
    }
    fun getFromLocation(lat: Double, lon: Double, limit: Int) = lookup(lat, lon)
}
