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
// The platform's request form from API 31, with its constants and getters; the minimum interval left unset reads as the
// platform's "implicit" value, so a test sees whether it was set.
class LocationRequest private constructor(
    val intervalMillis: Long, val minUpdateIntervalMillis: Long, val minUpdateDistanceMeters: Float, val quality: Int,
    val durationMillis: Long,
) {
    companion object {
        const val QUALITY_HIGH_ACCURACY = 100
        const val QUALITY_BALANCED_POWER_ACCURACY = 102
        const val QUALITY_LOW_POWER = 104
    }
    class Builder(private val intervalMillis: Long) {
        private var minUpdateIntervalMillis = -1L
        private var minUpdateDistanceMeters = 0f
        private var quality = QUALITY_BALANCED_POWER_ACCURACY
        private var durationMillis = Long.MAX_VALUE
        fun setMinUpdateIntervalMillis(value: Long) = apply { minUpdateIntervalMillis = value }
        fun setMinUpdateDistanceMeters(value: Float) = apply { minUpdateDistanceMeters = value }
        fun setQuality(value: Int) = apply { quality = value }
        fun setDurationMillis(value: Long) = apply { durationMillis = value }
        fun build() = LocationRequest(intervalMillis, minUpdateIntervalMillis, minUpdateDistanceMeters, quality, durationMillis)
    }
}
/** One continuous registration: the API 31 form fills [request] and [executor], the legacy form the rest. */
class LocationUpdates(
    val listener: LocationListener,
    val request: LocationRequest? = null,
    val executor: Executor? = null,
    val minTimeMs: Long = -1,
    val minDistanceM: Float = -1f,
    val looper: Looper? = null,
)
class LocationManager {
    companion object {
        const val NETWORK_PROVIDER = "network"
        const val FUSED_PROVIDER = "fused"
        const val GPS_PROVIDER = "gps"
    }
    var enabled = true
    val allProviders = mutableListOf(NETWORK_PROVIDER, FUSED_PROVIDER, GPS_PROVIDER)
    val updates = mutableMapOf<String, LocationUpdates>()
    val disabledProviders = mutableSetOf<String>()
    val failingProviders = mutableSetOf<String>()
    val lastKnown = mutableMapOf<String, Location>()
    val cacheReads = mutableListOf<String>()
    val callbacks = mutableMapOf<String, Consumer<Location?>>()
    val signals = mutableMapOf<String, CancellationSignal>()
    // The API 31 one-shot form's request, per provider.
    val currentRequests = mutableMapOf<String, LocationRequest>()
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
    fun getCurrentLocation(
        provider: String, request: LocationRequest, cancellation: CancellationSignal?, executor: Executor,
        receiver: Consumer<Location?>,
    ) {
        getCurrentLocation(provider, cancellation, executor, receiver)
        currentRequests[provider] = request
    }
    fun requestSingleUpdate(provider: String, listener: LocationListener, looper: Looper) {
        callback = Consumer { if (it != null) listener.onLocationChanged(it) }
        callbacks[provider] = callback!!
        listeners[provider] = listener
    }
    fun requestLocationUpdates(provider: String, request: LocationRequest, executor: Executor, listener: LocationListener) {
        if (provider in failingProviders) throw IllegalArgumentException("unavailable provider")
        updates[provider] = LocationUpdates(listener, request = request, executor = executor)
    }
    fun requestLocationUpdates(provider: String, minTimeMs: Long, minDistanceM: Float, listener: LocationListener, looper: Looper) {
        if (provider in failingProviders) throw IllegalArgumentException("unavailable provider")
        updates[provider] = LocationUpdates(listener, minTimeMs = minTimeMs, minDistanceM = minDistanceM, looper = looper)
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
