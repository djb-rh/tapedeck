// Job history on LittleFS: /hist/<id>.tdj, newest first, pruned to
// settings.history_max. Uploads land in /spool first and are validated before
// they join the history. Thread-safe (web, printer and display tasks all use it).
#pragma once
#include <Arduino.h>
#include "jobfile.h"

struct JobEntry {
  uint32_t id = 0;
  tdj::Header hdr;
  uint32_t total_cols = 0;   // sum of page widths (one copy)
  uint16_t max_height = 0;   // tallest page, dots
  uint32_t file_size = 0;
  uint32_t printed = 0;      // times printed since boot
  char source[12] = "";      // "web", "api", "text", "pbm"
};

class JobStore {
 public:
  bool begin();
  // Validates `tmp_path`, moves it into the history and returns the new id
  // (0 on failure, with a reason in err).
  uint32_t add(const char* tmp_path, const char* source, String& err);
  int count();
  // i = 0 is the newest.
  bool at(int i, JobEntry& out);
  bool find(uint32_t id, JobEntry& out);
  bool remove(uint32_t id);
  void mark_printed(uint32_t id);
  static String path(uint32_t id);
  // Validate a job file in place and fill `e` (id/source untouched).
  static bool inspect(const char* path, JobEntry& e, String& err);

 private:
  void prune_locked();
  void load_index();
  SemaphoreHandle_t mu_ = nullptr;
  JobEntry* list_ = nullptr;  // newest first
  int n_ = 0, cap_ = 0;
  uint32_t next_id_ = 1;
};

extern JobStore jobs;
