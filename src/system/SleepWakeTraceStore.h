#pragma once

#ifndef SIMULATOR
#include <HalGPIO.h>

bool saveSleepWakeTraceCheckpoint(bool requireRetainedSleepCycle = false);
HalGPIO::SleepWakeTraceSnapshot loadSleepWakeTraceCheckpoint();
void clearSleepWakeTraceCheckpoint();
#endif
