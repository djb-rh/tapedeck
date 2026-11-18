// Core Basic screen: printer + tape status, history browser, reprint.
//   A = newer job, B = print the shown job (press twice), C = older job.
//   Hold A = refresh printer status, hold B = info screen (QR code for the
//   editor, tape colour picker), hold C = jump to newest.
#pragma once
#include <Arduino.h>
#include "printer.h"

class Display {
 public:
  void begin();
  void task();
  void wake();
  // Simulated buttons from the serial console (0 = A, 1 = B, 2 = C; long = hold).
  void press(int btn, bool hold = false);
  // Dumps the screen over serial as run-length RGB565 (tools/screenshot.py).
  void screenshot();

 private:
  void draw_header(bool force);
  void draw_tape(bool force);
  void draw_job(bool force);
  void draw_footer(bool force);
  void draw_setup(bool force);
  void handle_buttons();
  void do_print();
  void open_info();
  void close_info(bool save);
  void draw_info();
  void draw_info_footer();

  bool lit_ = true;
  uint32_t last_activity_ms_ = 0;
  int sel_ = 0;                 // history index shown, 0 = newest
  uint32_t sel_id_ = 0;
  uint32_t last_seq_ = 0xFFFFFFFF;
  uint32_t last_job_sig_ = 0;
  String last_header_;
  String last_footer_;
  uint32_t confirm_until_ = 0;  // B pressed once: waiting for the second press
  uint32_t toast_until_ = 0;
  String toast_;
  bool was_setup_ = false;
  bool force_all_ = true;
  bool setup_dismissed_ = false;
  bool info_ = false, info_dirty_ = false, info_have_tape_ = false;
  int info_preset_ = 0;            // index into choices_
  struct Choice { uint8_t tape, ink, width; bool recent, same_type; };
  Choice choices_[32];
  int n_choices_ = 0;
  uint32_t info_until_ = 0;
};

extern Display display;
