// SPDX-License-Identifier: GPL-3.0-or-later
package android.os.storage
import android.content.Context
import java.io.File
import java.util.concurrent.Executor

// The storage volumes a test lists, with the platform's names: getDirectory() is null unless the volume is mounted.
class StorageVolume(
    val directory: File?,
    val state: String,
    val isRemovable: Boolean,
    val isPrimary: Boolean,
    val uuid: String?,
    private val description: String?,
) {
    fun getDescription(context: Context): String? = description
}

// Lists the test's volumes and keeps each volume callback registered, for the test to call.
class StorageManager {
    val volumes = mutableListOf<StorageVolume>()
    val callbacks = mutableListOf<StorageVolumeCallback>()

    val storageVolumes: List<StorageVolume> get() = volumes.toList()

    fun registerStorageVolumeCallback(executor: Executor, callback: StorageVolumeCallback) {
        callbacks += callback
    }

    open class StorageVolumeCallback {
        open fun onStateChanged(volume: StorageVolume) {}
    }
}
