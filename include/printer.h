// Printer service: one FreeRTOS task owns the USB host stack, keeps the tape
// status fresh while idle, and runs queued jobs from the history.
#pragma once
#include <Arduino.h>
#include "ptouch.h"

enum class PState : uint8_t { NO_MODULE, NO_PRINTER, IDLE, PRINTING, ERROR };

struct PrinterSnapshot {
  PState state = PState::NO_MODULE;
  bool status_valid = false;
  pt::Status st;
  uint32_t status_age_ms = 0;
  char product[40] = "";
  char serial[40] = "";
  int cs = -1, intr = -1;
  // current / last job
  uint32_t job_id = 0;
  char job_title[49] = "";
  int pages_done = 0, pages_total = 0;
  uint32_t last_job_id = 0;
  bool last_ok = true;
  char last_error[120] = "";   // last job failure or printer error text
  char printer_error[120] = ""; // live error from the status block
  uint32_t jobs_done = 0, labels_done = 0;
  int queued = 0;
  bool label_held = false;      // chain mode: last label still inside the printer
  // Tape colours. The PT-D600 always reports white tape / black ink, so the
  // user's choice per cassette type (width + media) overrides the status block.
  uint8_t tape_color = 0, ink_color = 0;  // effective Brother colour codes
  bool colors_user = false;     // from the user's choice, not the printer
  bool colors_confirmed = true; // false after a cover-open or cassette change until the user confirms
  uint32_t tape_epoch = 0;      // bumps on every detected cassette change
  uint32_t seq = 0;             // bumps on every visible change
};

class Printer {
 public:
  bool begin(int cs, int intr);
  PrinterSnapshot snapshot();
  // Queue a history job for printing. False if the queue is full or there is no printer.
  bool enqueue(uint32_t job_id, String& err);
  void refresh_status() { want_status_ = true; }
  // Set (and remember) the colours of the loaded cassette type.
  bool set_colors(uint8_t tape, uint8_t ink, String& err);
  uint32_t seq();

 private:
  static void task_fn(void*);
  void run();
  bool query_status(uint32_t timeout_ms = 1500);
  bool read_status(pt::Status& s, uint32_t timeout_ms);
  void print_job(uint32_t id);
  void set_error(const char* fmt, ...);
  void update_live_error();
  void bump();
  void on_status_locked();
  void load_colors_locked();
  uint8_t sig_w_ = 0xFF, sig_m_ = 0xFF;
  bool cover_open_ = false;

  SemaphoreHandle_t mu_ = nullptr;
  QueueHandle_t q_ = nullptr;
  PrinterSnapshot snap_;
  volatile bool want_status_ = true;
  uint32_t status_ms_ = 0;
  bool module_ok_ = false;
};

extern Printer printer;
