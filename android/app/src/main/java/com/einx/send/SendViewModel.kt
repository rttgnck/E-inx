package com.einx.send

import android.app.Application
import android.net.Uri
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/** Which screen the app is on. */
enum class SendStage { PickBook, Prepare, ChooseReader, Preparing, Sending, Sent, Failed, Settings }

data class SendUiState(
  val stage: SendStage = SendStage.PickBook,
  val book: BookFile? = null,
  /** What the user has typed. Separate from [book] so editing never disturbs the source Uri. */
  val editedName: String = "",
  val editedTitle: String = "",
  val editedAuthor: String = "",
  val originalMetadata: EpubMetadata = EpubMetadata.EMPTY,
  val readers: List<DiscoveredReader> = emptyList(),
  val scanning: Boolean = false,
  val defaultReader: DefaultReader? = null,
  val alwaysAsk: Boolean = false,
  val sentBytes: Long = 0,
  val totalBytes: Long = 0,
  val readerName: String = "",
  val message: String? = null,
  val errorCode: String? = null,
) {
  val percent: Int
    get() = if (totalBytes <= 0) 0 else ((sentBytes * 100) / totalBytes).toInt()

  /** True when the typed name would be refused by the reader. */
  val nameError: String?
    get() = when {
      editedName.isBlank() -> "The file needs a name"
      !BleProtocol.isSupportedFilename(editedName) -> "Must end in .epub, .txt, .xtc or .xtch"
      editedName != BookFiles.sanitizeName(editedName) -> "Cannot contain / \\ : * ? \" < > |"
      else -> null
    }

  val canSend: Boolean get() = nameError == null && book != null
}

class SendViewModel(application: Application) : AndroidViewModel(application) {

  private val scanner = ReaderScanner(application)
  private val client = EinxTransferClient(application)
  private val prefs = SendPreferences(application)

  private val _state = MutableStateFlow(
    SendUiState(defaultReader = prefs.defaultReader, alwaysAsk = prefs.alwaysAsk)
  )
  val state: StateFlow<SendUiState> = _state.asStateFlow()

  private var transferJob: Job? = null

  /** Accepts a book from the picker or from the Share sheet. */
  fun onBookChosen(uri: Uri) {
    viewModelScope.launch {
      val resolver = getApplication<Application>().contentResolver
      val book = withContext(Dispatchers.IO) { BookFiles.resolve(resolver, uri) }

      if (book == null) {
        _state.update {
          it.copy(stage = SendStage.Failed, message = "That file could not be opened", errorCode = "ERR_READ")
        }
        return@launch
      }

      if (!book.isSupported) {
        _state.update {
          it.copy(
            stage = SendStage.Failed,
            book = book,
            message = "E-inx readers take EPUB, TXT, XTC and XTCH files",
            errorCode = "ERR_EXT",
          )
        }
        return@launch
      }

      // Start the edit fields from what the book says about itself, so the user is correcting
      // something rather than typing into a blank box.
      val metadata = if (book.extension == "epub") {
        withContext(Dispatchers.IO) {
          runCatching { resolver.openInputStream(book.uri)!!.use { EpubMetadataReader.read(it) } }
            .getOrDefault(EpubMetadata.EMPTY)
        }
      } else {
        EpubMetadata.EMPTY
      }

      _state.update {
        it.copy(
          stage = SendStage.Prepare,
          book = book,
          editedName = book.name,
          editedTitle = metadata.title,
          editedAuthor = metadata.author,
          originalMetadata = metadata,
          message = null,
          errorCode = null,
        )
      }
    }
  }

  fun onNameChanged(value: String) = _state.update { it.copy(editedName = value) }
  fun onTitleChanged(value: String) = _state.update { it.copy(editedTitle = value) }
  fun onAuthorChanged(value: String) = _state.update { it.copy(editedAuthor = value) }

  fun openSettings() {
    stopScan()
    _state.update { it.copy(stage = SendStage.Settings, defaultReader = prefs.defaultReader, alwaysAsk = prefs.alwaysAsk) }
  }

  fun closeSettings() {
    _state.update { it.copy(stage = if (it.book == null) SendStage.PickBook else SendStage.Prepare) }
  }

  fun forgetDefaultReader() {
    prefs.defaultReader = null
    _state.update { it.copy(defaultReader = null) }
  }

  fun setAlwaysAsk(value: Boolean) {
    prefs.alwaysAsk = value
    _state.update { it.copy(alwaysAsk = value) }
  }

  /** Opens the reader list. Also the "send to a different reader" route. */
  fun chooseReader() {
    _state.update { it.copy(stage = SendStage.ChooseReader, readers = emptyList(), message = null) }
    startScan()
  }

  fun startScan() {
    _state.update { it.copy(scanning = true, message = null) }
    scanner.start(
      onFound = { reader ->
        _state.update { current ->
          // Readers re-advertise constantly; keep one entry each and take the newest RSSI.
          val existing = current.readers.filterNot { it.address == reader.address }
          current.copy(readers = (existing + reader).sortedByDescending { it.rssi })
        }
      },
      onFailed = { reason -> _state.update { it.copy(scanning = false, message = reason) } },
    )
  }

  fun stopScan() {
    scanner.stop()
    _state.update { it.copy(scanning = false) }
  }

  /** Picks a reader from the list and remembers it, then sends. */
  fun selectAndSend(reader: DiscoveredReader) {
    prefs.defaultReader = DefaultReader(reader.address, reader.name)
    _state.update { it.copy(defaultReader = prefs.defaultReader) }
    send(reader)
  }

  /**
   * Sends to the remembered reader without showing the list.
   *
   * The remembered address is not enough on its own — BLE needs a device that has actually been
   * seen — so this scans until the default turns up, and falls back to the list if it does not.
   */
  fun sendToDefault() {
    val target = _state.value.defaultReader ?: run { chooseReader(); return }

    _state.update { it.copy(stage = SendStage.Preparing, readerName = target.name, message = "Looking for ${target.name}…") }
    scanner.start(
      onFound = { reader ->
        if (reader.address == target.address) {
          stopScan()
          send(reader)
        }
      },
      onFailed = { reason -> _state.update { it.copy(stage = SendStage.ChooseReader, message = reason) } },
    )
  }

  fun send(reader: DiscoveredReader) {
    val current = _state.value
    val book = current.book ?: return
    stopScan()

    _state.update {
      it.copy(
        stage = SendStage.Preparing,
        readerName = reader.name,
        sentBytes = 0,
        totalBytes = book.size,
        message = null,
        errorCode = null,
      )
    }

    transferJob = viewModelScope.launch {
      val resolver = getApplication<Application>().contentResolver

      // One streaming pass for the checksum. The book is never held in memory.
      val prepared = withContext(Dispatchers.IO) {
        runCatching {
          resolver.openInputStream(book.uri)!!.use { stream ->
            val (crc, counted) = BookFiles.checksum(stream)
            book.copy(
              name = BookFiles.sanitizeName(current.editedName),
              size = counted,
              crc32 = crc,
              title = current.editedTitle,
              author = current.editedAuthor,
            )
          }
        }.getOrNull()
      }

      if (prepared == null) {
        _state.update {
          it.copy(stage = SendStage.Failed, message = "Could not read the book from this phone", errorCode = "ERR_READ")
        }
        return@launch
      }

      _state.update { it.copy(stage = SendStage.Sending, book = prepared, totalBytes = prepared.size) }

      val outcome = withContext(Dispatchers.IO) {
        client.send(reader.device, prepared, resolver) { sent, total ->
          _state.update { it.copy(sentBytes = sent, totalBytes = total) }
        }
      }

      when (outcome) {
        is TransferOutcome.Success ->
          _state.update { it.copy(stage = SendStage.Sent, sentBytes = it.totalBytes) }

        is TransferOutcome.Failure ->
          _state.update { it.copy(stage = SendStage.Failed, message = outcome.message, errorCode = outcome.code) }
      }
    }
  }

  fun cancel() {
    client.requestCancel()
  }

  /** Back to the prepare screen, keeping the book and everything typed about it. */
  fun backToPrepare() {
    stopScan()
    _state.update { it.copy(stage = SendStage.Prepare, message = null, errorCode = null) }
  }

  fun reset() {
    transferJob?.cancel()
    stopScan()
    _state.value = SendUiState(defaultReader = prefs.defaultReader, alwaysAsk = prefs.alwaysAsk)
  }

  override fun onCleared() {
    scanner.stop()
    client.requestCancel()
    super.onCleared()
  }
}
