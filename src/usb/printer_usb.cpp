#include "printer_usb.h"
#include <SPI.h>
#include "M5_Max3421E_Usb.h"

PrinterUsb printerUsb;
static M5_USBH_Host* g_host = nullptr;

// HIEN (reg 26): HXFRDN 0x80 | FRAME 0x40 | CONDET 0x20 | RCVDAV 0x04.
// FRAMEIRQ fires every millisecond once a device is attached; the TinyUSB
// driver only needs it to retry NAKed transfers. Serviced constantly over SPI
// it turns into a 1 kHz whine on the Core's speaker amp (see UPS Watcher), so
// it is masked except while one of our transfers is in flight.
void PrinterUsb::frame_irq(bool on) {
  if (g_host) g_host->max3421_writeRegister(26 << 3, on ? 0xE4 : 0xA4, false);
}

static void utf16_to_ascii(const uint8_t* buf, size_t buflen, char* out, size_t outlen) {
  size_t n = buf[0] < buflen ? buf[0] : buflen;
  size_t o = 0;
  for (size_t i = 2; i + 1 < n && o + 1 < outlen; i += 2) {
    uint16_t c = buf[i] | (buf[i + 1] << 8);
    out[o++] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
  }
  out[o] = 0;
  while (o && out[o - 1] == ' ') out[--o] = 0;
}

// Bit-banged SPI on the M-Bus pins (SCK 18, MOSI 23, MISO 19).
static uint8_t bb_xfer(uint8_t out) {
  uint8_t in = 0;
  for (int b = 7; b >= 0; --b) {
    digitalWrite(23, (out >> b) & 1); delayMicroseconds(2);
    digitalWrite(18, HIGH); delayMicroseconds(2);
    in = (in << 1) | digitalRead(19);
    digitalWrite(18, LOW); delayMicroseconds(2);
  }
  return in;
}
static void bb_write(int cs, uint8_t reg, uint8_t v) {
  digitalWrite(cs, LOW); bb_xfer((reg << 3) | 0x02); bb_xfer(v); digitalWrite(cs, HIGH); delayMicroseconds(5);
}
static uint8_t bb_read(int cs, uint8_t reg) {
  digitalWrite(cs, LOW); bb_xfer(reg << 3); uint8_t v = bb_xfer(0); digitalWrite(cs, HIGH); delayMicroseconds(5);
  return v;
}

void PrinterUsb::detect_pins(int& cs_out, int& int_out) {
  pinMode(18, OUTPUT); digitalWrite(18, LOW);
  pinMode(23, OUTPUT); digitalWrite(23, LOW);
  pinMode(19, INPUT);
  const int cands[] = {13, 5, 0};  // USB Module v1.2 "SS Select" CH1/CH2/CH3
  for (int cs : cands) { pinMode(cs, OUTPUT); digitalWrite(cs, HIGH); }  // a floating SS hears LCD traffic
  cs_out = -1;
  for (int cs : cands) {
    bb_write(cs, 17, 0x10);  // PINCTL: full-duplex SPI
    uint8_t rev = bb_read(cs, 18);
    Serial.printf("[usb] CS G%d: REVISION 0x%02X\r\n", cs, rev);
    if (cs_out < 0 && (rev == 0x12 || rev == 0x13)) cs_out = cs;
  }
  int_out = -1;
  if (cs_out >= 0) {
    // "INT Select" CH1 = G35, CH2 = G34. Reset the chip so OSCOKIRQ is
    // pending (it sets once the crystal is running), enable only that IRQ,
    // and watch which pin follows CPUCTL.IE: idle high with IE off, driven
    // low with IE on.
    pinMode(34, INPUT); pinMode(35, INPUT);
    bb_write(cs_out, 15, 0x20);      // USBCTL: CHIPRES
    bb_write(cs_out, 15, 0x00);
    bb_write(cs_out, 17, 0x10);      // PINCTL: full duplex again, edge INT, active low
    for (int i = 0; i < 50 && !(bb_read(cs_out, 13) & 0x01); ++i) delay(1);  // USBIRQ.OSCOKIRQ
    bb_write(cs_out, 26, 0x00);      // HIEN off
    bb_write(cs_out, 14, 0x01);      // USBIEN = OSCOKIE
    bb_write(cs_out, 16, 0x00);      // CPUCTL: IE off
    delay(1);
    int off34 = digitalRead(34), off35 = digitalRead(35);
    bb_write(cs_out, 16, 0x01);      // IE on
    delay(1);
    int on34 = digitalRead(34), on35 = digitalRead(35);
    bb_write(cs_out, 16, 0x00);
    bb_write(cs_out, 14, 0x00);
    Serial.printf("[usb] INT probe: G34 %d->%d  G35 %d->%d\r\n", off34, on34, off35, on35);
    if (off35 && !on35) int_out = 35;
    else if (off34 && !on34) int_out = 34;
  }
  pinMode(18, INPUT); pinMode(23, INPUT);
}

bool PrinterUsb::begin(int cs, int intr) {
  g_host = new M5_USBH_Host(&SPI, 18, 23, 19, cs, intr);
  if (!g_host->begin(0)) return false;
  return true;
}

void PrinterUsb::pump(uint32_t wait_ms) {
  if (!g_host) { if (wait_ms) delay(wait_ms); return; }
  g_host->task(wait_ms);
  if (pending_mount_) {
    uint8_t d = pending_mount_;
    pending_mount_ = 0;
    daddr_ = d;
    ++gen_;
    if (!setup_device()) {
      Serial.println("[usb] device has no usable printer interface");
    }
  }
}

bool PrinterUsb::device_present() {
  if (!g_host) return false;
  uint8_t hrsl = g_host->max3421_readRegister(31 << 3, false);  // HRSL: KSTATUS 0x80, JSTATUS 0x40
  return (hrsl & 0xC0) != 0;  // J or K = something is holding a data line up
}

void PrinterUsb::on_mount(uint8_t daddr) { pending_mount_ = daddr; }

void PrinterUsb::on_umount(uint8_t daddr) {
  if (daddr != daddr_) return;
  daddr_ = 0; ready_ = false; ep_in_ = ep_out_ = 0;
  vid_ = pid_ = 0; mfr_[0] = prod_[0] = serial_[0] = 0;
  ++gen_;
}

bool PrinterUsb::setup_device() {
  ready_ = false;
  tuh_vid_pid_get(daddr_, &vid_, &pid_);
  uint8_t sbuf[80];
  if (tuh_descriptor_get_manufacturer_string_sync(daddr_, 0x0409, sbuf, sizeof sbuf) == XFER_RESULT_SUCCESS)
    utf16_to_ascii(sbuf, sizeof sbuf, mfr_, sizeof mfr_);
  if (tuh_descriptor_get_product_string_sync(daddr_, 0x0409, sbuf, sizeof sbuf) == XFER_RESULT_SUCCESS)
    utf16_to_ascii(sbuf, sizeof sbuf, prod_, sizeof prod_);
  if (tuh_descriptor_get_serial_string_sync(daddr_, 0x0409, sbuf, sizeof sbuf) == XFER_RESULT_SUCCESS)
    utf16_to_ascii(sbuf, sizeof sbuf, serial_, sizeof serial_);

  // Configuration descriptor header first, then the whole thing.
  cfg_len_ = 0;
  if (tuh_descriptor_get_configuration_sync(daddr_, 0, cfg_, 9) != XFER_RESULT_SUCCESS) return false;
  uint16_t total = cfg_[2] | (cfg_[3] << 8);
  if (total > sizeof cfg_) total = sizeof cfg_;
  if (tuh_descriptor_get_configuration_sync(daddr_, 0, cfg_, total) != XFER_RESULT_SUCCESS) return false;
  cfg_len_ = total;

  // Walk interfaces: want class 7 (printer) with a bulk OUT, ideally a bulk IN.
  const uint8_t* p = cfg_;
  const uint8_t* end = cfg_ + total;
  bool in_printer = false;
  const tusb_desc_endpoint_t* eo = nullptr;
  const tusb_desc_endpoint_t* ei = nullptr;
  while (p + 2 <= end && p[0]) {
    if (p[1] == TUSB_DESC_INTERFACE) {
      if (eo) break;  // already got the first printer interface
      in_printer = (p[5] == 0x07);
    } else if (p[1] == TUSB_DESC_ENDPOINT && in_printer) {
      auto* ep = (const tusb_desc_endpoint_t*)p;
      if ((ep->bmAttributes.xfer) == TUSB_XFER_BULK) {
        if (tu_edpt_dir(ep->bEndpointAddress) == TUSB_DIR_IN) { if (!ei) ei = ep; }
        else if (!eo) eo = ep;
      }
    }
    p += p[0];
  }
  if (!eo) return false;
  if (!tuh_edpt_open(daddr_, eo)) return false;
  ep_out_ = eo->bEndpointAddress;
  ep_out_size_ = tu_edpt_packet_size(eo);
  if (ei) {
    if (tuh_edpt_open(daddr_, ei)) {
      ep_in_ = ei->bEndpointAddress;
      ep_in_size_ = tu_edpt_packet_size(ei);
    }
  }
  ready_ = true;
  frame_irq(false);
  return true;
}

struct XferWait {
  volatile bool done;
  volatile xfer_result_t result;
  volatile uint32_t actual;
};

static void xfer_cb(tuh_xfer_t* x) {
  auto* w = (XferWait*)x->user_data;
  w->result = x->result;
  w->actual = x->actual_len;
  w->done = true;
}

bool PrinterUsb::xfer(uint8_t ep, uint8_t* buf, size_t len, uint32_t timeout_ms, uint32_t* actual) {
  if (!ready_ || !ep) return false;
  XferWait w{false, XFER_RESULT_INVALID, 0};
  tuh_xfer_t x = {};
  x.daddr = daddr_;
  x.ep_addr = ep;
  x.buflen = len;
  x.buffer = buf;
  x.complete_cb = xfer_cb;
  x.user_data = (uintptr_t)&w;
  frame_irq(true);
  if (!tuh_edpt_xfer(&x)) { frame_irq(false); return false; }
  uint32_t t0 = millis();
  while (!w.done) {
    g_host->task(1);
    if (!ready_) { frame_irq(false); return false; }  // unplugged while waiting
    if (millis() - t0 > timeout_ms) {
      tuh_edpt_abort_xfer(daddr_, ep);
      frame_irq(false);
      if (actual) *actual = 0;
      return false;
    }
  }
  frame_irq(false);
  if (actual) *actual = w.actual;
  return w.result == XFER_RESULT_SUCCESS;
}

bool PrinterUsb::write(const uint8_t* data, size_t len, uint32_t timeout_ms) {
  // TinyUSB wants a writable buffer; copy through a bounce buffer in chunks.
  static uint8_t bounce[512];
  while (len) {
    size_t n = len < sizeof bounce ? len : sizeof bounce;
    memcpy(bounce, data, n);
    uint32_t got = 0;
    if (!xfer(ep_out_, bounce, n, timeout_ms, &got)) return false;
    data += n; len -= n;
  }
  return true;
}

int PrinterUsb::read(uint8_t* buf, size_t cap, uint32_t timeout_ms) {
  if (!ep_in_) return -1;
  // Read whole packets only; the printer's status block is 32 bytes.
  size_t n = cap < ep_in_size_ ? cap : ep_in_size_;
  static uint8_t bounce[64];
  uint32_t got = 0;
  if (!xfer(ep_in_, bounce, n, timeout_ms, &got)) return ready_ ? 0 : -1;
  memcpy(buf, bounce, got);
  return (int)got;
}

extern "C" {
void tuh_mount_cb(uint8_t daddr) { printerUsb.on_mount(daddr); }
void tuh_umount_cb(uint8_t daddr) { printerUsb.on_umount(daddr); }
}
