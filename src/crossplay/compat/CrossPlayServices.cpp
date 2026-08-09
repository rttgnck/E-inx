#include "CrossPlayServices.h"

#include "network/HttpDownloader.h"

#include "CrossPlayCompat.h"

namespace crossplay {

bool fetchUrlStreaming(const std::string& url, const std::string& scratchPath,
                       const std::function<bool(const uint8_t*, size_t)>& onData) {
  if (!onData) return false;

  Storage.remove(scratchPath.c_str());
  if (HttpDownloader::downloadToFile(url, scratchPath) != HttpDownloader::OK) {
    Storage.remove(scratchPath.c_str());
    return false;
  }

  FsFile file;
  if (!Storage.openFileForRead("CPNET", scratchPath.c_str(), file)) {
    Storage.remove(scratchPath.c_str());
    return false;
  }

  // 1KB at a time. Small enough that the buffer is never the thing that runs the
  // heap out, large enough that a multi-megabyte archive is not a million calls.
  constexpr size_t kChunk = 1024;
  auto buffer = makeUniqueNoThrow<uint8_t[]>(kChunk);
  if (!buffer) {
    file.close();
    Storage.remove(scratchPath.c_str());
    LOG_ERR("CPNET", "OOM streaming %s", url.c_str());
    return false;
  }

  bool ok = true;
  while (true) {
    const int read = file.read(buffer.get(), kChunk);
    if (read <= 0) break;
    if (!onData(buffer.get(), static_cast<size_t>(read))) {
      ok = false;  // The parser asked to stop; that is an abort, not a success.
      break;
    }
  }

  file.close();
  Storage.remove(scratchPath.c_str());
  return ok;
}

}  // namespace crossplay
