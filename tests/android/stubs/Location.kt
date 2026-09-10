// SPDX-License-Identifier: GPL-3.0-or-later
package android.location
import android.content.Context
import android.os.Bundle
import android.os.CancellationSignal
import android.os.Looper
import java.util.concurrent.Executor
import java.util.function.Consumer
class Location {
    var latitude = 41.0
    var longitude = -91.0
    var accuracy = 25.0f
    var time = 123456L
}
interface LocationListener {
    fun onLocationChanged(location: Location)
    fun onProviderDisabled(provider: String)
    fun onProviderEnabled(provider: String)
    fun onStatusChanged(provider: String?, status: Int, extras: Bundle?)
}
class LocationManager {
    companion object { const val NETWORK_PROVIDER = "network" }
    var enabled = true
    var callback: Consumer<Location?>? = null
    var signal: CancellationSignal? = null
    fun isProviderEnabled(provider: String) = enabled
    fun getCurrentLocation(provider: String, cancellation: CancellationSignal?, executor: Executor, receiver: Consumer<Location?>) {
        signal = cancellation
        callback = receiver
    }
    fun requestSingleUpdate(provider: String, listener: LocationListener, looper: Looper) {
        callback = Consumer { if (it != null) listener.onLocationChanged(it) }
    }
    fun removeUpdates(listener: LocationListener) {}
}
class Address(var postalCode: String? = "52240", var countryCode: String? = "US")
class Geocoder(context: Context) {
    companion object {
        @Volatile var lookup: (Double, Double) -> List<Address>? = { _, _ -> listOf(Address()) }
        fun isPresent() = true
    }
    fun getFromLocation(lat: Double, lon: Double, limit: Int) = lookup(lat, lon)
}
