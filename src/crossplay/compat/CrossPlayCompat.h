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
#include <functional>
#include <memory>
#include <new>
#include <string>
#include <type_traits>
#include <utility>

#include "activity/Activity.h"
#include "CrossPlayGfx.h"

/**
 * @brief What a sub-activity hands back when it finishes.
 *
 * Upstream's is richer (it carries a payload union); the only field any ported
 * app reads is this one, so it is the only one here. Adding the rest when
 * something needs it is a smaller change than carrying it unused.
 */
struct ActivityResult {
  bool isCancelled = false;
};

using ActivityResultHandler = std::function<void(const ActivityResult&)>;

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
  // Upstream's takes the activity so the guard can name what it is protecting.
  // Nothing here is, but the call sites say `RenderLock(*this)` and keeping them
  // unedited is the point of this file.
  explicit RenderLock(Activity&) {}
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
  CrossPlayActivity(std::string name, GfxRenderer& gfxRenderer, MappedInputManager& mappedInput)
      : Activity(std::move(name), gfxRenderer, mappedInput), renderer(gfxRenderer) {}

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

  /**
   * @brief Runs `activity` on top of this one, calling `handler` when it ends.
   *
   * CrossPoint has an ActivityManager holding a stack, and an app pushes onto it.
   * E-inx has one live activity, so "on top of" is implemented the way E-inx's
   * own screens do it: the parent owns the child, forwards `loop()` to it while
   * it lives, and destroys it when it finishes. Nesting is one deep, which is
   * all any ported app uses (a game opening the WiFi picker).
   */
  void startActivityForResult(std::unique_ptr<Activity>&& activity, ActivityResultHandler handler) {
    subActivity_ = std::move(activity);
    resultHandler_ = std::move(handler);
    if (subActivity_) subActivity_->onEnter();
  }

  /** True while a sub-activity owns the screen. */
  bool hasSubActivity() const { return subActivity_ != nullptr; }

  /**
   * @brief Ends the running sub-activity and delivers its result.
   *
   * Called by the adapter that wraps the child, not by the child itself: E-inx's
   * own activities report through a completion callback rather than by popping
   * themselves, which is the difference this whole file exists to absorb.
   */
  void finishSubActivity(const ActivityResult& result) {
    if (!subActivity_) return;
    subActivity_->onExit();
    subActivity_.reset();
    const ActivityResultHandler handler = resultHandler_;
    resultHandler_ = nullptr;
    if (handler) handler(result);
    requestUpdate();
  }

  /**
   * @brief Forwards the pass to a running sub-activity.
   *
   * Returns true when it consumed the pass, so a caller's `loop()` is
   * `if (pumpSubActivity()) return;` at the top.
   */
  bool pumpSubActivity() {
    if (!subActivity_) return false;
    subActivity_->loop();
    return true;
  }

 protected:
  /**
   * @brief The renderer, in CrossPoint's flat spelling.
   *
   * Deliberately shadows `Activity::renderer`, which is the same object seen
   * through E-inx's sub-renderer API. Ported apps draw their play surfaces with
   * `renderer.fillRect(...)` and friends; this is what makes those sources
   * compile unedited. It converts implicitly to `GfxRenderer&`, so every call
   * that wants the real thing — `toybox::makeTarget(renderer)`,
   * `toybox::blit1bpp(renderer, ...)` — still gets it.
   *
   * The shadowing is the one piece of cleverness in this layer, and it is
   * confined to src/crossplay/: nothing outside sees two renderers.
   */
  CrossPlayGfx renderer;

 private:
  bool updateRequired_ = false;
  std::unique_ptr<Activity> subActivity_;
  ActivityResultHandler resultHandler_;
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
