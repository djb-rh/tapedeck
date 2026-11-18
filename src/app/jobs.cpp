#include "jobs.h"
#include <LittleFS.h>
#include "settings.h"

JobStore jobs;

struct Lock {
  SemaphoreHandle_t m;
  explicit Lock(SemaphoreHandle_t mm) : m(mm) { xSemaphoreTakeRecursive(m, portMAX_DELAY); }
  ~Lock() { xSemaphoreGiveRecursive(m); }
};

String JobStore::path(uint32_t id) {
  char b[32];
  snprintf(b, sizeof b, "/hist/%06lu.tdj", (unsigned long)id);
  return b;
}

bool JobStore::inspect(const char* p, JobEntry& e, String& err) {
  File f = LittleFS.open(p, "r");
  if (!f) { err = "cannot open job file"; return false; }
  uint8_t h[tdj::kHeaderSize];
  size_t size = f.size();
  if (f.read(h, sizeof h) != sizeof h || !tdj::read_header(h, e.hdr)) { err = "not a TDJ1 job or bad header"; return false; }
  uint32_t off = tdj::kHeaderSize;
  e.total_cols = 0; e.max_height = 0;
  for (int i = 0; i < e.hdr.pages; ++i) {
    uint8_t ph[4];
    tdj::PageInfo pi;
    if (!f.seek(off) || f.read(ph, 4) != 4 || !tdj::read_page_header(ph, pi)) {
      err = "page " + String(i + 1) + ": bad page header"; return false;
    }
    off += 4 + pi.bytes();
    if (off > size) { err = "page " + String(i + 1) + ": file truncated"; return false; }
    e.total_cols += pi.width;
    if (pi.height > e.max_height) e.max_height = pi.height;
  }
  e.file_size = size;
  return true;
}

void JobStore::load_index() {
  n_ = 0;
  File d = LittleFS.open("/hist");
  if (!d) return;
  // Collect ids, then sort newest first.
  for (File f = d.openNextFile(); f; f = d.openNextFile()) {
    String name = f.name();
    f.close();
    if (!name.endsWith(".tdj")) continue;
    uint32_t id = strtoul(name.c_str(), nullptr, 10);
    if (!id) continue;
    if (n_ >= cap_) break;
    JobEntry e;
    String err;
    if (!inspect(path(id).c_str(), e, err)) {
      Serial.printf("[jobs] dropping %s: %s\r\n", name.c_str(), err.c_str());
      LittleFS.remove(path(id));
      continue;
    }
    e.id = id;
    strlcpy(e.source, "saved", sizeof e.source);
    list_[n_++] = e;
    if (id >= next_id_) next_id_ = id + 1;
  }
  for (int i = 1; i < n_; ++i)
    for (int j = i; j > 0 && list_[j].id > list_[j - 1].id; --j) std::swap(list_[j], list_[j - 1]);
}

bool JobStore::begin() {
  mu_ = xSemaphoreCreateRecursiveMutex();
  cap_ = 51;
  list_ = new JobEntry[cap_];
  if (!LittleFS.exists("/hist")) LittleFS.mkdir("/hist");
  if (!LittleFS.exists("/spool")) LittleFS.mkdir("/spool");
  Lock l(mu_);
  load_index();
  prune_locked();
  Serial.printf("[jobs] %d in history, next id %lu\r\n", n_, (unsigned long)next_id_);
  return true;
}

void JobStore::prune_locked() {
  while (n_ > settings.history_max) {
    LittleFS.remove(path(list_[n_ - 1].id));
    --n_;
  }
}

uint32_t JobStore::add(const char* tmp, const char* source, String& err) {
  JobEntry e;
  if (!inspect(tmp, e, err)) { LittleFS.remove(tmp); return 0; }
  Lock l(mu_);
  e.id = next_id_++;
  strlcpy(e.source, source, sizeof e.source);
  if (!e.hdr.created && time(nullptr) > 1600000000) {
    // stamp the creation time into the file
    e.hdr.created = (uint32_t)time(nullptr);
    File f = LittleFS.open(tmp, "r+");
    if (f) {
      uint8_t h[tdj::kHeaderSize];
      tdj::write_header(e.hdr, h);
      f.seek(0);
      f.write(h, sizeof h);
      f.close();
    }
  }
  String dst = path(e.id);
  LittleFS.remove(dst);
  if (!LittleFS.rename(tmp, dst)) { err = "rename failed (flash full?)"; LittleFS.remove(tmp); return 0; }
  // make room, then insert at the front
  while (n_ >= cap_) { LittleFS.remove(path(list_[n_ - 1].id)); --n_; }
  for (int i = n_; i > 0; --i) list_[i] = list_[i - 1];
  list_[0] = e;
  ++n_;
  prune_locked();
  return e.id;
}

int JobStore::count() { Lock l(mu_); return n_; }

bool JobStore::at(int i, JobEntry& out) {
  Lock l(mu_);
  if (i < 0 || i >= n_) return false;
  out = list_[i];
  return true;
}

bool JobStore::find(uint32_t id, JobEntry& out) {
  Lock l(mu_);
  for (int i = 0; i < n_; ++i) if (list_[i].id == id) { out = list_[i]; return true; }
  return false;
}

bool JobStore::remove(uint32_t id) {
  Lock l(mu_);
  for (int i = 0; i < n_; ++i) {
    if (list_[i].id != id) continue;
    LittleFS.remove(path(id));
    for (int k = i; k + 1 < n_; ++k) list_[k] = list_[k + 1];
    --n_;
    return true;
  }
  return false;
}

void JobStore::mark_printed(uint32_t id) {
  Lock l(mu_);
  for (int i = 0; i < n_; ++i) if (list_[i].id == id) { list_[i].printed++; return; }
}
