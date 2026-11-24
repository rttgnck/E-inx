"""
Pre-build script for the simulator envs: adapt the crosspoint-simulator library
(pinned in platformio.ini) to E-inx without editing it by hand.

The library's include directory comes before -Isrc/simulator on the compiler
command line, and its .cpp files include their own headers by quoted path, so
a same-named header in src/simulator/ never wins. Instead this script edits the
downloaded copy in .pio/libdeps/<env>/simulator/src/ on every build:

  OVERRIDES   the library header becomes a one-line forward to a project file,
              so library and firmware compile against one definition.
  EDITS       text inserted after an anchor line, or one line replaced. Each
              edit sits between BEGIN/END marker lines and is undone before
              being applied again, so changing an edit here takes effect on the
              next build instead of piling up.

A missing anchor prints a WARNING: the library changed and the edit needs
updating. Deleting .pio/libdeps/<env>/simulator restores a clean copy.
"""

Import("env")  # noqa: F821
import os

# Library header (relative to the library's src/) -> project file.
OVERRIDES = {
    "Preferences.h": "src/simulator/Preferences.h",
    "esp_system.h": "src/simulator/esp_system.h",
}

# (library file, anchor line, text to insert after it, or None to keep the
# anchor) and REPLACE edits that swap the anchor line out. "{project:PATH}" in
# the text becomes PATH relative to the edited file.
INSERT = "insert"
REPLACE = "replace"
EDITS = [
    (
        "HalGPIO.h",
        REPLACE,
        "  enum class WakeupReason { PowerButton, AfterFlash, AfterUSBPower, Other };",
        "  // E-inx: SleepTimer and Button appended, so the library's values keep their numbers.\n"
        "  enum class WakeupReason { PowerButton, AfterFlash, AfterUSBPower, Other, SleepTimer, Button };\n",
    ),
    (
        "HalGPIO.h",
        INSERT,
        "  WakeupReason getWakeupReason() const;",
        '#include "{project:src/simulator/HalGPIOEInxMembers.inc}"\n',
    ),
    (
        "WString.h",
        INSERT,
        "  String(uint16_t num) : s(std::to_string(num)) {}",
        "  // E-inx: Arduino's String(float/double, decimals), used by LocalServer.\n"
        "  String(double num, unsigned char decimals) {\n"
        "    char buf[48];\n"
        '    snprintf(buf, sizeof(buf), "%.*f", static_cast<int>(decimals), num);\n'
        "    s = buf;\n"
        "  }\n"
        "  String(float num, unsigned char decimals) : String(static_cast<double>(num), decimals) {}\n",
    ),
    (
        "EInkDisplay.h",
        INSERT,
        "  EInkDisplay(int, int, int, int, int, int) {}",
        "  // E-inx: callbacks the device runs during panel busy-waits. The simulator\n"
        "  // never busy-waits, but JpegRender invokes the wait callback directly.\n"
        "  using WaitCallback = void (*)();\n"
        "  static void setWaitCallback(WaitCallback cb) { waitCallbackSlot() = cb; }\n"
        "  static void setFrameWaitCallback(WaitCallback) {}\n"
        "  static void invokeWaitCallback() {\n"
        "    if (waitCallbackSlot()) waitCallbackSlot()();\n"
        "  }\n"
        "  static WaitCallback& waitCallbackSlot() {\n"
        "    static WaitCallback cb = nullptr;\n"
        "    return cb;\n"
        "  }\n",
    ),
    (
        "HalDisplay.h",
        INSERT,
        "  void presentIfNeeded();",
        "  // E-inx display API. The simulator shows the framebuffer the same way\n"
        "  // whatever waveform the device would have used.\n"
        "  void displayBwReinforced(RefreshMode fallback = FAST_REFRESH, bool turnOffScreen = false) {\n"
        "    displayBuffer(fallback, turnOffScreen);\n"
        "  }\n"
        "  void displayHalfScrub(RefreshMode fallback = HALF_REFRESH, bool turnOffScreen = false) {\n"
        "    displayBuffer(fallback, turnOffScreen);\n"
        "  }\n"
        "  void setX3PageWaveform(uint8_t) {}\n"
        "  // E-inx's displayGrayBuffer(quality, trackForRevert, turnOffScreen). One-argument\n"
        "  // calls resolve to the library overload, which ignores its arguments too.\n"
        "  void displayGrayBuffer(bool, bool, bool turnOffScreen = false) {\n"
        "    displayGrayBuffer(turnOffScreen, nullptr, false);\n"
        "  }\n"
        "  void displayGrayBufferFastQuality(bool turnOffScreen = false) {\n"
        "    displayGrayBuffer(turnOffScreen, nullptr, false);\n"
        "  }\n"
        "  void prepareQualityGrayscale() {}\n"
        "#if defined(SIMULATOR_DEVICE_X3)\n"
        "  bool deviceIsX3() const { return true; }\n"
        "#else\n"
        "  bool deviceIsX3() const { return false; }\n"
        "#endif\n",
    ),
    (
        "HalDisplay.cpp",
        INSERT,
        "static std::atomic<bool> pendingPresent{false};",
        "// E-inx: read by the simulator control channel's screenshot (src/simulator/SimControl.cpp).\n"
        "uint32_t *einxSimPixelBuffer() { return pixelBuf; }\n",
    ),
]

OVERRIDE_MARKER = "// E-inx override written by scripts/simulator_lib_overrides.py"
BEGIN = "// >>> E-inx simulator_lib_overrides.py\n"
WAS = "// was: "
END = "// <<< E-inx simulator_lib_overrides.py\n"

project_dir = env.subst("$PROJECT_DIR")  # noqa: F821
lib_src = os.path.join(env.subst("$PROJECT_LIBDEPS_DIR"), env.subst("$PIOENV"), "simulator", "src")  # noqa: F821


def log(message):
    print("simulator_lib_overrides: " + message)


def resolve_placeholders(text, target):
    while "{project:" in text:
        start = text.index("{project:")
        end = text.index("}", start)
        path = os.path.join(project_dir, text[start + len("{project:") : end])
        text = text[:start] + os.path.relpath(path, os.path.dirname(target)) + text[end + 1 :]
    return text


def undo_edits(text):
    while BEGIN in text and END in text:
        start = text.index(BEGIN)
        stop = text.index(END, start) + len(END)
        block = text[start + len(BEGIN) : stop - len(END)]
        restored = block[len(WAS) :].split("\n", 1)[0] + "\n" if block.startswith(WAS) else ""
        text = text[:start] + restored + text[stop:]
    return text


def apply_overrides():
    for lib_header, project_file in OVERRIDES.items():
        target = os.path.join(lib_src, lib_header)
        rel = os.path.relpath(os.path.join(project_dir, project_file), os.path.dirname(target))
        content = '%s\n#pragma once\n#include "%s"\n' % (OVERRIDE_MARKER, rel)
        try:
            with open(target) as f:
                if f.read() == content:
                    continue
        except FileNotFoundError:
            pass
        with open(target, "w") as f:
            f.write(content)
        log("%s -> %s" % (lib_header, project_file))


def apply_edits():
    by_file = {}
    for lib_file, kind, anchor, text in EDITS:
        by_file.setdefault(lib_file, []).append((kind, anchor, text))
    for lib_file, edits in by_file.items():
        target = os.path.join(lib_src, lib_file)
        if not os.path.isfile(target):
            log("WARNING %s not in the library; skipped its edits" % lib_file)
            continue
        with open(target) as f:
            original = f.read()
        text = undo_edits(original)
        for kind, anchor, new_text in edits:
            new_text = resolve_placeholders(new_text, target)
            line_start = text.find(anchor)
            if line_start < 0:
                log("WARNING anchor not found in %s (library changed?): %s" % (lib_file, anchor.strip()))
                continue
            line_start = text.rfind("\n", 0, line_start) + 1
            line_end = text.index("\n", line_start) + 1
            if kind == REPLACE:
                block = BEGIN + WAS + text[line_start : line_end - 1] + "\n" + new_text + END
                text = text[:line_start] + block + text[line_end:]
            else:
                text = text[:line_end] + BEGIN + new_text + END + text[line_end:]
        if "snprintf" in text and "<cstdio>" not in text:
            text = text.replace("#include <cstdint>", "#include <cstdint>\n#include <cstdio>", 1)
        if text != original:
            with open(target, "w") as f:
                f.write(text)
            log("patched %s" % lib_file)


if os.path.isdir(lib_src):
    apply_overrides()
    apply_edits()
else:
    log("%s not found; skipping" % lib_src)
