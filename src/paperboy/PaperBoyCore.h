#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// PaperBoy/CrankBoy DMG core, packed 2bpp output (low pair first).
const char* pb_validate(const uint8_t* header, size_t size);
const char* pb_start(const uint8_t* rom, size_t size);
// Reader must copy the entire requested range or return false. ROM is never written.
// Only the two visible 16 KiB banks are retained in RAM; context lives until pb_stop.
typedef bool (*pb_rom_reader)(void* context, size_t offset, uint8_t* dest, size_t count);
const char* pb_start_reader(pb_rom_reader reader, void* context, size_t size);
void pb_stop(void);
bool pb_frame(uint8_t pressed, bool skip_render);
const uint8_t* pb_lcd(void);
uint8_t* pb_save_data(void);
size_t pb_save_size(void);
bool pb_save_dirty(void);
void pb_save_clean(void);
void pb_rtc_tick(void);
#ifdef __cplusplus
}
#endif
