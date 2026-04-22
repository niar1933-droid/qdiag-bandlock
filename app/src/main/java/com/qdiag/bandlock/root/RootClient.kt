package com.qdiag.bandlock.root

import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.os.IBinder
import com.topjohnwu.superuser.Shell
import com.topjohnwu.superuser.ipc.RootService
import kotlinx.coroutines.channels.awaitClose
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.callbackFlow

/**
 * Root availability + connection to [DiagRootService].
 *
 * Usage from the UI/ViewModel:
 *
 *     RootClient.ensureRoot()
 *     RootClient.bind(context)
 *     val api = RootClient.api.value ?: return
 *     api.setBandPreference(...)
 */
object RootClient {
    init {
        Shell.enableVerboseLogging = true
        Shell.setDefaultBuilder(
            Shell.Builder.create()
                .setFlags(Shell.FLAG_REDIRECT_STDERR or Shell.FLAG_MOUNT_MASTER)
                .setTimeout(10)
        )
    }

    sealed class Status {
        data object Unknown : Status()
        data object NoRoot : Status()
        data object HasRoot : Status()
        data class Error(val message: String) : Status()
    }

    private val _status = MutableStateFlow<Status>(Status.Unknown)
    val status: StateFlow<Status> = _status.asStateFlow()

    private val _api = MutableStateFlow<IDiagRoot?>(null)
    val api: StateFlow<IDiagRoot?> = _api.asStateFlow()

    fun checkRoot() {
        Shell.getShell {
            _status.value = if (it.isRoot) Status.HasRoot else Status.NoRoot
        }
    }

    private var connection: ServiceConnection? = null

    fun bind(context: Context) {
        if (connection != null) return
        val intent = Intent(context, DiagRootService::class.java)
        val conn = object : ServiceConnection {
            override fun onServiceConnected(name: ComponentName?, service: IBinder?) {
                _api.value = IDiagRoot.Stub.asInterface(service)
            }
            override fun onServiceDisconnected(name: ComponentName?) { _api.value = null }
        }
        RootService.bind(intent, conn)
        connection = conn
    }

    fun unbind(context: Context) {
        connection?.let {
            RootService.stop(Intent(context, DiagRootService::class.java))
            connection = null
            _api.value = null
        }
    }
}
