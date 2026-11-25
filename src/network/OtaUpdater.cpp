/**
 * @file OtaUpdater.cpp
 * @brief Definitions for OtaUpdater.
 */

#include "OtaUpdater.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <SDCardManager.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <new>

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_task_wdt.h"
#include "esp_wifi.h"

#include "state/SystemSetting.h"

namespace {
/**
 * The default release channel, used when the setting is blank. The setting
 * itself lives on the SD card and is editable in the web manager, so a fork or
 * a private mirror is not stuck with this one.
 */
constexpr char defaultReleaseUrl[] = "https://api.github.com/repos/rttgnck/E-inx/releases/latest";

/** The configured release endpoint, falling back to the default when unset. */
const char* releaseUrl() {
  return SETTINGS.otaReleaseUrl[0] != '\0' ? SETTINGS.otaReleaseUrl : defaultReleaseUrl;
}

// GitHub's release JSON runs 9 to 11 KB, most of it author and uploader objects the filter throws away. It is
// parsed straight off the connection through a small window, so the check never needs one contiguous block for
// the whole reply. With TLS open the X3 often has no 9.5 KB block to spare, and the old buffered read then lost
// the first chunk (ESP-IDF ignores an event handler's error) and parsed a reply with its start missing.
constexpr int kReleaseReadWindowBytes = 512;
constexpr int kMaxReleaseRedirects = 3;

constexpr int kGithubCheckTaskStack = 16384;
constexpr int kGithubCheckTaskPrio = 3;
constexpr size_t kMinimumFirmwareSize = 64 * 1024;
constexpr uint8_t kEspImageMagic = 0xE9;
constexpr uint16_t kEsp32C3ChipId = 5;

struct ParsedVersion {
  int major = 0;
  int minor = 0;
  int patch = 0;
  int revision = 0;
  bool beta = false;
};

bool containsCaseInsensitive(const char* text, const char* needle) {
  if (text == nullptr || needle == nullptr || *needle == '\0') return false;
  for (const char* start = text; *start; ++start) {
    const char* a = start;
    const char* b = needle;
    while (*a && *b && std::tolower(static_cast<unsigned char>(*a)) ==
                           std::tolower(static_cast<unsigned char>(*b))) {
      ++a;
      ++b;
    }
    if (*b == '\0') return true;
  }
  return false;
}

bool parseVersion(const char* text, ParsedVersion& out) {
  if (text == nullptr) {
    return false;
  }
  while (*text && std::isspace(static_cast<unsigned char>(*text))) {
    ++text;
  }
  if (*text == 'v' || *text == 'V') {
    ++text;
  }

  char* end = nullptr;
  const long major = strtol(text, &end, 10);
  if (end == text || *end != '.') {
    return false;
  }
  text = end + 1;
  const long minor = strtol(text, &end, 10);
  if (end == text || *end != '.') {
    return false;
  }
  text = end + 1;
  const long patch = strtol(text, &end, 10);
  if (end == text || major < 0 || minor < 0 || patch < 0) {
    return false;
  }

  out.major = static_cast<int>(major);
  out.minor = static_cast<int>(minor);
  out.patch = static_cast<int>(patch);
  out.revision = 0;
  out.beta = containsCaseInsensitive(end, "beta");

  // E-inx releases use suffixes such as -w2 and -3_update. Treat the
  // first numeric suffix component as an ordered release revision.
  for (const char* p = end; *p; ++p) {
    if (std::isdigit(static_cast<unsigned char>(*p))) {
      char* revisionEnd = nullptr;
      const long revision = strtol(p, &revisionEnd, 10);
      if (revisionEnd != p && revision >= 0) {
        out.revision = static_cast<int>(revision);
      }
      break;
    }
  }
  return true;
}

bool hasBinExtension(const std::string& name) {
  if (name.size() < 4) return false;
  const size_t dot = name.size() - 4;
  return name[dot] == '.' && std::tolower(static_cast<unsigned char>(name[dot + 1])) == 'b' &&
         std::tolower(static_cast<unsigned char>(name[dot + 2])) == 'i' &&
         std::tolower(static_cast<unsigned char>(name[dot + 3])) == 'n';
}

bool isFirmwareAssetName(const std::string& name) {
  if (!hasBinExtension(name)) return false;
  std::string lower = name;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return lower == "firmware.bin" || lower.find("firmware") != std::string::npos || lower.rfind("e-inx", 0) == 0;
}

extern "C" {
extern esp_err_t esp_crt_bundle_attach(void* conf);
}

/** Set the User-Agent header on the OTA HTTPS client during esp_https_ota init. */
esp_err_t http_client_set_header_cb(esp_http_client_handle_t http_client) {
  return esp_http_client_set_header(http_client, "User-Agent", "Inx-ESP32-" INX_VERSION);
}

/** Feeds an open esp_http_client response to ArduinoJson one small window at a time. */
class HttpBodyReader {
 public:
  explicit HttpBodyReader(esp_http_client_handle_t client) : client_(client) {}

  int read() {
    if (pos_ >= len_ && !fill()) return -1;
    return static_cast<unsigned char>(window_[pos_++]);
  }

  size_t readBytes(char* out, const size_t length) {
    size_t done = 0;
    while (done < length) {
      if (pos_ >= len_ && !fill()) break;
      const size_t n = std::min(length - done, static_cast<size_t>(len_ - pos_));
      memcpy(out + done, window_ + pos_, n);
      pos_ += static_cast<int>(n);
      done += n;
    }
    return done;
  }

  bool failed() const { return failed_; }
  size_t bytesRead() const { return total_; }

 private:
  bool fill() {
    if (failed_ || ended_) return false;
    const int n = esp_http_client_read(client_, window_, sizeof(window_));
    if (n < 0) {
      failed_ = true;
      return false;
    }
    if (n == 0) {
      ended_ = true;
      return false;
    }
    len_ = n;
    pos_ = 0;
    total_ += static_cast<size_t>(n);
    esp_task_wdt_reset();
    return true;
  }

  esp_http_client_handle_t client_;
  char window_[kReleaseReadWindowBytes];
  int len_ = 0;
  int pos_ = 0;
  size_t total_ = 0;
  bool failed_ = false;
  bool ended_ = false;
};

bool isRedirect(const int status) {
  return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}
}  // namespace

struct OtaGithubCheckCtx {
  OtaUpdater* updater;
  OtaUpdater::OtaUpdaterError result;
  SemaphoreHandle_t done;
};

/** FreeRTOS task entry point that runs the GitHub update check and signals completion via a semaphore. */
void otaGithubCheckTask(void* param) {
  auto* ctx = static_cast<OtaGithubCheckCtx*>(param);
  ctx->result = ctx->updater->checkForUpdateWorker();
  xSemaphoreGive(ctx->done);
  vTaskDelete(nullptr);
}

/** Spawn a background task to check GitHub for updates and block until it completes. */
OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdate() {
  SemaphoreHandle_t done = xSemaphoreCreateBinary();
  if (done == nullptr) {
    return OOM_ERROR;
  }

  auto* ctx = new (std::nothrow) OtaGithubCheckCtx{this, INTERNAL_UPDATE_ERROR, done};
  if (ctx == nullptr) {
    vSemaphoreDelete(done);
    return OOM_ERROR;
  }

  if (xTaskCreate(otaGithubCheckTask, "otaGhChk", kGithubCheckTaskStack, ctx, kGithubCheckTaskPrio, nullptr) !=
      pdPASS) {
    delete ctx;
    vSemaphoreDelete(done);
    Serial.printf("[%lu] [OTA] Failed to spawn GitHub check task (stack %d)\n", millis(), kGithubCheckTaskStack);
    return OOM_ERROR;
  }

  while (xSemaphoreTake(done, pdMS_TO_TICKS(500)) != pdTRUE) {
    esp_task_wdt_reset();
  }

  const OtaUpdaterError out = ctx->result;
  delete ctx;
  vSemaphoreDelete(done);
  return out;
}

/** Fetch and parse the latest GitHub release JSON to find a firmware.bin asset. */
OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdateWorker() {
  updateAvailable = false;
  latestVersion.clear();
  releaseNotes.clear();
  failureDetail.clear();
  otaUrl.clear();
  otaSize = 0;
  processedSize = 0;
  totalSize = 0;

  esp_http_client_config_t client_config = {};
  client_config.url = releaseUrl();
  client_config.buffer_size = 2048;
  client_config.buffer_size_tx = 1024;
  client_config.timeout_ms = 25000;
  client_config.crt_bundle_attach = esp_crt_bundle_attach;
  client_config.keep_alive_enable = false;

  esp_http_client_handle_t client = esp_http_client_init(&client_config);
  if (!client) {
    Serial.printf("[%lu] [OTA] HTTP Client Handle Failed\n", millis());
    return INTERNAL_UPDATE_ERROR;
  }
  struct ClientCleaner {
    esp_http_client_handle_t client;
    ~ClientCleaner() { esp_http_client_cleanup(client); }
  } clientCleaner = {client};

  esp_err_t esp_err = esp_http_client_set_header(client, "User-Agent", "Inx-ESP32-" INX_VERSION);
  if (esp_err != ESP_OK) {
    Serial.printf("[%lu] [OTA] esp_http_client_set_header Failed : %s\n", millis(), esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  vTaskDelay(pdMS_TO_TICKS(200));
  esp_task_wdt_reset();

  int statusCode = 0;
  for (int hop = 0;; ++hop) {
    esp_err = esp_http_client_open(client, 0);
    if (esp_err != ESP_OK) {
      Serial.printf("[%lu] [OTA] esp_http_client_open Failed : %s\n", millis(), esp_err_to_name(esp_err));
      return HTTP_ERROR;
    }
    esp_http_client_fetch_headers(client);
    esp_task_wdt_reset();
    statusCode = esp_http_client_get_status_code(client);
    if (!isRedirect(statusCode) || hop >= kMaxReleaseRedirects) break;
    esp_http_client_flush_response(client, nullptr);
    if (esp_http_client_set_redirection(client) != ESP_OK) break;
    esp_http_client_close(client);
  }

  if (statusCode != 200) {
    Serial.printf("[%lu] [OTA] GitHub returned HTTP %d\n", millis(), statusCode);
    if (statusCode > 0) failureDetail = "GitHub answered HTTP " + std::to_string(statusCode);
    return HTTP_ERROR;
  }

  JsonDocument filter;
  filter["tag_name"] = true;
  filter["body"] = true;
  filter["assets"][0]["name"] = true;
  filter["assets"][0]["browser_download_url"] = true;
  filter["assets"][0]["size"] = true;

  JsonDocument doc;
  HttpBodyReader body(client);
  const DeserializationError error = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  if (body.failed()) {
    Serial.printf("[%lu] [OTA] Connection dropped after %u bytes\n", millis(), static_cast<unsigned>(body.bytesRead()));
    failureDetail = "the connection dropped after " + std::to_string(body.bytesRead()) + " bytes";
    return HTTP_ERROR;
  }
  if (error) {
    Serial.printf("[%lu] [OTA] JSON parse failed: %s (%u bytes, free heap %u, largest block %u)\n", millis(),
                  error.c_str(), static_cast<unsigned>(body.bytesRead()), static_cast<unsigned>(ESP.getFreeHeap()),
                  static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
    failureDetail = std::string(error.c_str()) + " after " + std::to_string(body.bytesRead()) + " bytes";
    return error == DeserializationError::NoMemory ? OOM_ERROR : JSON_PARSE_ERROR;
  }

  if (!doc["tag_name"].is<std::string>()) {
    Serial.printf("[%lu] [OTA] No tag_name found\n", millis());
    failureDetail = "the release had no tag_name";
    return JSON_PARSE_ERROR;
  }

  if (!doc["assets"].is<JsonArray>()) {
    Serial.printf("[%lu] [OTA] No assets found\n", millis());
    failureDetail = "the release listed no assets";
    return JSON_PARSE_ERROR;
  }

  latestVersion = doc["tag_name"].as<std::string>();
  if (doc["body"].is<std::string>()) {
    releaseNotes = doc["body"].as<std::string>();
  }

  int selectedAsset = -1;
  for (int i = 0; i < doc["assets"].size(); i++) {
    const std::string name = doc["assets"][i]["name"].as<std::string>();
    if (!isFirmwareAssetName(name)) continue;
    if (selectedAsset < 0 || name == "firmware.bin") selectedAsset = i;
    if (name == "firmware.bin") break;
  }

  if (selectedAsset >= 0) {
    otaUrl = doc["assets"][selectedAsset]["browser_download_url"].as<std::string>();
    otaSize = doc["assets"][selectedAsset]["size"].as<size_t>();
    if (!otaUrl.empty() && otaSize > 0) {
      totalSize = otaSize;
      updateAvailable = true;
    }
  }

  if (!updateAvailable) {
    Serial.printf("[%lu] [OTA] No firmware .bin asset found\n", millis());
    return NO_UPDATE;
  }

  Serial.printf("[%lu] [OTA] Found update: %s\n", millis(), latestVersion.c_str());
  return OK;
}

/** Check whether the latest known release version is newer than the running firmware. */
bool OtaUpdater::isUpdateNewer() const {
  if (!updateAvailable || latestVersion.empty() || latestVersion == INX_VERSION) {
    return false;
  }

  ParsedVersion current;
  ParsedVersion latest;
  if (!parseVersion(INX_VERSION, current) || !parseVersion(latestVersion.c_str(), latest)) {
    Serial.printf("[%lu] [OTA] Could not compare versions current=%s latest=%s\n", millis(), INX_VERSION,
                  latestVersion.c_str());
    return false;
  }

  if (latest.major != current.major) return latest.major > current.major;
  if (latest.minor != current.minor) return latest.minor > current.minor;
  if (latest.patch != current.patch) return latest.patch > current.patch;

  if (latest.revision != current.revision) return latest.revision > current.revision;
  if (latest.beta != current.beta) return current.beta && !latest.beta;

  // GitHub's /releases/latest endpoint is authoritative for two distinct
  // same-base release labels that do not carry an ordered numeric revision.
  return latestVersion != INX_VERSION;
}

/** Return the version string of the latest release found. */
const std::string& OtaUpdater::getLatestVersion() const { return latestVersion; }

/** Return the changelog body supplied with the latest GitHub release. */
const std::string& OtaUpdater::getReleaseNotes() const { return releaseNotes; }

/** Return why the last check failed, in the terms it failed in. */
const std::string& OtaUpdater::getFailureDetail() const { return failureDetail; }

/** Download and install the latest update over HTTPS. */
OtaUpdater::OtaUpdaterError OtaUpdater::installUpdate(const bool allowSameVersion) {
  // A reinstall still needs a release to have been found and an asset picked;
  // it only waives the "must be newer" test.
  if (allowSameVersion ? (otaUrl.empty() || otaSize == 0) : !isUpdateNewer()) {
    return UPDATE_OLDER_ERROR;
  }

  esp_https_ota_handle_t ota_handle = NULL;
  esp_err_t esp_err;

  processedSize.store(0, std::memory_order_relaxed);
  totalSize.store(otaSize, std::memory_order_relaxed);
  render = false;

  esp_http_client_config_t client_config = {};
  client_config.url = otaUrl.c_str();
  client_config.timeout_ms = 15000;
  client_config.buffer_size = 8192;
  client_config.buffer_size_tx = 8192;
  client_config.crt_bundle_attach = esp_crt_bundle_attach;
  client_config.keep_alive_enable = true;

  esp_https_ota_config_t ota_config = {};
  ota_config.http_config = &client_config;
  ota_config.http_client_init_cb = http_client_set_header_cb;

  esp_wifi_set_ps(WIFI_PS_NONE);

  esp_err = esp_https_ota_begin(&ota_config, &ota_handle);
  if (esp_err != ESP_OK) {
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    Serial.printf("[%lu] [OTA] HTTP OTA Begin Failed: %s\n", millis(), esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  do {
    esp_err = esp_https_ota_perform(ota_handle);
    processedSize = esp_https_ota_get_image_len_read(ota_handle);

    render = true;
    esp_task_wdt_reset();
    vTaskDelay(10 / portTICK_PERIOD_MS);
  } while (esp_err == ESP_ERR_HTTPS_OTA_IN_PROGRESS);

  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

  if (esp_err != ESP_OK) {
    Serial.printf("[%lu] [OTA] esp_https_ota_perform Failed: %s\n", millis(), esp_err_to_name(esp_err));
    esp_https_ota_finish(ota_handle);
    render = false;
    return HTTP_ERROR;
  }

  if (!esp_https_ota_is_complete_data_received(ota_handle)) {
    Serial.printf("[%lu] [OTA] esp_https_ota_is_complete_data_received Failed: %s\n", millis(),
                  esp_err_to_name(esp_err));
    esp_https_ota_finish(ota_handle);
    render = false;
    return INTERNAL_UPDATE_ERROR;
  }

  esp_err = esp_https_ota_finish(ota_handle);
  if (esp_err != ESP_OK) {
    Serial.printf("[%lu] [OTA] esp_https_ota_finish Failed: %s\n", millis(), esp_err_to_name(esp_err));
    render = false;
    return INTERNAL_UPDATE_ERROR;
  }

  processedSize.store(totalSize.load(std::memory_order_relaxed), std::memory_order_relaxed);
  render = true;
  Serial.printf("[%lu] [OTA] Update completed\n", millis());
  return OK;
}

/** Install a firmware image read from the SD card. */
OtaUpdater::OtaUpdaterError OtaUpdater::installUpdateFromSd(const char* firmwarePath) {
  if (firmwarePath == nullptr || firmwarePath[0] == '\0') {
    return INTERNAL_UPDATE_ERROR;
  }

  FsFile file;
  if (!SdMan.openFileForRead("OTA", firmwarePath, file)) {
    Serial.printf("[%lu] [OTA] SD firmware not found: %s\n", millis(), firmwarePath);
    return HTTP_ERROR;
  }

  const size_t firmwareSize = file.size();
  if (firmwareSize < kMinimumFirmwareSize) {
    Serial.printf("[%lu] [OTA] SD firmware is too small: %s (%u bytes)\n", millis(), firmwarePath,
                  static_cast<unsigned>(firmwareSize));
    file.close();
    return INTERNAL_UPDATE_ERROR;
  }

  uint8_t imageHeader[14] = {};
  if (file.read(imageHeader, sizeof(imageHeader)) != static_cast<int>(sizeof(imageHeader)) ||
      imageHeader[0] != kEspImageMagic) {
    Serial.printf("[%lu] [OTA] SD file is not an ESP application image: %s\n", millis(), firmwarePath);
    file.close();
    return INTERNAL_UPDATE_ERROR;
  }
  const uint16_t chipId = static_cast<uint16_t>(imageHeader[12]) | (static_cast<uint16_t>(imageHeader[13]) << 8);
  if (chipId != kEsp32C3ChipId || !file.seek(0)) {
    Serial.printf("[%lu] [OTA] SD firmware is not for ESP32-C3: %s\n", millis(), firmwarePath);
    file.close();
    return INTERNAL_UPDATE_ERROR;
  }

  const esp_partition_t* updatePartition = esp_ota_get_next_update_partition(nullptr);
  if (updatePartition == nullptr) {
    Serial.printf("[%lu] [OTA] No OTA update partition available\n", millis());
    file.close();
    return INTERNAL_UPDATE_ERROR;
  }
  if (firmwareSize > updatePartition->size) {
    Serial.printf("[%lu] [OTA] SD firmware exceeds %s (%u > %u bytes)\n", millis(), updatePartition->label,
                  static_cast<unsigned>(firmwareSize), static_cast<unsigned>(updatePartition->size));
    file.close();
    return INTERNAL_UPDATE_ERROR;
  }

  Serial.printf("[%lu] [OTA] Installing SD firmware %s (%u bytes) to %s\n", millis(), firmwarePath,
                static_cast<unsigned>(firmwareSize), updatePartition->label);

  esp_ota_handle_t otaHandle = 0;
  esp_err_t err = esp_ota_begin(updatePartition, firmwareSize, &otaHandle);
  if (err != ESP_OK) {
    Serial.printf("[%lu] [OTA] esp_ota_begin failed: %s\n", millis(), esp_err_to_name(err));
    file.close();
    return INTERNAL_UPDATE_ERROR;
  }

  uint8_t buffer[1024];
  processedSize = 0;
  totalSize = firmwareSize;
  render = false;

  while (processedSize.load(std::memory_order_relaxed) < firmwareSize) {
    const size_t currentSize = processedSize.load(std::memory_order_relaxed);
    const size_t toRead = std::min(sizeof(buffer), firmwareSize - currentSize);
    const int readBytes = file.read(buffer, toRead);
    if (readBytes <= 0) {
      Serial.printf("[%lu] [OTA] SD read failed at %u / %u\n", millis(), static_cast<unsigned>(currentSize),
                    static_cast<unsigned>(firmwareSize));
      esp_ota_abort(otaHandle);
      file.close();
      return HTTP_ERROR;
    }

    err = esp_ota_write(otaHandle, buffer, static_cast<size_t>(readBytes));
    if (err != ESP_OK) {
      Serial.printf("[%lu] [OTA] esp_ota_write failed: %s\n", millis(), esp_err_to_name(err));
      esp_ota_abort(otaHandle);
      file.close();
      return INTERNAL_UPDATE_ERROR;
    }

    processedSize.fetch_add(static_cast<size_t>(readBytes), std::memory_order_relaxed);
    render = true;
    esp_task_wdt_reset();
    vTaskDelay(1);
  }

  file.close();

  err = esp_ota_end(otaHandle);
  if (err != ESP_OK) {
    Serial.printf("[%lu] [OTA] esp_ota_end failed: %s\n", millis(), esp_err_to_name(err));
    return INTERNAL_UPDATE_ERROR;
  }

  err = esp_ota_set_boot_partition(updatePartition);
  if (err != ESP_OK) {
    Serial.printf("[%lu] [OTA] esp_ota_set_boot_partition failed: %s\n", millis(), esp_err_to_name(err));
    return INTERNAL_UPDATE_ERROR;
  }

  Serial.printf("[%lu] [OTA] SD firmware install completed\n", millis());
  return OK;
}
