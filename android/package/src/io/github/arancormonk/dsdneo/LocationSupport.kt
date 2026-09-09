// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

import android.Manifest
import android.app.Activity
import android.content.Context
import android.content.pm.PackageManager
import android.location.Geocoder
import android.location.Location
import android.location.LocationListener
import android.location.LocationManager
import android.os.Build
import android.os.Bundle
import android.os.CancellationSignal
import android.os.Handler
import android.os.Looper
import org.json.JSONObject
import java.util.concurrent.Executors

/** One cancellable foreground request. All mutable request state belongs to the main looper. */
object LocationSupport {
    private const val PERMISSION = 4303
    private val main = Handler(Looper.getMainLooper())
    private val worker = Executors.newSingleThreadExecutor()
    private var active: Request? = null
    private var permissionActivity: Activity? = null
    private var result: String? = null

    private class Request(val activity: Activity, val id: Long) {
        val manager = activity.getSystemService(Context.LOCATION_SERVICE) as LocationManager
        var signal: CancellationSignal? = null
        var listener: LocationListener? = null
        var fix: Location? = null
        var timeout: Runnable? = null
    }

    @JvmStatic fun requestCurrentLocation(activity: Activity, id: Long) {
        main.post {
            active?.let { release(it) }
            synchronized(this) { result = null }
            val request = Request(activity, id)
            active = request
            request.timeout = Runnable {
                finish(request, false, "", "", if (request.fix == null) "Location request timed out; use Browse." else "Geocoding timed out; use Browse.")
            }.also { main.postDelayed(it, 20_000) }
            if (activity.checkSelfPermission(Manifest.permission.ACCESS_COARSE_LOCATION) == PackageManager.PERMISSION_GRANTED) {
                locate(request)
            } else if (permissionActivity == null) {
                permissionActivity = activity
                activity.requestPermissions(arrayOf(Manifest.permission.ACCESS_COARSE_LOCATION), PERMISSION)
            }
        }
    }

    @JvmStatic fun onRequestPermissionsResult(activity: Activity, code: Int, grants: IntArray): Boolean {
        if (code != PERMISSION) return false
        if (permissionActivity !== activity) return true
        permissionActivity = null
        val request = active ?: return true
        if (request.activity !== activity) return true
        if (grants.isNotEmpty() && grants[0] == PackageManager.PERMISSION_GRANTED) locate(request)
        else finish(request, false, "", "", "Location permission denied; use Browse.")
        return true
    }

    @Suppress("DEPRECATION", "OVERRIDE_DEPRECATION")
    private fun locate(request: Request) {
        if (active !== request) return
        try {
            // Coarse permission works with the network provider; no fine permission is requested.
            if (!request.manager.isProviderEnabled(LocationManager.NETWORK_PROVIDER)) {
                finish(request, false, "", "", "Location provider unavailable; use Browse.")
                return
            }
            if (Build.VERSION.SDK_INT >= 30) {
                request.signal = CancellationSignal()
                request.manager.getCurrentLocation(LocationManager.NETWORK_PROVIDER, request.signal,
                    request.activity.mainExecutor) { location -> gotFix(request, location) }
            } else {
                request.listener = object : LocationListener {
                    override fun onLocationChanged(location: Location) { gotFix(request, location) }
                    override fun onProviderDisabled(provider: String) {
                        finish(request, false, "", "", "Location provider unavailable; use Browse.")
                    }
                    override fun onProviderEnabled(provider: String) {}
                    override fun onStatusChanged(provider: String?, status: Int, extras: Bundle?) {}
                }
                request.manager.requestSingleUpdate(LocationManager.NETWORK_PROVIDER, request.listener!!, Looper.getMainLooper())
            }
        } catch (_: Exception) {
            finish(request, false, "", "", "Location unavailable; use Browse.")
        }
    }

    @Suppress("DEPRECATION", "OVERRIDE_DEPRECATION")
    private fun gotFix(request: Request, location: Location?) {
        if (active !== request || request.fix != null) return
        removeUpdates(request)
        if (location == null) {
            finish(request, false, "", "", "No location fix available; use Browse.")
            return
        }
        request.fix = location
        // Capture only application context and coordinates; an abandoned geocoder must not retain an Activity.
        val context = request.activity.applicationContext
        val latitude = location.latitude
        val longitude = location.longitude
        val id = request.id
        worker.execute {
            var postal = ""
            var country = ""
            var ok = false
            try {
                if (Geocoder.isPresent()) {
                    val address = Geocoder(context).getFromLocation(latitude, longitude, 1)?.firstOrNull()
                    if (address != null) {
                        postal = address.postalCode ?: ""
                        country = address.countryCode ?: ""
                        ok = true
                    }
                }
            } catch (_: Exception) { /* Never expose platform exception text. */ }
            main.post {
                active?.takeIf { it.id == id }?.let {
                    finish(it, ok, postal, country, if (ok) "" else "Geocoding unavailable; use Browse.")
                }
            }
        }
    }

    private fun removeUpdates(request: Request) {
        request.signal?.cancel()
        request.signal = null
        request.listener?.let { listener ->
            try { request.manager.removeUpdates(listener) } catch (_: Exception) { }
        }
        request.listener = null
    }

    private fun release(request: Request) {
        active = null
        request.timeout?.let { main.removeCallbacks(it) }
        removeUpdates(request)
    }

    private fun finish(request: Request, geocodeOk: Boolean, postal: String, country: String, error: String) {
        if (active !== request) return
        val fix = request.fix
        val record = JSONObject().put("id", request.id).put("fixOk", fix != null)
            .put("lat", fix?.latitude ?: 0.0).put("lon", fix?.longitude ?: 0.0)
            .put("accuracyM", fix?.accuracy?.toDouble() ?: 0.0).put("fixAtMs", fix?.time ?: 0L)
            .put("geocodeOk", geocodeOk).put("postalCode", postal).put("countryCode", country).put("error", error)
        release(request)
        synchronized(this) { result = record.toString() }
    }

    @JvmStatic fun cancelLocationRequest(id: Long) {
        main.post {
            active?.takeIf { it.id == id }?.let { release(it) }
            synchronized(this) {
                if (result?.let { JSONObject(it).optLong("id") == id } == true) result = null
            }
        }
    }

    @JvmStatic fun onDestroy(activity: Activity) {
        active?.takeIf { it.activity === activity }?.let {
            finish(it, false, "", "", "Location request cancelled; use Browse.")
        }
        if (permissionActivity === activity) permissionActivity = null
    }

    @JvmStatic @Synchronized fun pollResult(): String {
        val record = result ?: ""
        result = null
        return record
    }
}
