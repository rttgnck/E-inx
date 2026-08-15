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
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
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
  onSend: (DiscoveredReader) -> Unit,
  onRescan: () -> Unit,
  onCancel: () -> Unit,
  onDone: () -> Unit,
  onChooseAnotherReader: () -> Unit,
) {
  Column(
    modifier = Modifier
      .fillMaxSize()
      .padding(24.dp),
  ) {
    Text("E-inx Send", style = MaterialTheme.typography.headlineMedium)
    Spacer(Modifier.height(24.dp))

    when (state.stage) {
      SendStage.PickBook -> PickBookScreen(onChooseBook)
      SendStage.ChooseReader -> ChooseReaderScreen(state, onSend, onRescan)
      SendStage.Preparing -> PreparingScreen(state)
      SendStage.Sending -> SendingScreen(state, onCancel)
      SendStage.Sent -> SentScreen(state, onDone)
      SendStage.Failed -> FailedScreen(state, onDone, onChooseAnotherReader)
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
    Button(onClick = onChooseBook, modifier = Modifier.fillMaxWidth()) {
      Text("Choose Book")
    }
    Text(
      "EPUB, TXT, XTC and XTCH",
      style = MaterialTheme.typography.bodySmall,
    )
    Spacer(Modifier.height(8.dp))
    Text(
      "On the reader: Device Connections → Bluetooth Transfer",
      style = MaterialTheme.typography.bodyMedium,
    )
  }
}

@Composable
private fun ChooseReaderScreen(
  state: SendUiState,
  onSend: (DiscoveredReader) -> Unit,
  onRescan: () -> Unit,
) {
  Column(modifier = Modifier.fillMaxSize()) {
    state.book?.let { book ->
      Text(book.name, style = MaterialTheme.typography.titleMedium)
      Text(formatBytes(book.size), style = MaterialTheme.typography.bodySmall)
      Spacer(Modifier.height(24.dp))
    }

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
      LazyColumn(verticalArrangement = Arrangement.spacedBy(12.dp)) {
        items(state.readers, key = { it.address }) { reader ->
          Card(modifier = Modifier.fillMaxWidth()) {
            Row(
              modifier = Modifier
                .fillMaxWidth()
                .padding(16.dp),
              horizontalArrangement = Arrangement.SpaceBetween,
              verticalAlignment = Alignment.CenterVertically,
            ) {
              Column {
                Text(reader.name, style = MaterialTheme.typography.titleSmall)
                Text(reader.address, style = MaterialTheme.typography.bodySmall)
              }
              Button(onClick = { onSend(reader) }) { Text("Send") }
            }
          }
        }
      }
    }
  }
}

@Composable
private fun PreparingScreen(state: SendUiState) {
  Column(verticalArrangement = Arrangement.spacedBy(16.dp)) {
    Text("Preparing ${state.book?.name.orEmpty()}", style = MaterialTheme.typography.titleMedium)
    Text("Checking the book before sending", style = MaterialTheme.typography.bodyMedium)
    LinearProgressIndicator(modifier = Modifier.fillMaxWidth())
  }
}

@Composable
private fun SendingScreen(state: SendUiState, onCancel: () -> Unit) {
  Column(verticalArrangement = Arrangement.spacedBy(16.dp)) {
    Text("Sending to ${state.readerName}", style = MaterialTheme.typography.titleMedium)
    Text(state.book?.name.orEmpty(), style = MaterialTheme.typography.bodyLarge)

    LinearProgressIndicator(
      progress = { state.percent / 100f },
      modifier = Modifier.fillMaxWidth(),
    )

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
    Text(
      "Your book is now on ${state.readerName}.",
      style = MaterialTheme.typography.bodyLarge,
    )
    Button(onClick = onDone, modifier = Modifier.fillMaxWidth()) { Text("Done") }
  }
}

@Composable
private fun FailedScreen(
  state: SendUiState,
  onDone: () -> Unit,
  onChooseAnotherReader: () -> Unit,
) {
  Column(verticalArrangement = Arrangement.spacedBy(16.dp)) {
    Text("Not sent", style = MaterialTheme.typography.headlineSmall)
    state.book?.let { Text(it.name, style = MaterialTheme.typography.titleMedium) }
    Text(
      state.message ?: "The transfer did not finish",
      style = MaterialTheme.typography.bodyLarge,
      textAlign = TextAlign.Start,
    )
    state.errorCode?.let {
      Text(it, style = MaterialTheme.typography.bodySmall)
    }
    Text(
      "Nothing was added to the reader's library.",
      style = MaterialTheme.typography.bodyMedium,
    )

    if (state.book != null && state.book.isSupported) {
      Button(onClick = onChooseAnotherReader, modifier = Modifier.fillMaxWidth()) {
        Text("Try again")
      }
    }
    OutlinedButton(onClick = onDone, modifier = Modifier.fillMaxWidth()) { Text("Start over") }
  }
}
