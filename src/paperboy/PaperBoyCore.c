#include "PaperBoyCore.h"

#include <stdlib.h>
#include <string.h>
static uint8_t* rom_bank(unsigned bank, unsigned slot);
static bool rom_read(size_t offset, uint8_t* dest, size_t count);
#define PGB_ROM_BANK(bank, slot) rom_bank(bank, slot)
#define PGB_ROM_READ(offset, dest, count) rom_read(offset, dest, count)
#define PGB_CGB 0
#define ENABLE_SOUND 0
#define ENABLE_LCD 1
#define PGB_IMPL
#include "core/peanut_gb.h"

int preferences_cgb_speed;
int preferences_ppu_timing;
int audio_enabled;
static gb_s* core;
static uint8_t* memory;
static bool failed;
static pb_rom_reader read_rom;
static void* read_context;
static const uint8_t* contiguous_rom;
static size_t rom_length;
static uint8_t rom_header[0x150];
static uint8_t* rom_windows;
static unsigned loaded_bank[2];
static bool rom_read(size_t offset, uint8_t* dest, size_t count) {
  if (offset > rom_length || count > rom_length - offset) {
    failed = true;
    return false;
  }
  if (read_rom) {
    if (!read_rom(read_context, offset, dest, count)) {
      failed = true;
      return false;
    }
  } else
    memcpy(dest, contiguous_rom + offset, count);
  return true;
}
static uint8_t* rom_bank(unsigned bank, unsigned slot) {
  if (!read_rom) return (uint8_t*)contiguous_rom + (size_t)bank * 0x4000;
  uint8_t* window = rom_windows + slot * 0x4000;
  if (loaded_bank[slot] != bank) {
    if (failed || !rom_read((size_t)bank * 0x4000, window, 0x4000)) {
      memset(window, 0xff, 0x4000);
    } else
      loaded_bank[slot] = bank;
  }
  return window;
}
void __gb_on_breakpoint(gb_s* gb, int breakpoint_number) {
  (void)gb;
  (void)breakpoint_number;
}
static void error_cb(gb_s* gb, enum gb_error_e err, uint16_t addr) {
  (void)gb;
  (void)err;
  (void)addr;
  failed = true;
}
const char* pb_validate(const uint8_t* h, size_t size) {
  if (!h || size < 32768) return "ROM is smaller than 32 KiB";
  if (h[0x143] == 0xC0) return "Color-only ROM: not supported yet";
  if (h[0x148] > 6 || size != ((size_t)32768 << h[0x148])) return "ROM must be complete and at most 2 MiB";
  if (h[0x149] > 5) return "Invalid cartridge RAM size";
  uint8_t sum = 0;
  for (unsigned i = 0x134; i <= 0x14c; ++i) sum = sum - h[i] - 1;
  if (sum != h[0x14d]) return "Invalid ROM header checksum";
  // Supported, non-peripheral cartridge families. Reject MBC7 tilt/EEPROM.
  const uint8_t t = h[0x147];
  if (!(t <= 3 || t == 5 || t == 6 || t == 8 || t == 9 || (t >= 0x0f && t <= 0x13) || (t >= 0x19 && t <= 0x1e)))
    return "Unsupported cartridge controller";
  return NULL;
}
void pb_stop(void) {
  if (core) free(core->gb_cart_ram);
  free(core);
  core = NULL;
  free(memory);
  memory = NULL;
  free(rom_windows);
  rom_windows = NULL;
  read_rom = NULL;
  read_context = NULL;
  contiguous_rom = NULL;
  rom_length = 0;
}
static const char* start_core(const uint8_t* rom, size_t size) {
  const char* error = pb_validate(rom, size);
  if (error) return error;
  core = calloc(1, sizeof(*core));
  // Local reset patch limits DMG clearing to its actual RAM banks.
  memory = calloc(1, WRAM_SIZE + VRAM_SIZE + LCD_BUFFER_BYTES);
  if (!core || !memory) {
    pb_stop();
    return "Not enough RAM for emulator";
  }
  enum gb_init_error_e result = gb_init(core, memory, memory + WRAM_SIZE, memory + WRAM_SIZE + VRAM_SIZE, (uint8_t*)rom,
                                        size, error_cb, NULL, false);
  if (result != GB_INIT_NO_ERROR && result != GB_INIT_NO_ERROR_BUT_REQUIRES_CGB) {
    pb_stop();
    return "Unsupported ROM";
  }
  gb_reset(core, false);
  size_t n = gb_get_save_size(core);
  // Small dummy RAM also protects core pointer arithmetic for ROM-only carts.
  core->gb_cart_ram = calloc(1, n ? n : 1);
  if (!core->gb_cart_ram) {
    pb_stop();
    return "Not enough cartridge RAM";
  }
  core->gb_cart_ram_size = n;
  __gb_update_selected_cart_bank_addr(core);
  gb_init_lcd(core);
  core->direct.joypad = 0xff;
  core->direct.joypad_interrupt_delay = -1;
  if (failed) {
    pb_stop();
    return "SD ROM read failed";
  }
  return NULL;
}
const char* pb_start(const uint8_t* rom, size_t size) {
  pb_stop();
  failed = false;
  contiguous_rom = rom;
  rom_length = size;
  const char* error = start_core(rom, size);
  if (error) pb_stop();
  return error;
}
const char* pb_start_reader(pb_rom_reader reader, void* context, size_t size) {
  pb_stop();
  failed = false;
  if (!reader || size < 32768) return "Invalid ROM reader or size";
  read_rom = reader;
  read_context = context;
  rom_length = size;
  if (!rom_read(0, rom_header, sizeof(rom_header))) {
    pb_stop();
    return "SD ROM header read failed";
  }
  const char* error = pb_validate(rom_header, size);
  if (error) {
    pb_stop();
    return error;
  }
  rom_windows = malloc(0x8000);
  if (!rom_windows) {
    pb_stop();
    return "Not enough RAM for ROM banks";
  }
  loaded_bank[0] = loaded_bank[1] = ~0u;
  error = start_core(rom_header, size);
  if (error) pb_stop();
  return error;
}
bool pb_frame(uint8_t pressed, bool skip_render) {
  if (!core || failed) return false;
  // Latch a falling edge on the selected JOYP lines even when IE is disabled.
  // Upstream's front-end delay hook never scheduled this on a host key press.
  const uint8_t before = __gb_read_full(core, 0xff00) & 0x0f;
  core->direct.joypad = (uint8_t)~pressed;
  const uint8_t after = __gb_read_full(core, 0xff00) & 0x0f;
  if (before & (uint8_t)~after) core->gb_reg.IF |= CONTROL_INTR;
  core->direct.frame_skip = skip_render;
  gb_run_frame__dmg(core);
  return !failed;
}
const uint8_t* pb_lcd(void) { return core ? core->lcd : NULL; }
uint8_t* pb_save_data(void) { return core ? core->gb_cart_ram : NULL; }
size_t pb_save_size(void) { return core ? core->gb_cart_ram_size : 0; }
bool pb_save_dirty(void) { return core && (core->direct.sram_dirty || core->direct.sram_updated); }
void pb_save_clean(void) {
  if (core) {
    core->direct.sram_dirty = 0;
    core->direct.sram_updated = 0;
  }
}
void pb_rtc_tick(void) {
  if (core) gb_tick_rtc(core);
}
