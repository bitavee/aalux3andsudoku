#include "HttpDownloader.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <Logging.h>
#include <NetworkClient.h>
#include <NetworkClientSecure.h>
#include <StreamString.h>
#include <WiFi.h>
#include <base64.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <utility>

#ifndef SIMULATOR
#include "esp_wifi.h"
#endif

#include "CrossPointSettings.h"
#include "util/UrlUtils.h"

namespace {
constexpr uint16_t HTTP_TIMEOUT_MS = 15000;
constexpr size_t DOWNLOAD_CHUNK_BYTES = 2048;
constexpr unsigned long DOWNLOAD_STALL_TIMEOUT_MS = 300000;

class WifiSleepGuard {
 public:
  WifiSleepGuard() {
#ifndef SIMULATOR
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_max_tx_power(84);
#endif
  }
  ~WifiSleepGuard() {
#ifndef SIMULATOR
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
#endif
  }
  WifiSleepGuard(const WifiSleepGuard&) = delete;
  WifiSleepGuard& operator=(const WifiSleepGuard&) = delete;
};

class FileWriteStream final : public Stream {
 public:
  FileWriteStream(FsFile& file, size_t total, HttpDownloader::ProgressCallback progress)
      : file_(file), total_(total), progress_(std::move(progress)) {}

  size_t write(uint8_t byte) override { return write(&byte, 1); }

  size_t write(const uint8_t* buffer, size_t size) override {
    const size_t written = file_.write(buffer, size);
    if (written != size) {
      writeOk_ = false;
    }
    downloaded_ += written;
    if (progress_ && total_ > 0) {
      progress_(downloaded_, total_);
    }
    return written;
  }

  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override { file_.flush(); }

  size_t downloaded() const { return downloaded_; }
  bool ok() const { return writeOk_; }

 private:
  FsFile& file_;
  size_t total_;
  size_t downloaded_ = 0;
  bool writeOk_ = true;
  HttpDownloader::ProgressCallback progress_;
};
}  // namespace

bool HttpDownloader::fetchUrl(const std::string& url, Stream& outContent) {
  // Use NetworkClientSecure for HTTPS, regular NetworkClient for HTTP
  std::unique_ptr<NetworkClient> client;
  if (UrlUtils::isHttpsUrl(url)) {
    auto* secureClient = new NetworkClientSecure();
    secureClient->setInsecure();
    client.reset(secureClient);
  } else {
    client.reset(new NetworkClient());
  }
  HTTPClient http;

  LOG_DBG("HTTP", "Fetching: %s", url.c_str());

  http.begin(*client, url.c_str());
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.addHeader("User-Agent", "CrossPoint-ESP32-" AALU_VERSION);

  // Add Basic HTTP auth if credentials are configured
  if (strlen(SETTINGS.opdsUsername) > 0 && strlen(SETTINGS.opdsPassword) > 0) {
    std::string credentials = std::string(SETTINGS.opdsUsername) + ":" + SETTINGS.opdsPassword;
    String encoded = base64::encode(credentials.c_str());
    http.addHeader("Authorization", "Basic " + encoded);
  }

  const int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    LOG_ERR("HTTP", "Fetch failed: %d", httpCode);
    http.end();
    return false;
  }

  http.writeToStream(&outContent);

  http.end();

  LOG_DBG("HTTP", "Fetch success");
  return true;
}

bool HttpDownloader::fetchUrl(const std::string& url, std::string& outContent) {
  StreamString stream;
  if (!fetchUrl(url, stream)) {
    return false;
  }
  outContent = stream.c_str();
  return true;
}

HttpDownloader::DownloadError HttpDownloader::downloadToFile(const std::string& url, const std::string& destPath,
                                                             ProgressCallback progress) {
  WifiSleepGuard wifiSleepGuard;

  // Use NetworkClientSecure for HTTPS, regular NetworkClient for HTTP
  std::unique_ptr<NetworkClient> client;
  if (UrlUtils::isHttpsUrl(url)) {
    auto* secureClient = new NetworkClientSecure();
    secureClient->setInsecure();
    client.reset(secureClient);
  } else {
    client.reset(new NetworkClient());
  }
  HTTPClient http;

  LOG_DBG("HTTP", "Downloading: %s", url.c_str());
  LOG_DBG("HTTP", "Destination: %s", destPath.c_str());

  http.begin(*client, url.c_str());
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.addHeader("User-Agent", "CrossPoint-ESP32-" AALU_VERSION);

  // Add Basic HTTP auth if credentials are configured
  if (strlen(SETTINGS.opdsUsername) > 0 && strlen(SETTINGS.opdsPassword) > 0) {
    std::string credentials = std::string(SETTINGS.opdsUsername) + ":" + SETTINGS.opdsPassword;
    String encoded = base64::encode(credentials.c_str());
    http.addHeader("Authorization", "Basic " + encoded);
  }

  const int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    LOG_ERR("HTTP", "Download failed: %d", httpCode);
    http.end();
    return HTTP_ERROR;
  }

  const int64_t reportedLength = http.getSize();
  const size_t contentLength = reportedLength > 0 ? static_cast<size_t>(reportedLength) : 0;
  if (contentLength > 0) {
    LOG_DBG("HTTP", "Content-Length: %zu", contentLength);
  } else {
    LOG_DBG("HTTP", "Content-Length: unknown");
  }

  // Remove existing file if present
  if (Storage.exists(destPath.c_str())) {
    Storage.remove(destPath.c_str());
  }

  // Open file for writing
  FsFile file;
  if (!Storage.openFileForWrite("HTTP", destPath.c_str(), file)) {
    LOG_ERR("HTTP", "Failed to open file for writing");
    http.end();
    return FILE_ERROR;
  }

  size_t downloaded = 0;
  bool writeOk = true;

  if (contentLength > 0) {
    Stream* stream = http.getStreamPtr();
    if (stream == nullptr) {
      LOG_ERR("HTTP", "No response stream");
      file.close();
      http.end();
      Storage.remove(destPath.c_str());
      return HTTP_ERROR;
    }

    uint8_t buf[DOWNLOAD_CHUNK_BYTES];
    unsigned long lastDataMs = millis();

    while (downloaded < contentLength) {
      if (!http.connected() && stream->available() == 0) {
        break;
      }
      if (millis() - lastDataMs > DOWNLOAD_STALL_TIMEOUT_MS) {
        LOG_ERR("HTTP", "Stalled at %zu/%zu bytes", downloaded, contentLength);
        break;
      }

      const size_t avail = static_cast<size_t>(stream->available());
      if (avail == 0) {
        delay(10);
        continue;
      }

      size_t want = std::min(avail, DOWNLOAD_CHUNK_BYTES);
      want = std::min(want, contentLength - downloaded);

      const int bytesRead = stream->readBytes(buf, want);
      if (bytesRead <= 0) {
        delay(10);
        continue;
      }

      if (file.write(buf, static_cast<size_t>(bytesRead)) != static_cast<size_t>(bytesRead)) {
        writeOk = false;
        break;
      }
      downloaded += static_cast<size_t>(bytesRead);
      lastDataMs = millis();

      if (progress) {
        progress(downloaded, contentLength);
      }
    }
  } else {
    FileWriteStream fileStream(file, contentLength, progress);
    const int writeResult = http.writeToStream(&fileStream);
    downloaded = fileStream.downloaded();
    writeOk = fileStream.ok();
    if (writeResult < 0) {
      LOG_ERR("HTTP", "writeToStream error: %d", writeResult);
      file.close();
      http.end();
      Storage.remove(destPath.c_str());
      return HTTP_ERROR;
    }
  }

  file.close();
  http.end();

  LOG_DBG("HTTP", "Downloaded %zu bytes", downloaded);

  if (!writeOk) {
    LOG_ERR("HTTP", "Write failed during download");
    Storage.remove(destPath.c_str());
    return FILE_ERROR;
  }

  if (contentLength == 0 && downloaded == 0) {
    LOG_ERR("HTTP", "Download failed: no data received");
    Storage.remove(destPath.c_str());
    return HTTP_ERROR;
  }

  if (contentLength > 0 && downloaded != contentLength) {
    LOG_ERR("HTTP", "Size mismatch: got %zu, expected %zu", downloaded, contentLength);
    Storage.remove(destPath.c_str());
    return HTTP_ERROR;
  }

  return OK;
}
