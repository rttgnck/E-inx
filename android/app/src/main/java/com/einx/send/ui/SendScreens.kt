package com.einx.send.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import com.einx.send.DiscoveredReader
import com.einx.send.SendStage
import com.einx.send.SendUiState

/** Bytes as the user would say them, not as the machine stores them. */
fun formatBytes(bytes: Long): String = when {
  bytes >= 1024L * 1024L -> String.format("%.2f MB", bytes / (1024.0 * 1024.0))
  bytes >= 1024L -> String.format("%.0f KB", bytes / 1024.0)
  else -> "$bytes B"
}

@Composable
fun SendApp(
  state: SendUiState,
  onChooseBook: () -> Unit,
  onNameChanged: (String) -> Unit,
  onTitleChanged: (String) -> Unit,
  onAuthorChanged: (String) -> Unit,
  onSendToDefault: () -> Unit,
  onChooseReader: () -> Unit,
  onSelectReader: (DiscoveredReader) -> Unit,
  onRescan: () -> Unit,
  onCancel: () -> Unit,
  onDone: () -> Unit,
  onBackToPrepare: () -> Unit,
  onOpenSettings: () -> Unit,
  onCloseSettings: () -> Unit,
  onForgetReader: () -> Unit,
  onAlwaysAskChanged: (Boolean) -> Unit,
) {
  Column(
    modifier = Modifier
      .fillMaxSize()
      .padding(24.dp),
  ) {
    Row(
      modifier = Modifier.fillMaxWidth(),
      horizontalArrangement = Arrangement.SpaceBetween,
      verticalAlignment = Alignment.CenterVertically,
    ) {
      Text("E-inx Send", style = MaterialTheme.typography.headlineMedium)
      if (state.stage != SendStage.Settings) {
        TextButton(onClick = onOpenSettings) { Text("Settings") }
      }
    }
    Spacer(Modifier.height(16.dp))

    when (state.stage) {
      SendStage.PickBook -> PickBookScreen(onChooseBook)
      SendStage.Prepare -> PrepareScreen(
        state, onNameChanged, onTitleChanged, onAuthorChanged, onSendToDefault, onChooseReader, onDone,
      )
      SendStage.ChooseReader -> ChooseReaderScreen(state, onSelectReader, onRescan, onBackToPrepare)
      SendStage.Preparing -> PreparingScreen(state)
      SendStage.Sending -> SendingScreen(state, onCancel)
      SendStage.Sent -> SentScreen(state, onDone)
      SendStage.Failed -> FailedScreen(state, onDone, onBackToPrepare)
      SendStage.Settings -> SettingsScreen(state, onForgetReader, onAlwaysAskChanged, onCloseSettings)
    }
  }
}

@Composable
private fun PickBookScreen(onChooseBook: () -> Unit) {
  Column(verticalArrangement = Arrangement.spacedBy(16.dp)) {
    Text(
      "Pick a book to send to your reader, or share one to E-inx Send from any app.",
      style = MaterialTheme.typography.bodyLarge,
    )
    Button(onClick = onChooseBook, modifier = Modifier.fillMaxWidth()) { Text("Choose Book") }
    Text("EPUB, TXT, XTC and XTCH", style = MaterialTheme.typography.bodySmall)
    Spacer(Modifier.height(8.dp))
    Text(
      "On the reader: Device Connections → Bluetooth Transfer",
      style = MaterialTheme.typography.bodyMedium,
    )
  }
}

@Composable
private fun PrepareScreen(
  state: SendUiState,
  onNameChanged: (String) -> Unit,
  onTitleChanged: (String) -> Unit,
  onAuthorChanged: (String) -> Unit,
  onSendToDefault: () -> Unit,
  onChooseReader: () -> Unit,
  onDone: () -> Unit,
) {
  Column(
    modifier = Modifier
      .fillMaxSize()
      .verticalScroll(rememberScrollState()),
    verticalArrangement = Arrangement.spacedBy(16.dp),
  ) {
    state.book?.let { Text(formatBytes(it.size), style = MaterialTheme.typography.bodySmall) }

    OutlinedTextField(
      value = state.editedName,
      onValueChange = onNameChanged,
      label = { Text("File name") },
      singleLine = true,
      isError = state.nameError != null,
      supportingText = state.nameError?.let { { Text(it) } },
      modifier = Modifier.fillMaxWidth(),
    )

    if (state.book?.extension == "epub") {
      Text("Book details", style = MaterialTheme.typography.titleMedium)
      Text(
        "Saved on the reader alongside the book. The file itself is sent unchanged.",
        style = MaterialTheme.typography.bodySmall,
      )

      OutlinedTextField(
        value = state.editedTitle,
        onValueChange = onTitleChanged,
        label = { Text("Title") },
        singleLine = true,
        placeholder = { Text("Keep the book's own") },
        modifier = Modifier.fillMaxWidth(),
      )
      OutlinedTextField(
        value = state.editedAuthor,
        onValueChange = onAuthorChanged,
        label = { Text("Author") },
        singleLine = true,
        placeholder = { Text("Keep the book's own") },
        modifier = Modifier.fillMaxWidth(),
      )
    }

    HorizontalDivider()

    val target = state.defaultReader
    if (target != null && !state.alwaysAsk) {
      Text("Sending to ${target.name}", style = MaterialTheme.typography.titleMedium)
      Button(onClick = onSendToDefault, enabled = state.canSend, modifier = Modifier.fillMaxWidth()) {
        Text("Send")
      }
      OutlinedButton(onClick = onChooseReader, modifier = Modifier.fillMaxWidth()) {
        Text("Send to a different reader")
      }
    } else {
      Button(onClick = onChooseReader, enabled = state.canSend, modifier = Modifier.fillMaxWidth()) {
        Text("Choose reader")
      }
    }

    TextButton(onClick = onDone, modifier = Modifier.fillMaxWidth()) { Text("Pick a different book") }
  }
}

@Composable
private fun ChooseReaderScreen(
  state: SendUiState,
  onSelectReader: (DiscoveredReader) -> Unit,
  onRescan: () -> Unit,
  onBack: () -> Unit,
) {
  Column(modifier = Modifier.fillMaxSize()) {
    Row(
      modifier = Modifier.fillMaxWidth(),
      horizontalArrangement = Arrangement.SpaceBetween,
      verticalAlignment = Alignment.CenterVertically,
    ) {
      Text("Readers Nearby", style = MaterialTheme.typography.titleMedium)
      if (state.scanning) {
        CircularProgressIndicator(modifier = Modifier.height(20.dp))
      } else {
        TextButton(onClick = onRescan) { Text("Search again") }
      }
    }

    state.message?.let {
      Spacer(Modifier.height(8.dp))
      Text(it, style = MaterialTheme.typography.bodyMedium)
    }

    Spacer(Modifier.height(8.dp))

    if (state.readers.isEmpty()) {
      Text(
        "Open Device Connections → Bluetooth Transfer on your reader and it will appear here.",
        style = MaterialTheme.typography.bodyMedium,
      )
    } else {
      LazyColumn(
        modifier = Modifier.fillMaxWidth().height(360.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
      ) {
        items(state.readers, key = { it.address }) { reader ->
          Card(modifier = Modifier.fillMaxWidth()) {
            Row(
              modifier = Modifier.fillMaxWidth().padding(16.dp),
              horizontalArrangement = Arrangement.SpaceBetween,
              verticalAlignment = Alignment.CenterVertically,
            ) {
              Column {
                Text(reader.name, style = MaterialTheme.typography.titleSmall)
                Text(reader.address, style = MaterialTheme.typography.bodySmall)
              }
              Button(onClick = { onSelectReader(reader) }) { Text("Send") }
            }
          }
        }
      }
    }

    Spacer(Modifier.height(16.dp))
    Text(
      "The reader you pick becomes the one this app sends to by default.",
      style = MaterialTheme.typography.bodySmall,
    )
    TextButton(onClick = onBack, modifier = Modifier.fillMaxWidth()) { Text("Back") }
  }
}

@Composable
private fun SettingsScreen(
  state: SendUiState,
  onForgetReader: () -> Unit,
  onAlwaysAskChanged: (Boolean) -> Unit,
  onClose: () -> Unit,
) {
  Column(verticalArrangement = Arrangement.spacedBy(16.dp)) {
    Text("Default reader", style = MaterialTheme.typography.titleMedium)

    val target = state.defaultReader
    if (target == null) {
      Text("None yet. The first reader you send to becomes the default.", style = MaterialTheme.typography.bodyMedium)
    } else {
      Text(target.name, style = MaterialTheme.typography.bodyLarge)
      Text(target.address, style = MaterialTheme.typography.bodySmall)
      OutlinedButton(onClick = onForgetReader, modifier = Modifier.fillMaxWidth()) { Text("Forget this reader") }
    }

    HorizontalDivider()

    Row(
      modifier = Modifier.fillMaxWidth(),
      horizontalArrangement = Arrangement.SpaceBetween,
      verticalAlignment = Alignment.CenterVertically,
    ) {
      Column(modifier = Modifier.fillMaxWidth(0.8f)) {
        Text("Always ask which reader", style = MaterialTheme.typography.bodyLarge)
        Text(
          "Show the list every time instead of sending straight to the default.",
          style = MaterialTheme.typography.bodySmall,
        )
      }
      Switch(checked = state.alwaysAsk, onCheckedChange = onAlwaysAskChanged)
    }

    HorizontalDivider()

    Text(
      "The reader's own name is set on the reader, under Settings → Device name, or from its " +
        "web manager. It is the same name it uses for <name>.local.",
      style = MaterialTheme.typography.bodySmall,
    )

    Button(onClick = onClose, modifier = Modifier.fillMaxWidth()) { Text("Done") }
  }
}

@Composable
private fun PreparingScreen(state: SendUiState) {
  Column(verticalArrangement = Arrangement.spacedBy(16.dp)) {
    Text("Preparing ${state.editedName}", style = MaterialTheme.typography.titleMedium)
    Text(state.message ?: "Checking the book before sending", style = MaterialTheme.typography.bodyMedium)
    LinearProgressIndicator(modifier = Modifier.fillMaxWidth())
  }
}

@Composable
private fun SendingScreen(state: SendUiState, onCancel: () -> Unit) {
  Column(verticalArrangement = Arrangement.spacedBy(16.dp)) {
    Text("Sending to ${state.readerName}", style = MaterialTheme.typography.titleMedium)
    Text(state.book?.name.orEmpty(), style = MaterialTheme.typography.bodyLarge)

    LinearProgressIndicator(progress = { state.percent / 100f }, modifier = Modifier.fillMaxWidth())

    Text("${state.percent}%", style = MaterialTheme.typography.headlineSmall)
    Text(
      "${formatBytes(state.sentBytes)} / ${formatBytes(state.totalBytes)}",
      style = MaterialTheme.typography.bodyMedium,
    )

    OutlinedButton(onClick = onCancel, modifier = Modifier.fillMaxWidth()) { Text("Cancel") }
  }
}

@Composable
private fun SentScreen(state: SendUiState, onDone: () -> Unit) {
  Column(verticalArrangement = Arrangement.spacedBy(16.dp)) {
    Text("Sent", style = MaterialTheme.typography.headlineSmall)
    Text(state.book?.name.orEmpty(), style = MaterialTheme.typography.titleMedium)
    Text("Your book is now on ${state.readerName}.", style = MaterialTheme.typography.bodyLarge)
    Button(onClick = onDone, modifier = Modifier.fillMaxWidth()) { Text("Done") }
  }
}

@Composable
private fun FailedScreen(state: SendUiState, onDone: () -> Unit, onBackToPrepare: () -> Unit) {
  Column(verticalArrangement = Arrangement.spacedBy(16.dp)) {
    Text("Not sent", style = MaterialTheme.typography.headlineSmall)
    state.book?.let { Text(it.name, style = MaterialTheme.typography.titleMedium) }
    Text(
      state.message ?: "The transfer did not finish",
      style = MaterialTheme.typography.bodyLarge,
      textAlign = TextAlign.Start,
    )
    state.errorCode?.let { Text(it, style = MaterialTheme.typography.bodySmall) }
    Text("Nothing was added to the reader's library.", style = MaterialTheme.typography.bodyMedium)

    if (state.book != null && state.book.isSupported) {
      Button(onClick = onBackToPrepare, modifier = Modifier.fillMaxWidth()) { Text("Try again") }
    }
    OutlinedButton(onClick = onDone, modifier = Modifier.fillMaxWidth()) { Text("Start over") }
  }
}
