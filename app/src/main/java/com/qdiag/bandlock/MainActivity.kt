package com.qdiag.bandlock

import android.Manifest
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.viewModels
import com.qdiag.bandlock.ui.MainScreen
import com.qdiag.bandlock.ui.MainViewModel
import com.qdiag.bandlock.ui.QDiagTheme

class MainActivity : ComponentActivity() {
    private val vm: MainViewModel by viewModels()

    private val permsLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { vm.refreshCells() }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        permsLauncher.launch(arrayOf(
            Manifest.permission.READ_PHONE_STATE,
            Manifest.permission.ACCESS_FINE_LOCATION,
            Manifest.permission.ACCESS_COARSE_LOCATION,
        ))
        setContent { QDiagTheme { MainScreen(vm) } }
    }

    override fun onResume() {
        super.onResume()
        vm.refreshCells()
    }
}
