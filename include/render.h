// Builds TDJ1 job files on the device: text jobs from the JSON API (rendered
// with M5GFX's built-in fonts) and 1-bit PBM images.
#pragma once
#include <Arduino.h>

namespace render {

// JSON text job -> job file at out_path. print_pins = printable dots of the
// loaded tape (the label height), tape_mm = its width (stored in the header).
bool text_job(const char* json, size_t len, int print_pins, int tape_mm, const char* out_path, String& err);
// Binary PBM (P4) file -> one-page job file.
bool pbm_job(const char* pbm_path, const char* out_path, const String& title, int copies, String& err);

}  // namespace render
