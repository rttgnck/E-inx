// Simulator control channel: drive the simulator from a script or an agent
// without window focus, Accessibility or Screen Recording permission.
//
// Set EINX_SIM_CONTROL to a file or FIFO path ("-" for stdin). Each line is a
// command; '#' starts a comment.
//
//   key <back|confirm|left|right|up|down|power> [holdMs]   press and release
//   wait <ms>
//   shot <path.png>     save the panel as the user sees it (orientation applied)
//   failmalloc <count> [minBytes] [maxBytes] [skip]
//                       after letting <skip> matching calls through, the next
//                       <count> malloc() calls of minBytes..maxBytes (default
//                       32768..SIZE_MAX) return NULL, to reproduce the X3's
//                       fragmented heap; "failmalloc 0" disarms
//   quit
//
// EINX_SIM_FAIL_MALLOC="count[:min[:max[:skip]]]" arms failmalloc at startup.
//
// Every command and every refused allocation is logged as a [SIMCTL] line on
// stderr, alongside the firmware's serial log.
#ifdef SIMULATOR

#include <GfxRenderer.h>
#include <SDL.h>
#ifdef __APPLE__
#include <malloc/malloc.h>
#else
extern "C" void* __libc_malloc(size_t);
#endif
#include <miniz.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "HalDisplay.h"

// Added to the library's HalDisplay.cpp by scripts/simulator_lib_overrides.py.
uint32_t* einxSimPixelBuffer();
extern GfxRenderer renderer;

namespace {

std::atomic<int> failMallocRemaining{0};
std::atomic<size_t> failMallocMin{32768};
std::atomic<size_t> failMallocMax{SIZE_MAX};
std::atomic<int> failMallocSkip{0};

// Logging without malloc: this runs inside malloc().
void rawLog(const char* fmt, ...) {
  char buf[256];
  va_list args;
  va_start(args, fmt);
  const int n = vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  if (n > 0) {
    ssize_t ignored = write(STDERR_FILENO, buf, static_cast<size_t>(n) < sizeof(buf) ? n : sizeof(buf) - 1);
    (void)ignored;
  }
}

void armFailMalloc(int count, size_t minBytes, size_t maxBytes, int skip) {
  failMallocMin.store(minBytes);
  failMallocMax.store(maxBytes);
  failMallocSkip.store(skip);
  failMallocRemaining.store(count);
  rawLog("[SIMCTL] failmalloc armed: after %d, the next %d allocation(s) of %zu..%zu bytes fail\n", skip, count,
         minBytes, maxBytes);
}

SDL_Scancode scancodeFor(const std::string& name) {
  if (name == "back") return SDL_SCANCODE_ESCAPE;
  if (name == "confirm") return SDL_SCANCODE_RETURN;
  if (name == "left") return SDL_SCANCODE_LEFT;
  if (name == "right") return SDL_SCANCODE_RIGHT;
  if (name == "up") return SDL_SCANCODE_UP;
  if (name == "down") return SDL_SCANCODE_DOWN;
  if (name == "power") return SDL_SCANCODE_P;
  return SDL_SCANCODE_UNKNOWN;
}

void pushKey(SDL_Scancode code, bool down) {
  SDL_Event e{};
  e.type = down ? SDL_KEYDOWN : SDL_KEYUP;
  e.key.state = down ? SDL_PRESSED : SDL_RELEASED;
  e.key.repeat = 0;
  e.key.keysym.scancode = code;
  e.key.keysym.sym = SDL_GetKeyFromScancode(code);
  SDL_PushEvent(&e);
}

bool writeScreenshot(const std::string& path) {
  const uint32_t* src = einxSimPixelBuffer();
  const int w = HalDisplay::DISPLAY_WIDTH;
  const int h = HalDisplay::DISPLAY_HEIGHT;
  const auto orientation = renderer.getOrientation();
  const bool portrait = orientation == GfxRenderer::Portrait || orientation == GfxRenderer::PortraitInverted;
  const int outW = portrait ? h : w;
  const int outH = portrait ? w : h;
  std::vector<uint8_t> rgb(static_cast<size_t>(outW) * outH * 3);
  for (int sy = 0; sy < h; sy++) {
    for (int sx = 0; sx < w; sx++) {
      int dx = sx;
      int dy = sy;
      // Same rotations HalDisplay::presentIfNeeded() applies to the SDL texture.
      switch (orientation) {
        case GfxRenderer::Portrait:
          dx = h - 1 - sy;
          dy = sx;
          break;
        case GfxRenderer::PortraitInverted:
          dx = sy;
          dy = w - 1 - sx;
          break;
        case GfxRenderer::LandscapeClockwise:
          dx = w - 1 - sx;
          dy = h - 1 - sy;
          break;
        default:
          break;
      }
      const uint32_t argb = src[sy * w + sx];
      uint8_t* out = &rgb[(static_cast<size_t>(dy) * outW + dx) * 3];
      out[0] = (argb >> 16) & 0xFF;
      out[1] = (argb >> 8) & 0xFF;
      out[2] = argb & 0xFF;
    }
  }
  size_t pngSize = 0;
  void* png = tdefl_write_image_to_png_file_in_memory(rgb.data(), outW, outH, 3, &pngSize);
  if (!png) return false;
  std::ofstream file(path, std::ios::binary);
  file.write(static_cast<const char*>(png), static_cast<std::streamsize>(pngSize));
  mz_free(png);
  return file.good();
}

void runCommand(const std::string& line) {
  std::istringstream in(line);
  std::string cmd;
  in >> cmd;
  if (cmd.empty() || cmd[0] == '#') return;
  rawLog("[%u] [SIMCTL] %s\n", static_cast<unsigned>(SDL_GetTicks()), line.c_str());

  if (cmd == "key") {
    std::string name;
    int holdMs = 80;
    in >> name >> holdMs;
    const SDL_Scancode code = scancodeFor(name);
    if (code == SDL_SCANCODE_UNKNOWN) {
      rawLog("[SIMCTL] unknown key '%s'\n", name.c_str());
      return;
    }
    pushKey(code, true);
    std::this_thread::sleep_for(std::chrono::milliseconds(holdMs));
    pushKey(code, false);
    // Let the firmware see the release before the next command.
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
  } else if (cmd == "wait") {
    int ms = 0;
    in >> ms;
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
  } else if (cmd == "shot") {
    std::string path;
    in >> path;
    rawLog("[SIMCTL] shot %s: %s\n", path.c_str(), writeScreenshot(path) ? "ok" : "FAILED");
  } else if (cmd == "failmalloc") {
    long count = 0;
    unsigned long long minBytes = 32768;
    unsigned long long maxBytes = SIZE_MAX;
    int skip = 0;
    in >> count >> minBytes >> maxBytes >> skip;
    armFailMalloc(static_cast<int>(count), static_cast<size_t>(minBytes), static_cast<size_t>(maxBytes), skip);
  } else if (cmd == "quit") {
    SDL_Event e{};
    e.type = SDL_QUIT;
    SDL_PushEvent(&e);
  } else {
    rawLog("[SIMCTL] unknown command '%s'\n", cmd.c_str());
  }
}

void controlLoop(std::string path) {
  // Give setup() time to create the window before the first command.
  while (SDL_WasInit(SDL_INIT_VIDEO) == 0) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  std::string line;
  if (path == "-") {
    while (std::getline(std::cin, line)) runCommand(line);
    return;
  }
  // Reopen on EOF so a FIFO can take several writers in turn.
  while (true) {
    std::ifstream in(path);
    if (!in) {
      rawLog("[SIMCTL] cannot open %s\n", path.c_str());
      return;
    }
    while (std::getline(in, line)) runCommand(line);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

struct Startup {
  Startup() {
    if (const char* spec = std::getenv("EINX_SIM_FAIL_MALLOC")) {
      int count = 0;
      unsigned long long minBytes = 32768;
      unsigned long long maxBytes = SIZE_MAX;
      int skip = 0;
      if (sscanf(spec, "%d:%llu:%llu:%d", &count, &minBytes, &maxBytes, &skip) >= 1) {
        armFailMalloc(count, static_cast<size_t>(minBytes), static_cast<size_t>(maxBytes), skip);
      }
    }
    if (const char* path = std::getenv("EINX_SIM_CONTROL")) {
      std::thread(controlLoop, std::string(path)).detach();
    }
  }
} startup;

}  // namespace

namespace {
void* systemMalloc(size_t size) {
#ifdef __APPLE__
  return malloc_zone_malloc(malloc_default_zone(), size);
#else
  return __libc_malloc(size);
#endif
}
}  // namespace

// Replaces malloc() for code linked into the simulator executable (firmware and
// its libraries); system libraries keep their own. The allocation itself comes
// from the system allocator, so free() and realloc() work unchanged.
extern "C" void* malloc(size_t size) {
  if (failMallocRemaining.load(std::memory_order_relaxed) > 0 && size >= failMallocMin.load() &&
      size <= failMallocMax.load()) {
    if (failMallocSkip.load() > 0 && failMallocSkip.fetch_sub(1) > 0) {
      rawLog("[SIMCTL] failmalloc: let malloc(%zu) through\n", size);
      return systemMalloc(size);
    }
    const int left = failMallocRemaining.fetch_sub(1) - 1;
    if (left >= 0) {
      rawLog("[SIMCTL] failmalloc: refused malloc(%zu), %d left\n", size, left);
      return nullptr;
    }
  }
  return systemMalloc(size);
}

#endif  // SIMULATOR
