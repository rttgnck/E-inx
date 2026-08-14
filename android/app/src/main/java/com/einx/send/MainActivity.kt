package com.einx.send

import android.Manifest
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.viewModels
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.foundation.layout.fillMaxSize
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.einx.send.ui.SendApp

class MainActivity : ComponentActivity() {

  private val viewModel: SendViewModel by viewModels()

  /**
   * SAF, not a storage permission. The picker hands back a Uri for exactly the one file
   * the user chose, which is all this app ever needs to read.
   */
  private val pickBook = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
    uri?.let {
      contentResolver.takePersistableUriPermission(it, Intent.FLAG_GRANT_READ_URI_PERMISSION)
      viewModel.onBookChosen(it)
    }
  }

  private val requestPermissions =
    registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { granted ->
      if (granted.values.all { it }) {
        viewModel.startScan()
      }
    }

  override fun onCreate(savedInstanceState: Bundle?) {
    super.onCreate(savedInstanceState)

    ensureBluetoothPermissions()

    setContent {
      MaterialTheme {
        Surface(modifier = Modifier.fillMaxSize()) {
          val state by viewModel.state.collectAsStateWithLifecycle()

          SendApp(
            state = state,
            onChooseBook = { pickBook.launch(PICKER_MIME_TYPES) },
            onSend = { reader ->
              ensureBluetoothPermissions()
              viewModel.send(reader)
            },
            onRescan = viewModel::startScan,
            onCancel = viewModel::cancel,
            onDone = viewModel::reset,
            onChooseAnotherReader = viewModel::chooseAnotherReader,
          )
        }
      }
    }

    handleShare(intent)
  }

  override fun onNewIntent(intent: Intent) {
    super.onNewIntent(intent)
    setIntent(intent)
    handleShare(intent)
  }

  override fun onStop() {
    super.onStop()
    // Scanning is expensive and pointless once the app is off screen.
    viewModel.stopScan()
  }

  /** Picks up a book handed over by the Share sheet. */
  private fun handleShare(intent: Intent?) {
    if (intent?.action != Intent.ACTION_SEND) return

    val uri: Uri? = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
      intent.getParcelableExtra(Intent.EXTRA_STREAM, Uri::class.java)
    } else {
      @Suppress("DEPRECATION")
      intent.getParcelableExtra(Intent.EXTRA_STREAM)
    }

    uri?.let { viewModel.onBookChosen(it) }
  }

  private fun ensureBluetoothPermissions() {
    val needed = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
      arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
    } else {
      arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
    }

    val missing = needed.filter {
      checkSelfPermission(it) != android.content.pm.PackageManager.PERMISSION_GRANTED
    }

    if (missing.isNotEmpty()) {
      requestPermissions.launch(missing.toTypedArray())
    }
  }

  private companion object {
    /**
     * Deliberately broad. Android has no single agreed MIME type for EPUB and file managers
     * disagree about it; the extension check in BleProtocol is what actually decides.
     */
    val PICKER_MIME_TYPES = arrayOf(
      "application/epub+zip",
      "text/plain",
      "application/octet-stream",
    )
  }
}
