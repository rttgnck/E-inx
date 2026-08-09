#pragma once

/**
 * @file CrossPlayCompat.h
 * @brief The seam between CrossPlay's app sources and E-inx's firmware.
 *
 * CrossPlay is a CrossPoint fork, and CrossPoint moved its activities onto an
 * async render task: `loop()` mutates state, `render(RenderLock&&)` draws,
 * `requestUpdate()` schedules a repaint, and an ActivityManager owns a stack of
 * activities. E-inx still runs the older Inx model, where `loop()` draws inline
 * and `switchTo<T>()` deletes and replaces the one live activity.
 *
 * Adopting the render task is a firmware-wide change and is deliberately NOT
 * part of this port. Everything that difference costs is paid here instead, in
 * one file, so that a future migration deletes this header rather than picking
 * CrossPoint assumptions back out of a dozen app sources.
 *
 * Three things live here:
 *   - RenderLock, an empty stand-in for CrossPoint's render mutex guard.
 *   - CrossPlayActivity, which turns requestUpdate() into a dirty flag that the
 *     E-inx loop consumes.
 *   - makeUniqueNoThrow, which CrossPlay app code uses everywhere and E-inx
 *     does not have.
 */

#include <cstddef>
#include <memory>
#include <new>
#include <string>
#include <type_traits>
#include <utility>

#include "activity/Activity.h"

/**
 * @brief Stand-in for CrossPoint's render-mutex RAII guard.
 *
 * On CrossPoint a render task and the main loop both touch the framebuffer, so
 * drawing takes a lock. E-inx draws only from the loop, so there is nothing to
 * lock against and this is empty. It exists so ported `render(RenderLock&&)`
 * signatures compile unchanged.
 */
class RenderLock {
 public:
  RenderLock() = default;
  RenderLock(const RenderLock&) = delete;
  RenderLock& operator=(const RenderLock&) = delete;
};

/**
 * @brief Base class for ported CrossPlay apps.
 *
 * Gives CrossPlay's split lifecycle (`loop()` decides, `render()` draws) on top
 * of E-inx's single `loop()`. A subclass overrides `loop()` and
 * `render(RenderLock&&)` exactly as it does upstream and calls requestUpdate()
 * to ask for a repaint; this class runs the repaint at the top of the next
 * frame.
 *
 * Painting after the decision rather than during it is what upstream's split
 * buys, and keeping it means the ported sources need no edits at their call
 * sites. It also means a run of state changes in one `loop()` costs one paint,
 * which on a panel that takes a third of a second to refresh is the difference
 * between responsive and not.
 */
class CrossPlayActivity : public Activity {
 public:
  CrossPlayActivity(std::string name, GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity(std::move(name), renderer, mappedInput) {}

  /** Ask for a repaint before the next `loop()`. Idempotent within a frame. */
  virtual void requestUpdate(bool /*immediate*/ = false) { updateRequired_ = true; }

  /** Paint. Overridden by every app; the RenderLock argument is vestigial here. */
  virtual void render(RenderLock&&) {}

  /**
   * @brief E-inx's entry point. Paints if asked, then runs the app's own loop.
   *
   * Apps override `loop()`, so this is deliberately not it: E-inx calls
   * `loop()`, which the app has replaced. The pump therefore has to be driven
   * from the app's loop, which every ported app does by calling
   * `pumpRender()` first. See SolitaireActivity::loop().
   */
  void pumpRender() {
    if (!updateRequired_) return;
    updateRequired_ = false;
    render(RenderLock{});
  }

  /** True when a repaint is pending, for apps that want to skip work before one. */
  bool updatePending() const { return updateRequired_; }

 private:
  bool updateRequired_ = false;
};

/**
 * @brief Nothrow std::make_unique. Returns nullptr instead of aborting on OOM.
 *
 * Exceptions are off on the ESP32, so plain `new` calls abort() when the heap
 * is exhausted. CrossPlay app code checks the pointer; this is the allocator it
 * expects. Ported verbatim from CrossPlay's lib/Memory so the app sources need
 * no edits.
 */
// Constrained with enable_if rather than a `requires` clause: CrossPlay builds
// against a much newer toolchain than the xtensa/riscv GCC 8 this platform pins,
// which has no concepts. SFINAE says the same thing and compiles here.
template <typename T, typename... Args, typename = std::enable_if_t<!std::is_array_v<T>>>
std::unique_ptr<T> makeUniqueNoThrow(Args&&... args) {
  return std::unique_ptr<T>(new (std::nothrow) T(std::forward<Args>(args)...));
}

template <typename T, typename = std::enable_if_t<std::is_array_v<T> && std::extent_v<T> == 0>>
std::unique_ptr<T> makeUniqueNoThrow(const size_t size) {
  return std::unique_ptr<T>(new (std::nothrow) std::remove_extent_t<T>[size]());
}
