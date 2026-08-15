/**
 * @file BluetoothTransferServer.cpp
 * @brief Definitions for BluetoothTransferServer.
 */

#include "BluetoothTransferServer.h"

#ifdef SIMULATOR

#include <HardwareSerial.h>

/** The simulator has no radio; every entry point is a no-op that reports "unsupported". */
BluetoothTransferServer::~BluetoothTransferServer() = default;

bool BluetoothTransferServer::isSupported() { return false; }

std::string BluetoothTransferServer::advertisedName() { return "E-inx X3"; }

bool BluetoothTransferServer::begin() {
  Serial.printf("[%lu] [BLE] Simulator build has no Bluetooth\n", millis());
  return false;
}

void BluetoothTransferServer::end() {}

void BluetoothTransferServer::poll() {}

void BluetoothTransferServer::cancelTransfer() {}

bool BluetoothTransferServer::isConnected() const { return false; }

BleTransferStatus BluetoothTransferServer::status() const {
  BleTransferStatus snapshot;
  snapshot.state = BleTransferState::Failed;
  snapshot.errorCode = "ERR_UNSUPPORTED";
  snapshot.errorMessage = "Bluetooth is not available in the simulator";
  return snapshot;
}

#else

#include <ArduinoJson.h>
#include <HardwareSerial.h>
#include <NimBLEDevice.h>
#include <SDCardManager.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace {

// ---------------------------------------------------------------------------
// Protocol constants — keep in step with docs/BLE_FILE_TRANSFER.md
// ---------------------------------------------------------------------------

constexpr uint8_t PROTOCOL_VERSION = 1;

constexpr const char* SERVICE_UUID = "e1780001-5b41-4d2e-9a63-7c8f1b0d4e21";
constexpr const char* CONTROL_UUID = "e1780002-5b41-4d2e-9a63-7c8f1b0d4e21";
constexpr const char* DATA_UUID = "e1780003-5b41-4d2e-9a63-7c8f1b0d4e21";
constexpr const char* STATUS_UUID = "e1780004-5b41-4d2e-9a63-7c8f1b0d4e21";

/** Where accepted books land. Created on demand. */
constexpr const char* BOOKS_DIR = "/Books";

/** Anything larger is refused outright rather than half-written and then rejected. */
constexpr uint32_t MAX_TRANSFER_BYTES = 64u * 1024u * 1024u;

/** Long enough for a real title, short enough to stay inside FAT's 255-byte name limit with ".part". */
constexpr size_t MAX_NAME_LENGTH = 96;

/** Bytes held in RAM between the radio and the card. Never the whole book — see the header. */
constexpr size_t RING_CAPACITY = 16 * 1024;

/** How much is handed to SdFat per write. */
constexpr size_t SD_CHUNK = 4096;

/** How long a DATA write will wait for the card to catch up before giving up on the transfer. */
constexpr uint32_t RING_WAIT_TIMEOUT_MS = 4000;

/** Minimum spacing between progress notifications, so the link is not spent on status traffic. */
constexpr uint32_t PROGRESS_INTERVAL_MS = 400;
constexpr uint32_t PROGRESS_INTERVAL_BYTES = 64 * 1024;

const char* const SUPPORTED_EXTENSIONS[] = {".epub", ".txt", ".xtc", ".xtch"};

// ---------------------------------------------------------------------------
// CRC32 (IEEE 802.3, the same polynomial zip and java.util.zip.CRC32 use)
// ---------------------------------------------------------------------------

/** Nibble-wise table: 64 bytes of flash instead of 1 KB, still one pass over the data. */
constexpr uint32_t CRC32_NIBBLES[16] = {0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4,
                                        0x4DB26158, 0x5005713C, 0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
                                        0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C};

uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t length) {
  crc = ~crc;
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    crc = (crc >> 4) ^ CRC32_NIBBLES[crc & 0x0F];
    crc = (crc >> 4) ^ CRC32_NIBBLES[crc & 0x0F];
  }
  return ~crc;
}

// ---------------------------------------------------------------------------
// Shared state
// ---------------------------------------------------------------------------

/**
 * What the GATT callbacks and poll() both touch. Guarded by stateMutex, which is held only
 * for field access and never across an SD or BLE call.
 */
struct SharedState {
  BleTransferState state = BleTransferState::Idle;
  bool connected = false;
  bool advertising = false;

  std::string filename;
  std::string tempPath;
  std::string finalPath;

  uint32_t total = 0;         /**< Size promised by START */
  uint32_t accepted = 0;      /**< Bytes taken off the radio */
  uint32_t written = 0;       /**< Bytes committed to the card */
  uint32_t expectedCrc = 0;   /**< CRC32 promised by START */
  unsigned long startedAt = 0;

  bool openRequested = false;   /**< START validated; poll() still has to open the file */
  bool finishRequested = false; /**< FINISH arrived; poll() verifies once the ring is drained */
  bool abortRequested = false;  /**< ABORT, disconnect or a fatal write; poll() cleans up */
  std::string abortCode;
  std::string abortMessage;

  std::string errorCode;
  std::string errorMessage;
  std::string lastCompletedName;
  unsigned long lastCompletedAt = 0;
  uint32_t lastCompletedBytes = 0;
  unsigned long lastCompletedMs = 0;
};

SharedState gState;
SemaphoreHandle_t gStateMutex = nullptr;

/** Ring buffer between the NimBLE task (producer) and poll() (consumer). */
uint8_t* gRing = nullptr;
size_t gRingHead = 0; /**< Write position */
size_t gRingTail = 0; /**< Read position */
size_t gRingUsed = 0;
SemaphoreHandle_t gRingMutex = nullptr;

NimBLEServer* gServer = nullptr;
NimBLECharacteristic* gStatusCharacteristic = nullptr;
FsFile gFile;
uint32_t gRunningCrc = 0;
unsigned long gLastProgressAt = 0;
uint32_t gLastProgressBytes = 0;

/** RAII lock so no early return can leave a mutex held. */
class Lock {
 public:
  explicit Lock(SemaphoreHandle_t handle) : handle_(handle) {
    if (handle_) {
      xSemaphoreTake(handle_, portMAX_DELAY);
    }
  }
  ~Lock() {
    if (handle_) {
      xSemaphoreGive(handle_);
    }
  }
  Lock(const Lock&) = delete;
  Lock& operator=(const Lock&) = delete;

 private:
  SemaphoreHandle_t handle_;
};

// ---------------------------------------------------------------------------
// STATUS notifications
// ---------------------------------------------------------------------------

void notifyStatus(const std::string& payload) {
  if (!gStatusCharacteristic) {
    return;
  }
  gStatusCharacteristic->setValue(reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
  gStatusCharacteristic->notify();
}

void notifyReady() {
  JsonDocument doc;
  doc["st"] = "ready";
  doc["proto"] = PROTOCOL_VERSION;
  doc["name"] = BluetoothTransferServer::advertisedName();
  doc["maxSize"] = MAX_TRANSFER_BYTES;
  std::string out;
  serializeJson(doc, out);
  notifyStatus(out);
}

void notifyReceiving(const std::string& filename, uint32_t total) {
  JsonDocument doc;
  doc["st"] = "receiving";
  doc["name"] = filename;
  doc["total"] = total;
  std::string out;
  serializeJson(doc, out);
  notifyStatus(out);
}

void notifyProgress(uint32_t received, uint32_t total) {
  JsonDocument doc;
  doc["st"] = "progress";
  doc["recv"] = received;
  doc["total"] = total;
  std::string out;
  serializeJson(doc, out);
  notifyStatus(out);
}

void notifyComplete(const std::string& filename, uint32_t size) {
  JsonDocument doc;
  doc["st"] = "complete";
  doc["name"] = filename;
  doc["size"] = size;
  std::string out;
  serializeJson(doc, out);
  notifyStatus(out);
}

void notifyError(const char* code, const char* message) {
  JsonDocument doc;
  doc["st"] = "error";
  doc["code"] = code;
  doc["msg"] = message;
  std::string out;
  serializeJson(doc, out);
  notifyStatus(out);
  Serial.printf("[%lu] [BLE] %s: %s\n", millis(), code, message);
}

// ---------------------------------------------------------------------------
// Ring buffer
// ---------------------------------------------------------------------------

void ringReset() {
  Lock lock(gRingMutex);
  gRingHead = 0;
  gRingTail = 0;
  gRingUsed = 0;
}

/** Copies in as much as fits right now and reports how much that was. */
size_t ringPush(const uint8_t* data, size_t length) {
  Lock lock(gRingMutex);
  const size_t room = std::min(length, RING_CAPACITY - gRingUsed);
  for (size_t i = 0; i < room; ++i) {
    gRing[gRingHead] = data[i];
    gRingHead = (gRingHead + 1) % RING_CAPACITY;
  }
  gRingUsed += room;
  return room;
}

/** Copies out up to `length` bytes into `out`, reporting how many were available. */
size_t ringPop(uint8_t* out, size_t length) {
  Lock lock(gRingMutex);
  const size_t available = std::min(length, gRingUsed);
  for (size_t i = 0; i < available; ++i) {
    out[i] = gRing[gRingTail];
    gRingTail = (gRingTail + 1) % RING_CAPACITY;
  }
  gRingUsed -= available;
  return available;
}

size_t ringUsed() {
  Lock lock(gRingMutex);
  return gRingUsed;
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

std::string toLower(const std::string& value) {
  std::string out = value;
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

bool hasSupportedExtension(const std::string& name) {
  const std::string lowered = toLower(name);
  for (const char* extension : SUPPORTED_EXTENSIONS) {
    const size_t length = strlen(extension);
    if (lowered.size() > length && lowered.compare(lowered.size() - length, length, extension) == 0) {
      return true;
    }
  }
  return false;
}

/**
 * Accepts only a bare file name that is safe to append to a directory path.
 *
 * Rejecting every separator and every dot-leading name is what keeps a sender from
 * writing outside /Books or from creating a name that collides with our own .part files.
 */
bool isSafeFilename(const std::string& name) {
  if (name.empty() || name.size() > MAX_NAME_LENGTH) {
    return false;
  }
  if (name.front() == '.' || name.front() == ' ') {
    return false;
  }
  if (name.find("..") != std::string::npos) {
    return false;
  }
  for (const char c : name) {
    const auto byte = static_cast<unsigned char>(c);
    if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
      return false;
    }
    if (byte < 0x20 || byte == 0x7F) {
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Transfer teardown helpers (main task only — they touch the card)
// ---------------------------------------------------------------------------

/** Closes and removes the .part file, then puts the state machine back to waiting. */
void discardPartial(const char* code, const char* message) {
  std::string tempPath;
  {
    Lock lock(gStateMutex);
    tempPath = gState.tempPath;
  }

  if (gFile) {
    gFile.close();
  }
  if (!tempPath.empty() && SdMan.exists(tempPath.c_str())) {
    SdMan.remove(tempPath.c_str());
  }

  ringReset();
  gRunningCrc = 0;

  {
    Lock lock(gStateMutex);
    gState.state = BleTransferState::Failed;
    gState.errorCode = code;
    gState.errorMessage = message;
    gState.filename.clear();
    gState.tempPath.clear();
    gState.finalPath.clear();
    gState.total = 0;
    gState.accepted = 0;
    gState.written = 0;
    gState.openRequested = false;
    gState.finishRequested = false;
    gState.abortRequested = false;
    gState.abortCode.clear();
    gState.abortMessage.clear();
  }
}

/** Marks the current transfer as doomed; poll() does the cleanup on the main task. */
void requestAbort(const char* code, const char* message) {
  Lock lock(gStateMutex);
  if (gState.abortRequested) {
    return;
  }
  gState.abortRequested = true;
  gState.abortCode = code;
  gState.abortMessage = message;
}

// ---------------------------------------------------------------------------
// GATT callbacks — NimBLE host task, RAM only
// ---------------------------------------------------------------------------

/** Handles a validated START by recording it; poll() opens the file. */
void handleStart(const JsonDocument& doc) {
  {
    Lock lock(gStateMutex);
    if (gState.state == BleTransferState::Receiving) {
      notifyError("ERR_BUSY", "A transfer is already in progress");
      return;
    }
  }

  const uint32_t protocol = doc["protocol"] | 0u;
  if (protocol != PROTOCOL_VERSION) {
    notifyError("ERR_PROTOCOL", "Unsupported protocol version");
    return;
  }

  const char* rawName = doc["name"] | "";
  const std::string name = rawName;
  if (!isSafeFilename(name)) {
    notifyError("ERR_NAME", "File name is not accepted");
    return;
  }
  if (!hasSupportedExtension(name)) {
    notifyError("ERR_EXT", "Only .epub, .txt, .xtc and .xtch are supported");
    return;
  }

  const uint32_t size = doc["size"] | 0u;
  if (size == 0 || size > MAX_TRANSFER_BYTES) {
    notifyError("ERR_SIZE", "File size is zero or too large");
    return;
  }

  // Sent as hex text so it survives languages without an unsigned 32-bit type.
  const char* crcText = doc["crc32"] | "";
  char* parseEnd = nullptr;
  const auto crc = static_cast<uint32_t>(strtoul(crcText, &parseEnd, 16));
  if (crcText[0] == '\0' || parseEnd == crcText || *parseEnd != '\0') {
    notifyError("ERR_PROTOCOL", "crc32 must be a hex string");
    return;
  }

  {
    Lock lock(gStateMutex);
    gState.filename = name;
    gState.finalPath = std::string(BOOKS_DIR) + "/" + name;
    gState.tempPath = std::string(BOOKS_DIR) + "/." + name + ".part";
    gState.total = size;
    gState.accepted = 0;
    gState.written = 0;
    gState.expectedCrc = crc;
    gState.openRequested = true;
    gState.finishRequested = false;
    gState.abortRequested = false;
    gState.errorCode.clear();
    gState.errorMessage.clear();
    gState.startedAt = millis();
  }

  ringReset();
  Serial.printf("[%lu] [BLE] START %s (%u bytes, crc %08x)\n", millis(), name.c_str(), size, crc);
}

class ControlCallbacks final : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic) override {
    const std::string value = characteristic->getValue();
    if (value.empty()) {
      return;
    }

    JsonDocument doc;
    if (deserializeJson(doc, value) != DeserializationError::Ok) {
      notifyError("ERR_JSON", "CONTROL payload is not valid JSON");
      return;
    }

    // START may omit "cmd" — it is recognisable by carrying a file name.
    const char* command = doc["cmd"] | (doc["name"].is<const char*>() ? "start" : "");

    if (strcmp(command, "start") == 0) {
      handleStart(doc);
      return;
    }

    if (strcmp(command, "finish") == 0) {
      Lock lock(gStateMutex);
      if (gState.state != BleTransferState::Receiving) {
        notifyError("ERR_STATE", "No transfer to finish");
        return;
      }
      gState.finishRequested = true;
      return;
    }

    if (strcmp(command, "abort") == 0) {
      requestAbort("ERR_ABORTED", "Cancelled from the phone");
      return;
    }

    notifyError("ERR_PROTOCOL", "Unknown CONTROL command");
  }
};

class DataCallbacks final : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic) override {
    const std::string value = characteristic->getValue();
    if (value.empty()) {
      return;
    }

    {
      Lock lock(gStateMutex);
      if (gState.state != BleTransferState::Receiving || gState.abortRequested) {
        return;
      }
      if (gState.accepted + value.size() > gState.total) {
        gState.abortRequested = true;
        gState.abortCode = "ERR_OVERFLOW";
        gState.abortMessage = "Sender sent more data than it promised";
        return;
      }
      gState.accepted += value.size();
    }

    // Blocking here is the back-pressure: the link layer stops acknowledging while the
    // card catches up, which is what keeps the phone from outrunning the SD writes.
    const auto* data = reinterpret_cast<const uint8_t*>(value.data());
    size_t offset = 0;
    const unsigned long deadline = millis() + RING_WAIT_TIMEOUT_MS;
    while (offset < value.size()) {
      offset += ringPush(data + offset, value.size() - offset);
      if (offset < value.size()) {
        if (millis() > deadline) {
          requestAbort("ERR_STORAGE", "SD card could not keep up with the transfer");
          return;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
      }
    }
  }
};

class ServerCallbacks final : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* /*server*/) override {
    {
      Lock lock(gStateMutex);
      gState.connected = true;
    }
    Serial.printf("[%lu] [BLE] Phone connected\n", millis());
    notifyReady();
  }

  void onDisconnect(NimBLEServer* /*server*/) override {
    bool wasReceiving = false;
    {
      Lock lock(gStateMutex);
      gState.connected = false;
      wasReceiving = gState.state == BleTransferState::Receiving;
    }

    if (wasReceiving) {
      requestAbort("ERR_DISCONNECTED", "Phone disconnected during the transfer");
    }

    Serial.printf("[%lu] [BLE] Phone disconnected, advertising again\n", millis());
    NimBLEDevice::startAdvertising();
  }
};

ControlCallbacks gControlCallbacks;
DataCallbacks gDataCallbacks;
ServerCallbacks gServerCallbacks;

}  // namespace

// ---------------------------------------------------------------------------
// BluetoothTransferServer
// ---------------------------------------------------------------------------

BluetoothTransferServer::~BluetoothTransferServer() { end(); }

bool BluetoothTransferServer::isSupported() { return true; }

std::string BluetoothTransferServer::advertisedName() {
  const uint64_t mac = ESP.getEfuseMac();
  char suffix[5];
  snprintf(suffix, sizeof(suffix), "%02X%02X", static_cast<unsigned>((mac >> 8) & 0xFF),
           static_cast<unsigned>(mac & 0xFF));
  return std::string("E-inx X3 ") + suffix;
}

bool BluetoothTransferServer::begin() {
  if (running_) {
    return true;
  }

  if (!gStateMutex) {
    gStateMutex = xSemaphoreCreateMutex();
  }
  if (!gRingMutex) {
    gRingMutex = xSemaphoreCreateMutex();
  }
  if (!gRing) {
    gRing = static_cast<uint8_t*>(malloc(RING_CAPACITY));
  }
  if (!gStateMutex || !gRingMutex || !gRing) {
    Serial.printf("[%lu] [BLE] Not enough memory to start\n", millis());
    end();
    return false;
  }

  {
    Lock lock(gStateMutex);
    gState = SharedState{};
  }
  ringReset();
  gRunningCrc = 0;

  if (!SdMan.ensureDirectoryExists(BOOKS_DIR)) {
    Serial.printf("[%lu] [BLE] Could not create %s\n", millis(), BOOKS_DIR);
  }

  NimBLEDevice::init(advertisedName());
  NimBLEDevice::setMTU(517);

  gServer = NimBLEDevice::createServer();
  if (!gServer) {
    Serial.printf("[%lu] [BLE] createServer failed\n", millis());
    end();
    return false;
  }
  gServer->setCallbacks(&gServerCallbacks, false);

  NimBLEService* service = gServer->createService(SERVICE_UUID);
  if (!service) {
    Serial.printf("[%lu] [BLE] createService failed\n", millis());
    end();
    return false;
  }

  NimBLECharacteristic* control = service->createCharacteristic(CONTROL_UUID, NIMBLE_PROPERTY::WRITE);
  control->setCallbacks(&gControlCallbacks);

  NimBLECharacteristic* data =
      service->createCharacteristic(DATA_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  data->setCallbacks(&gDataCallbacks);

  gStatusCharacteristic = service->createCharacteristic(STATUS_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);

  service->start();

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->setScanResponse(true);
  advertising->setName(advertisedName());
  advertising->start();

  running_ = true;
  {
    Lock lock(gStateMutex);
    gState.advertising = true;
  }

  Serial.printf("[%lu] [BLE] Advertising as %s\n", millis(), advertisedName().c_str());
  return true;
}

void BluetoothTransferServer::end() {
  if (running_) {
    // A transfer still open here would otherwise leave its .part behind.
    bool receiving = false;
    {
      Lock lock(gStateMutex);
      receiving = gState.state == BleTransferState::Receiving;
    }
    if (receiving) {
      discardPartial("ERR_ABORTED", "Bluetooth Transfer was closed");
    }

    NimBLEDevice::deinit(true);
    gServer = nullptr;
    gStatusCharacteristic = nullptr;
    running_ = false;
    Serial.printf("[%lu] [BLE] Stack shut down\n", millis());
  }

  if (gFile) {
    gFile.close();
  }

  free(gRing);
  gRing = nullptr;

  if (gStateMutex) {
    {
      Lock lock(gStateMutex);
      gState = SharedState{};
    }
    vSemaphoreDelete(gStateMutex);
    gStateMutex = nullptr;
  }
  if (gRingMutex) {
    vSemaphoreDelete(gRingMutex);
    gRingMutex = nullptr;
  }
  gRingHead = gRingTail = gRingUsed = 0;
}

bool BluetoothTransferServer::isConnected() const {
  if (!running_) {
    return false;
  }
  Lock lock(gStateMutex);
  return gState.connected;
}

BleTransferStatus BluetoothTransferServer::status() const {
  BleTransferStatus snapshot;
  if (!gStateMutex) {
    return snapshot;
  }

  Lock lock(gStateMutex);
  snapshot.state = gState.state;
  snapshot.advertising = gState.advertising;
  snapshot.connected = gState.connected;
  snapshot.filename = gState.filename;
  snapshot.received = gState.written;
  snapshot.total = gState.total;
  snapshot.errorCode = gState.errorCode;
  snapshot.errorMessage = gState.errorMessage;
  snapshot.lastCompletedName = gState.lastCompletedName;
  snapshot.lastCompletedAt = gState.lastCompletedAt;
  snapshot.lastCompletedBytes = gState.lastCompletedBytes;
  snapshot.lastCompletedMs = gState.lastCompletedMs;
  return snapshot;
}

void BluetoothTransferServer::cancelTransfer() {
  if (!running_) {
    return;
  }
  requestAbort("ERR_ABORTED", "Cancelled on the reader");
}

void BluetoothTransferServer::poll() {
  if (!running_) {
    return;
  }

  // 1. Cleanup always wins, so an abort raised mid-write is honoured before more data lands.
  bool abortRequested = false;
  std::string abortCode;
  std::string abortMessage;
  {
    Lock lock(gStateMutex);
    abortRequested = gState.abortRequested;
    abortCode = gState.abortCode;
    abortMessage = gState.abortMessage;
  }
  if (abortRequested) {
    discardPartial(abortCode.c_str(), abortMessage.c_str());
    notifyError(abortCode.c_str(), abortMessage.c_str());
    return;
  }

  // 2. Open the .part file for a START that the control callback accepted.
  bool openRequested = false;
  std::string tempPath;
  std::string finalPath;
  std::string filename;
  uint32_t total = 0;
  {
    Lock lock(gStateMutex);
    openRequested = gState.openRequested;
    tempPath = gState.tempPath;
    finalPath = gState.finalPath;
    filename = gState.filename;
    total = gState.total;
  }

  if (openRequested) {
    {
      Lock lock(gStateMutex);
      gState.openRequested = false;
    }

    // Refuse rather than overwrite: a book already in the library is never destroyed by a resend.
    if (SdMan.exists(finalPath.c_str())) {
      discardPartial("ERR_EXISTS", "That book is already on the reader");
      notifyError("ERR_EXISTS", "That book is already on the reader");
      return;
    }

    if (SdMan.exists(tempPath.c_str())) {
      SdMan.remove(tempPath.c_str());
    }

    if (!SdMan.openFileForWrite("BLE", tempPath.c_str(), gFile)) {
      discardPartial("ERR_STORAGE", "Could not create the file on the SD card");
      notifyError("ERR_STORAGE", "Could not create the file on the SD card");
      return;
    }

    gRunningCrc = 0;
    gLastProgressAt = millis();
    gLastProgressBytes = 0;
    {
      Lock lock(gStateMutex);
      gState.state = BleTransferState::Receiving;
    }
    notifyReceiving(filename, total);
  }

  // 3. Drain whatever the radio has buffered onto the card.
  bool receiving = false;
  {
    Lock lock(gStateMutex);
    receiving = gState.state == BleTransferState::Receiving;
  }
  if (!receiving) {
    return;
  }

  static uint8_t chunk[SD_CHUNK];
  while (ringUsed() > 0) {
    const size_t count = ringPop(chunk, SD_CHUNK);
    if (count == 0) {
      break;
    }

    esp_task_wdt_reset();
    const size_t wrote = gFile.write(chunk, count);
    if (wrote != count) {
      discardPartial("ERR_STORAGE", "Writing to the SD card failed");
      notifyError("ERR_STORAGE", "Writing to the SD card failed");
      return;
    }
    gRunningCrc = crc32Update(gRunningCrc, chunk, count);

    {
      Lock lock(gStateMutex);
      gState.written += count;
    }
    yield();
  }

  // 4. Progress, rate-limited so status traffic does not compete with the data.
  uint32_t written = 0;
  bool finishRequested = false;
  uint32_t expectedCrc = 0;
  unsigned long startedAt = 0;
  {
    Lock lock(gStateMutex);
    written = gState.written;
    total = gState.total;
    finishRequested = gState.finishRequested;
    expectedCrc = gState.expectedCrc;
    filename = gState.filename;
    finalPath = gState.finalPath;
    tempPath = gState.tempPath;
    startedAt = gState.startedAt;
  }

  const unsigned long now = millis();
  if (now - gLastProgressAt >= PROGRESS_INTERVAL_MS && written - gLastProgressBytes >= 1) {
    if (written - gLastProgressBytes >= PROGRESS_INTERVAL_BYTES || now - gLastProgressAt >= 1000) {
      gLastProgressAt = now;
      gLastProgressBytes = written;
      notifyProgress(written, total);
    }
  }

  // 5. FINISH: only once every buffered byte is on the card.
  if (!finishRequested || ringUsed() > 0) {
    return;
  }

  gFile.flush();
  const uint32_t fileSize = gFile.size();
  gFile.close();

  if (written != total || fileSize != total) {
    discardPartial("ERR_TRUNCATED", "The file arrived incomplete");
    notifyError("ERR_TRUNCATED", "The file arrived incomplete");
    return;
  }

  if (gRunningCrc != expectedCrc) {
    Serial.printf("[%lu] [BLE] CRC mismatch: got %08x, expected %08x\n", millis(), gRunningCrc, expectedCrc);
    discardPartial("ERR_CRC", "The file did not survive the transfer intact");
    notifyError("ERR_CRC", "The file did not survive the transfer intact");
    return;
  }

  if (SdMan.exists(finalPath.c_str())) {
    discardPartial("ERR_EXISTS", "That book is already on the reader");
    notifyError("ERR_EXISTS", "That book is already on the reader");
    return;
  }

  if (!SdMan.rename(tempPath.c_str(), finalPath.c_str())) {
    discardPartial("ERR_STORAGE", "Could not save the book to the library");
    notifyError("ERR_STORAGE", "Could not save the book to the library");
    return;
  }

  const unsigned long elapsed = millis() - startedAt;
  {
    Lock lock(gStateMutex);
    gState.state = BleTransferState::Complete;
    gState.lastCompletedName = filename;
    gState.lastCompletedAt = millis();
    gState.lastCompletedBytes = total;
    gState.lastCompletedMs = elapsed;
    gState.filename.clear();
    gState.tempPath.clear();
    gState.finalPath.clear();
    gState.total = 0;
    gState.accepted = 0;
    gState.written = 0;
    gState.finishRequested = false;
    gState.errorCode.clear();
    gState.errorMessage.clear();
  }

  ringReset();
  gRunningCrc = 0;

  Serial.printf("[%lu] [BLE] Received %s (%u bytes in %lu ms)\n", millis(), filename.c_str(), total, elapsed);
  notifyComplete(filename, total);
}

#endif  // SIMULATOR
