// USB transport for a P-touch on the M5Stack USB Module (MAX3421E).
// Every call except begin() must come from the one task that runs pump():
// TinyUSB's host stack is not re-entrant, and the blocking helpers here pump
// it themselves while they wait.
#pragma once
#include <Arduino.h>

class PrinterUsb {
 public:
  bool begin(int cs, int intr);
  // Runs the host stack once; call often from the USB task.
  void pump(uint32_t wait_ms = 0);

  bool ready() const { return ready_; }      // printer interface found and endpoints open
  bool attached() const { return daddr_ != 0; }
  uint16_t vid() const { return vid_; }
  uint16_t pid() const { return pid_; }
  const char* manufacturer() const { return mfr_; }
  const char* product() const { return prod_; }
  const char* serial() const { return serial_; }
  // Bumped on every attach/detach so callers can notice a re-plug.
  uint32_t generation() const { return gen_; }

  // Blocking bulk OUT. False on timeout/stall/detach.
  bool write(const uint8_t* data, size_t len, uint32_t timeout_ms = 3000);
  // Blocking bulk IN of up to `cap` bytes; returns bytes read, 0 on timeout, -1 on error.
  int read(uint8_t* buf, size_t cap, uint32_t timeout_ms);
  // Polls the IN endpoint briefly; same return as read().
  int poll_in(uint8_t* buf, size_t cap) { return read(buf, cap, 30); }

  // Raw configuration descriptor (for the probe/diagnostics).
  const uint8_t* config_desc() const { return cfg_; }
  uint16_t config_len() const { return cfg_len_; }

  // Called from tuh_mount_cb / tuh_umount_cb.
  void on_mount(uint8_t daddr);
  void on_umount(uint8_t daddr);

  void frame_irq(bool on);

 private:
  bool setup_device();
  bool xfer(uint8_t ep, uint8_t* buf, size_t len, uint32_t timeout_ms, uint32_t* actual);

  volatile uint8_t pending_mount_ = 0;
  uint8_t daddr_ = 0;
  uint8_t ep_in_ = 0, ep_out_ = 0;
  uint16_t ep_out_size_ = 64, ep_in_size_ = 64;
  bool ready_ = false;
  uint16_t vid_ = 0, pid_ = 0;
  char mfr_[40] = "", prod_[40] = "", serial_[40] = "";
  uint8_t cfg_[256];
  uint16_t cfg_len_ = 0;
  uint32_t gen_ = 0;
};

extern PrinterUsb printerUsb;
