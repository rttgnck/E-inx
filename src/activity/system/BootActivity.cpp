/**
 * @file BootActivity.cpp
 * @brief Definitions for BootActivity.
 */

#include "BootActivity.h"

#include <GfxRenderer.h>
#include <SDCardManager.h>

#include "KOReaderCredentialStore.h"
#include "images/CorgiWhite.h"
#include "state/BookState.h"
#include "state/RecentBooks.h"
#include "state/Session.h"
#include "state/SystemSetting.h"

extern void onGoToRecent();
extern void onGoToReader(const std::string&);
extern HalDisplay display;
extern HalGPIO gpio;
extern MappedInputManager mappedInputManager;
extern GfxRenderer renderer;
extern Activity* currentActivity;
extern char rtcLastReadPath[256];
extern bool rtcLastSleepSleptFromReader;

BootActivity::BootActivity(GfxRenderer& renderer, MappedInputManager& inputManager)
    : Activity("BootActivity", renderer, inputManager) {}

/**
 * @brief Initializes the boot activity when it becomes active.
 */
void BootActivity::onEnter() {
  Activity::onEnter();

  APP_STATE.loadFromFile();
  RECENT_BOOKS.loadFromFile();
  BOOK_STATE.loadFromFile();

  if (SdMan.ready() && SdMan.exists(KOReaderCredentialStore::SYSTEM_SETTINGS_PATH)) {
    (void)KOREADER_STORE.loadFromFile();
  }

  bootComplete = true;
}

/**
 * @brief Main update loop for the boot activity.
 */
void BootActivity::loop() {
  if (bootComplete) {
    std::string resumePath = APP_STATE.lastRead;
    if (resumePath.empty() && rtcLastReadPath[0] != '\0') {
      resumePath = rtcLastReadPath;
    }
    const bool resumeFromSleep = !resumePath.empty() && rtcLastSleepSleptFromReader;
    if (resumePath.empty() || (!resumeFromSleep && SETTINGS.bootSetting == SystemSetting::HOME_PAGE)) {
      onGoToRecent();
    } else {
      APP_STATE.lastRead = "";
      APP_STATE.saveToFile();
      rtcLastReadPath[0] = '\0';
      onGoToReader(resumePath);
    }
    return;
  }
}
