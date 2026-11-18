#include "innotech_genesis.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

#include "esphome/core/application.h"
#include "esphome/core/log.h"
#include "esphome/components/network/util.h"

#include <lwip/sockets.h>
#include <fcntl.h>

namespace esphome {
namespace innotech_genesis {

ESPHOME_LOG_TAG(TAG, "innotech_genesis");

// Net protocol drivers/commands
static constexpr uint8_t DRV_SYSTEM = 0x00;
static constexpr uint8_t CMD_DEVICEINFO = 0xFF;
static constexpr uint8_t DRV_FLASH = 0x01;
static constexpr uint8_t CMD_SAVE_FLASH = 0x02;
static constexpr uint8_t DRV_BLOCK = 0x02;
static constexpr uint8_t CMD_READ_BLOCK = 0x05;
static constexpr uint8_t CMD_WRITE_BLOCK = 0x06;
static constexpr uint8_t CMD_BLOCK_COUNT = 0x08;
static constexpr uint8_t CMD_READ_OUTPUTS = 0x09;
static constexpr uint8_t DRV_STATUS = 0x09;
static constexpr uint8_t CMD_SYSTEM_READ = 0x01;
// Block type code in the record at [0x15] = iComm's Genesis II type id + 1 (User Variable 29, Digital Output 3).
static constexpr uint8_t BLOCK_TYPE_USER_VARIABLE = 0x1E;
static constexpr uint8_t BLOCK_TYPE_DIGITAL_OUTPUT = 0x04;
// Digital Output parameters (from iComm's Genesis II field table): DisableValue +0, FaultValue +1, Type +3,
// Forced +4, ForceValue +5, all u8.
static constexpr size_t DO_FORCED_OFFSET = 4;
static constexpr size_t DO_FORCE_VALUE_OFFSET = 5;
static constexpr uint32_t PARAM_POLL_MS = 30000;  // serial: re-read parameter points (block records)

static constexpr uint32_t SAVE_FLASH_TIMEOUT_MS = 12000;
static constexpr uint32_t BOOT_STATUS_DELAY_MS = 2000;
static constexpr uint32_t LISTEN_RETRY_MS = 5000;
static constexpr uint32_t AUTO_SWITCH_AFTER_MS = 20000;
static constexpr uint32_t SERIAL_PROBE_MS = 600000;  // auto mode: re-check the serial bus every 10 min
static constexpr uint8_t DEAD_AFTER_FAILURES = 3;

// iComm TCP protocol (Supervisor <-> iComm, port 20000): 78 56 | len16 | type16 | invoke32 | body
static constexpr uint16_t IC_REPLY = 1;
static constexpr uint16_t IC_ADVISE = 2;
static constexpr uint16_t IC_UPDATE = 3;
static constexpr uint16_t IC_UNADVISE = 4;
static constexpr uint16_t IC_WRITE = 6;
static constexpr uint16_t IC_RAW_REQUEST = 7;  // raw Net request passed through to the bus
static constexpr uint16_t IC_RAW_REPLY = 8;
static constexpr uint16_t IC_HELLO = 0x11;
static constexpr uint16_t IC_STATUS_OK = 1;
static constexpr uint16_t IC_STATUS_NO_VALUE = 0xF440;
static constexpr uint16_t IC_STATUS_BAD_FIELD = 0xF43B;
static constexpr uint16_t IC_STATUS_DEVICE_DEAD = 0xF444;
// The live value is called OUTPUT on outputs/valves/User Variables, Value on analogue inputs, State on digital inputs.
// The first IC_AUTO_FIELDS are tried in turn for live values; the rest are fixed names (parameter points, and
// outputs with their own names such as a PID loop's Direct = cooling demand / Reverse = heating demand).
// Indices must match ICOMM_FIELDS in sensor.py.
static constexpr const char *IC_FIELDS[] = {"OUTPUT", "Value", "State", "Forced", "ForceValue", "Direct", "Reverse"};
static constexpr uint8_t IC_AUTO_FIELDS = 3;
static constexpr uint8_t IC_FIELD_FORCED = 3;
static constexpr uint8_t IC_FIELD_FORCE_VALUE = 4;
static constexpr uint32_t IC_HANDLE_BASE = 0x10000;
static constexpr uint32_t IC_HANDLE_STRIDE = 8;  // handle = base + point index * stride + field index
static constexpr size_t IC_ADVISE_BATCH = 20;
static constexpr uint32_t IC_SEND_WAIT_MS = 1000;     // max wait for send-buffer space before giving up
static constexpr uint32_t IC_ADVISE_REPLY_MS = 3000;  // send the next advise batch anyway after this
static constexpr uint32_t IC_RAW_TIMEOUT_MS = 10000;  // raw requests via the server took 0.5-3 s in testing
static constexpr uint32_t IC_RECONNECT_MS = 10000;
static constexpr uint32_t IC_CONNECT_TIMEOUT_MS = 5000;
static constexpr uint32_t IC_SILENCE_MS = 45000;  // iComm sends a keepalive every ~10 s

// iComm server (Supervisor -> ESP). Message types beyond the client ones above:
static constexpr uint16_t IC_CONN_INFO = 0x09;       // -> 0x12
static constexpr uint16_t IC_CONN_INFO_REPLY = 0x12;
static constexpr uint16_t IC_CONN_LIST = 0x0B;       // -> 0x0C
static constexpr uint16_t IC_CONN_LIST_REPLY = 0x0C;
static constexpr uint16_t IC_DEV_SUMMARY = 0x0F;     // "conn:dev" -> 0x13
static constexpr uint16_t IC_DEV_SUMMARY_REPLY = 0x13;
static constexpr uint16_t IC_KEEPALIVE = 0x10;
static constexpr uint16_t IC_SESSION = 0x16;         // -> 0x17
static constexpr uint16_t IC_SESSION_REPLY = 0x17;
static constexpr uint16_t IC_DEV_LIST = 0x1A;        // "conn" -> 0x1B
static constexpr uint16_t IC_DEV_LIST_REPLY = 0x1B;
static constexpr uint16_t IC_STATUS_ERROR = 0xFC15;  // what iComm 2.38 answers for fields it can't read
static constexpr uint16_t IC_STATUS_RAW_DEAD = 0xF447;  // type-1 answer to a raw request for a dead controller
static constexpr uint32_t SERVER_KEEPALIVE_MS = 10000;
static constexpr uint32_t SERVER_POLL_MS = 2000;      // subscription polling
static constexpr uint32_t SERVER_SYS_MS = 30000;      // ConfigKey refresh
static constexpr uint32_t SERVER_RAW_TIMEOUT_MS = 3000;
static constexpr uint32_t SERVER_LISTEN_RETRY_MS = 5000;
// Replies copied from iComm 2.38 (a Supervisor capture): the connection list (Facility Manager + "BMS Network"
// as connection 1) and the connection's info ("BMS Network", "Serial\nCOM1@9600"). Zero-padded to full length.
static constexpr uint8_t SERVER_CONN_LIST[] = {
    0x02, 0x00, 0x03, 0x00, 0x01, 0x00, 0x01, 0x00, 'F', 'a', 'c', 'i', 'l', 'i', 't', 'y', ' ', 'M', 'a', 'n',
    'a', 'g', 'e', 'r', ' ', 'o', 'n', ' ', 'l', 'o', 'c', 'a', 'l', ' ', 'P', 'C', 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x04, 0x00, 'B', 'M', 'S', ' ', 'N', 'e', 't', 'w', 'o', 'r',
    'k', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x02};
static constexpr size_t SERVER_CONN_LIST_LEN = 154;
static constexpr uint8_t SERVER_CONN_INFO[] = {
    0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x04, 0x00, 0x0C, 0x00, 0x11, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00,
    'B', 'M', 'S', ' ', 'N', 'e', 't', 'w', 'o', 'r', 'k', 0, 'S', 'e', 'r', 'i', 'a', 'l', '\n', 'C', 'O', 'M',
    '1', '@', '9', '6', '0', '0'};
static constexpr size_t SERVER_CONN_INFO_LEN = 50;
static constexpr uint8_t SERVER_OK_TAIL[] = {0x01, 0x01, 0x00, 0x00};  // after echo + result in OK replies

uint16_t crc16_arc(const uint8_t *data, size_t len, uint16_t crc) {
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++)
      crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
  }
  return crc;
}

// ---------------------------------------------------------------- FrameParser

bool FrameParser::feed(uint8_t b) {
  switch (this->state_) {
    case 0:
      if (this->prev_ == 0x22 && b == 0xDD) {
        this->len_ = 0;
        this->state_ = 1;
      }
      this->prev_ = b;
      return false;
    case 1:
      this->buf_[this->len_++] = b;
      if (this->len_ < 11)
        return false;
      if (crc16_arc(this->buf_, 11) != 0) {
        this->crc_errors++;
        this->state_ = 0;
        this->prev_ = b;
        return false;
      }
      this->need_ = this->buf_[7] | (this->buf_[8] << 8);
      if (this->need_ > MAX_FRAME_DATA) {
        this->state_ = 0;
        this->prev_ = b;
        return false;
      }
      this->state_ = 2;
      return false;
    default:
      this->buf_[this->len_++] = b;
      if (this->len_ < 11u + this->need_ + 2u)
        return false;
      this->state_ = 0;
      this->prev_ = 0;
      if (crc16_arc(this->buf_ + 11, this->need_ + 2) != 0) {
        this->crc_errors++;
        return false;
      }
      return true;
  }
}

// ----------------------------------------------------------------- GenesisBus setup

void GenesisBus::add_device(uint8_t address) {
  if (this->find_device_(address) != nullptr)
    return;
  if (this->devices_.size() >= MAX_DEVICES) {
    ESP_LOGE(TAG, "Too many devices (max %u)", (unsigned) MAX_DEVICES);
    return;
  }
  DeviceState d;
  d.address = address;
  this->devices_.push_back(d);
}

DeviceState *GenesisBus::find_device_(uint8_t address) {
  for (auto &d : this->devices_)
    if (d.address == address)
      return &d;
  return nullptr;
}

Point *GenesisBus::add_point_(uint8_t address, uint8_t block, uint8_t output) {
  this->add_device(address);
  if (this->points_.size() >= MAX_POINTS) {
    ESP_LOGE(TAG, "Too many points (max %u)", (unsigned) MAX_POINTS);
    return nullptr;
  }
  Point p;
  p.address = address;
  p.block = block;
  p.output = output;
  this->points_.push_back(p);
  return &this->points_[this->points_.size() - 1];
}

#ifdef USE_SENSOR
void GenesisBus::add_sensor(uint8_t address, uint8_t block, uint8_t output, sensor::Sensor *s, uint8_t icomm_field) {
  if (auto *p = this->add_point_(address, block, output)) {
    p->sensor = s;
    if (icomm_field != 0) {
      p->base_field = icomm_field;
      p->field_idx = icomm_field;
    }
  }
}
#endif

#ifdef USE_BINARY_SENSOR
void GenesisBus::add_binary_point(uint8_t address, uint8_t block, uint8_t output, binary_sensor::BinarySensor *s) {
  if (auto *p = this->add_point_(address, block, output))
    p->binary = s;
}
void GenesisBus::set_online_sensor(uint8_t address, binary_sensor::BinarySensor *s) {
  this->add_device(address);
  if (auto *d = this->find_device_(address))
    d->online_sensor = s;
}
#endif

#ifdef USE_NUMBER
void GenesisBus::add_number(uint8_t address, uint8_t block, number::Number *n) {
  if (auto *p = this->add_point_(address, block, 1))
    p->number = n;
}
#endif

#ifdef USE_CLIMATE
void GenesisBus::add_climate_point(uint8_t address, uint8_t block, uint8_t role, GenesisClimate *c) {
  if (auto *p = this->add_point_(address, block, 1)) {
    p->climate = c;
    p->climate_role = role;
  }
}
void GenesisBus::add_climate_param(uint8_t address, uint8_t block, uint8_t param, uint8_t role, GenesisClimate *c) {
  if (auto *p = this->add_point_(address, block, 1)) {
    p->param = param;
    p->base_field = param == PARAM_FORCED ? IC_FIELD_FORCED : IC_FIELD_FORCE_VALUE;
    p->field_idx = p->base_field;
    p->climate = c;
    p->climate_role = role;
  }
}
#endif

void GenesisBus::setup() {
  if (this->flow_control_pin_ != nullptr) {
    this->flow_control_pin_->setup();
    this->flow_control_pin_->digital_write(false);
  }
  // The TCP listener / iComm client are opened from loop() once the network stack is up.
  if (this->transport_ == TRANSPORT_ICOMM) {
    this->active_icomm_ = true;
    return;
  }
  this->set_timeout(BOOT_STATUS_DELAY_MS, [this]() {
    for (auto &d : this->devices_)
      this->check_status(d.address);
  });
}

static constexpr const char *TRANSPORTS[] = {"serial", "icomm", "auto"};

void GenesisBus::set_icomm_server(const std::string &value) {
  // "192.168.1.10" or "192.168.1.10:20001"; the port defaults to icomm_port (20000)
  std::string host = value;
  uint16_t port = 0;
  size_t colon = value.find(':');
  if (colon != std::string::npos) {
    host = value.substr(0, colon);
    char *end = nullptr;
    unsigned long p = strtoul(value.c_str() + colon + 1, &end, 10);
    if (end == value.c_str() + colon + 1 || *end != '\0' || p == 0 || p > 65535) {
      ESP_LOGW(TAG, "iComm server '%s' has a bad port, ignored", value.c_str());
      return;
    }
    port = p;
  }
  struct in_addr a {};
  if (!host.empty() && (host.size() >= sizeof(this->icomm_server_) || inet_aton(host.c_str(), &a) == 0)) {
    ESP_LOGW(TAG, "iComm server '%s' is not an IPv4 address, ignored", value.c_str());
    return;
  }
  snprintf(this->icomm_server_, sizeof(this->icomm_server_), "%s", host.c_str());
  this->icomm_server_port_ = host.empty() ? 0 : port;
  // An IP replaces the configured hosts; empty goes back to the ones from the YAML (`icomm_hosts`).
  this->icomm_hosts_ = this->icomm_config_hosts_;
  if (!host.empty()) {
    this->icomm_hosts_.clear();
    this->icomm_hosts_.push_back(this->icomm_server_);
  }
  this->icomm_host_idx_ = 0;
  this->icomm_last_try_ = 0;
  if (this->icomm_fd_ >= 0)
    this->icomm_close_();  // reconnect to the new server
  if (host.empty())
    ESP_LOGI(TAG, "iComm server: from config");
  else
    ESP_LOGI(TAG, "iComm server: %s:%u", this->icomm_server_, this->icomm_port_for_(this->icomm_server_));
}

void GenesisBus::set_transport_mode(uint8_t transport) {
  if (transport > TRANSPORT_AUTO)
    return;
  this->transport_ = transport;
  if (transport == TRANSPORT_SERIAL && this->active_icomm_) {
    this->switch_transport_(false);
  } else if (transport == TRANSPORT_ICOMM && !this->active_icomm_) {
    this->switch_transport_(true);
  }
  ESP_LOGI(TAG, "Transport set to %s", TRANSPORTS[transport]);
  if (transport == TRANSPORT_ICOMM && this->icomm_hosts_.empty())
    ESP_LOGW(TAG, "iComm mode but no iComm server is set");
}

void GenesisBus::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Innotech Genesis II bus:\n"
                "  Transport: %s (now using %s)\n"
                "  Polling: %s every %u ms, %u points/message\n"
                "  Status check every %u ms, reply timeout %u ms\n"
                "  Save writes to flash: %s\n"
                "  Devices: %u, points: %u",
                TRANSPORTS[this->transport_ % 3], this->active_icomm_ ? "iComm TCP" : "serial bus",
                YESNO(this->polling_), (unsigned) this->update_interval_, this->max_points_,
                (unsigned) this->status_interval_, (unsigned) this->reply_timeout_, YESNO(this->save_to_flash_),
                (unsigned) this->devices_.size(), (unsigned) this->points_.size());
  LOG_PIN("  Flow control pin: ", this->flow_control_pin_);
  if (this->tcp_port_ != 0)
    ESP_LOGCONFIG(TAG, "  TCP bridge port: %u (iComm 'TCP/IP' connection)", this->tcp_port_);
  for (const char *h : this->icomm_hosts_)
    ESP_LOGCONFIG(TAG, "  iComm server: %s:%u connection %u", h, this->icomm_port_for_(h), this->icomm_conn_);
}

bool GenesisBus::any_alive() const {
  for (const auto &d : this->devices_)
    if (d.alive)
      return true;
  return false;
}

bool GenesisBus::any_lost() const {
  for (const auto &d : this->devices_)
    if (d.seen && !d.alive)
      return true;
  return false;
}

bool GenesisBus::all_alive() const {
  if (this->devices_.empty())
    return false;
  for (const auto &d : this->devices_)
    if (!d.alive)
      return false;
  return true;
}

// ----------------------------------------------------------------- serial bus

bool GenesisBus::enqueue_(uint8_t dst, uint8_t drv, uint8_t cmd, RequestKind kind, const uint8_t *payload,
                          uint8_t len) {
  Request r;
  r.dst = dst;
  r.drv = drv;
  r.cmd = cmd;
  r.kind = kind;
  r.len = std::min<uint8_t>(len, REQ_PAYLOAD);
  if (payload != nullptr && r.len > 0)
    memcpy(r.payload, payload, r.len);
  if (!this->queue_.push(r)) {
    ESP_LOGW(TAG, "Queue full, dropping drv=%02X cmd=%02X for %u", drv, cmd, dst);
    return false;
  }
  return true;
}

void GenesisBus::send_raw_(const uint8_t *buf, size_t len, uint32_t now) {
  // Drop anything stale before we talk.
  uint8_t junk;
  while (this->available())
    this->read_byte(&junk);
  this->bus_parser_.reset();
  if (this->flow_control_pin_ != nullptr)
    this->flow_control_pin_->digital_write(true);
  this->write_array(buf, len);
  this->flush();
  if (this->flow_control_pin_ != nullptr)
    this->flow_control_pin_->digital_write(false);
  this->last_activity_ = now;
  this->sent_at_ = now;
  this->busy_ = true;
}

bool GenesisBus::via_icomm_() const {
  // On iComm, bus commands (writes, scans, record reads) ride inside iComm type-7 messages. Status probes stay
  // on serial: in iComm mode they are how we notice that the serial bus has come back.
  return this->active_icomm_ && this->current_.kind != REQ_STATUS && this->current_.kind != REQ_MVO &&
         this->current_.kind != REQ_BRIDGE;
}

void GenesisBus::transmit_(uint8_t dst, uint8_t drv, uint8_t cmd, const uint8_t *payload, size_t len, uint32_t now) {
  if (len > MAX_FRAME_DATA)
    len = MAX_FRAME_DATA;
  if (this->via_icomm_()) {
    this->icomm_raw_(dst, drv, cmd, payload, len, now);
    return;
  }
  this->current_seq_ = this->seq_++;
  uint8_t *f = this->tx_buf_;
  size_t n = 0;
  f[n++] = 0xFF;
  f[n++] = 0xFF;
  f[n++] = 0xFF;
  f[n++] = 0xFF;
  f[n++] = 0x22;
  f[n++] = 0xDD;
  uint8_t *hdr = f + n;
  hdr[0] = this->current_seq_;
  hdr[1] = dst;
  hdr[2] = drv;
  hdr[3] = 0xFF;
  hdr[4] = 0x00;
  hdr[5] = this->current_.flag;
  hdr[6] = cmd;
  hdr[7] = len & 0xFF;
  hdr[8] = len >> 8;
  n += 9;
  uint16_t c = crc16_arc(hdr, 9);
  f[n++] = c & 0xFF;
  f[n++] = c >> 8;
  if (len > 0)
    memcpy(f + n, payload, len);
  n += len;
  c = crc16_arc(payload, len);
  f[n++] = c & 0xFF;
  f[n++] = c >> 8;
  f[n++] = 0xFF;
  f[n++] = 0xFF;
  ESP_LOGV(TAG, "TX dst=%u drv=%02X cmd=%02X len=%u seq=%u", dst, drv, cmd, (unsigned) len, this->current_seq_);
  this->send_raw_(f, n, now);
}

void GenesisBus::send_next_(uint32_t now) {
  if (this->busy_)
    return;

  // 1. Bridged traffic first so iComm/Supervisor stay snappy.
  if (this->bridge_pending_) {
    this->bridge_pending_ = false;
    if (this->active_icomm_) {
      // No serial bus: send the bridged command through our iComm connection and rebuild the reply frame.
      this->current_.kind = REQ_BRIDGE_ICOMM;
      this->current_.dst = this->bridge_dst_;
      this->current_.drv = this->bridge_drv_;
      this->current_.cmd = this->bridge_cmd_;
      this->current_.flag = this->bridge_flag_;
      this->current_.client = 0xFF;
      this->current_.len = 0;
      this->transmit_(this->bridge_dst_, this->bridge_drv_, this->bridge_cmd_, this->bridge_buf_ + 6 + 11,
                      this->bridge_data_len_, now);
      return;
    }
    this->current_.kind = REQ_BRIDGE;
    this->current_.dst = this->bridge_dst_;
    this->current_.drv = 0;
    this->current_.cmd = 0;
    this->current_.len = 0;
    this->current_seq_ = this->bridge_seq_;
    this->send_raw_(this->bridge_buf_, this->bridge_len_, now);
    return;
  }

  // 1b. Raw requests from iComm server clients (Supervisor), one per client at a time.
  for (size_t i = 0; i < MAX_SERVER_CLIENTS; i++) {
    ServerClient &c = this->server_clients_[i];
    if (c.fd < 0 || !c.raw_pending || c.raw_inflight)
      continue;
    c.raw_pending = false;
    c.raw_inflight = true;
    this->current_.kind = REQ_SERVER_RAW;
    this->current_.dst = c.raw_dst;
    this->current_.drv = c.raw_drv;
    this->current_.cmd = c.raw_cmd;
    this->current_.flag = c.raw_flag;
    this->current_.len = 0;
    this->current_.client = i;
    this->current_.client_gen = c.gen;
    this->current_.invoke = c.raw_invoke;
    this->transmit_(c.raw_dst, c.raw_drv, c.raw_cmd, c.raw_buf, c.raw_len, now);
    return;
  }

  // 2. Setpoint write sequence: read block -> write patched block -> save to flash.
  if (this->write_stage_ == WS_IDLE && !this->writes_.empty()) {
    this->write_job_ = this->writes_.front();
    this->writes_.pop();
    this->write_stage_ = WS_READ;
  }
  if (this->write_stage_ == WS_READ || this->write_stage_ == WS_WRITE || this->write_stage_ == WS_SAVE) {
    this->current_.dst = this->write_job_.address;
    this->current_.len = 0;
    // Mark the stage as waiting BEFORE transmitting: over iComm a request can fail inside transmit_() (not
    // connected, send error) and finish_() then sets the next stage. Setting WS_WAIT afterwards would
    // overwrite that and wedge every later write.
    const WriteStage stage = this->write_stage_;
    this->write_stage_ = WS_WAIT;
    this->current_.flag = 0;
    this->current_.client = 0xFF;
    if (stage == WS_READ) {
      this->current_.kind = REQ_WRITE_READ;
      this->current_.drv = DRV_BLOCK;
      this->current_.cmd = CMD_READ_BLOCK;
      this->transmit_(this->write_job_.address, DRV_BLOCK, CMD_READ_BLOCK, &this->write_job_.block, 1, now);
    } else if (stage == WS_WRITE) {
      this->current_.kind = REQ_WRITE_BLOCK;
      this->current_.drv = DRV_BLOCK;
      this->current_.cmd = CMD_WRITE_BLOCK;
      this->transmit_(this->write_job_.address, DRV_BLOCK, CMD_WRITE_BLOCK, this->write_buf_, this->write_len_, now);
    } else {
      this->current_.kind = REQ_SAVE_FLASH;
      this->current_.drv = DRV_FLASH;
      this->current_.cmd = CMD_SAVE_FLASH;
      this->transmit_(this->write_job_.address, DRV_FLASH, CMD_SAVE_FLASH, nullptr, 0, now);
    }
    return;
  }

  // 3. Block scan, one block per request (generated on demand, never queued).
  if (this->scan_address_ != 0 && this->scan_count_ > 0 && !this->scan_waiting_ &&
      this->scan_next_ <= this->scan_count_) {
    uint8_t seq = this->scan_next_;
    this->current_.kind = REQ_SCAN_BLOCK;
    this->current_.flag = 0;
    this->current_.client = 0xFF;
    this->current_.dst = this->scan_address_;
    this->current_.drv = DRV_BLOCK;
    this->current_.cmd = CMD_READ_BLOCK;
    this->current_.len = 0;
    this->scan_waiting_ = true;
    this->transmit_(this->scan_address_, DRV_BLOCK, CMD_READ_BLOCK, &seq, 1, now);
    return;
  }

  // 4. Queued requests (status checks, polls, scan setup).
  if (this->queue_.empty())
    return;
  this->current_ = this->queue_.front();
  this->queue_.pop();
  this->transmit_(this->current_.dst, this->current_.drv, this->current_.cmd, this->current_.payload,
                  this->current_.len, now);
}

void GenesisBus::handle_frame_() {
  const uint32_t now = App.get_loop_component_start_time();
  this->frames_ok_++;
  this->last_activity_ = now;
  if (!this->busy_)
    return;
  // Reply header: seq, FF, 0, src, drv, 0, status, len16. Ignore our own echo (src FF).
  const uint8_t *h = this->bus_parser_.hdr();
  if (h[0] != this->current_seq_ || h[3] == 0xFF)
    return;
  if (this->current_.dst != 0xFF && h[3] != this->current_.dst)
    return;
  if (this->current_.kind == REQ_BRIDGE && this->client_fd_ >= 0) {
    // tx_buf_ is free once our request is on the wire; rebuild the full frame for the bridge client.
    uint8_t *f = this->tx_buf_;
    const size_t body = this->bus_parser_.body_len();
    f[0] = 0xFF;
    f[1] = 0xFF;
    f[2] = 0x22;
    f[3] = 0xDD;
    memcpy(f + 4, this->bus_parser_.body(), body);
    f[4 + body] = 0xFF;
    f[5 + body] = 0xFF;
    ::send(this->client_fd_, f, body + 6, MSG_DONTWAIT);
  }
  this->finish_(true, h[6], this->bus_parser_.data(), this->bus_parser_.data_len());
}

void GenesisBus::finish_(bool ok, uint8_t status, const uint8_t *d, size_t n) {
  this->busy_ = false;
  if (!this->active_icomm_ && this->current_.kind != REQ_BRIDGE) {
    if (auto *dev = this->find_device_(this->current_.dst)) {
      if (ok) {
        dev->failures = 0;
      } else if (dev->failures < 255 && ++dev->failures >= DEAD_AFTER_FAILURES && dev->alive) {
        ESP_LOGW(TAG, "Device %u stopped answering", dev->address);
        this->set_device_alive_(dev->address, false);
      }
    }
  }
  switch (this->current_.kind) {
    case REQ_STATUS:
      this->handle_status_(ok, status, d, n);
      break;
    case REQ_MVO:
      this->handle_mvo_(ok, status, d, n);
      break;
    case REQ_WRITE_READ:
      this->handle_write_read_(ok, status, d, n);
      break;
    case REQ_WRITE_BLOCK:
      if (!ok || status != 0) {
        ESP_LOGW(TAG, "Write %u/%u failed (ok=%d status=%u)", this->write_job_.address, this->write_job_.block, ok,
                 status);
        this->write_stage_ = WS_IDLE;
      } else {
        this->last_poll_ = 0;  // refresh values
        this->last_param_poll_ = 0;
        // No save-to-flash through iComm: the controller goes quiet while saving, iComm (not knowing why) marks
        // it Dead and only re-probes ~3 min later (seen 18 Nov 2026). The change stays in RAM until the next save.
        this->write_stage_ = (this->save_to_flash_ && !this->active_icomm_) ? WS_SAVE : WS_IDLE;
      }
      break;
    case REQ_PARAM_READ:
      this->handle_param_read_(ok, status, d, n);
      break;
    case REQ_SAVE_FLASH:
      ESP_LOGI(TAG, "Device %u save to flash %s (status %u)", this->write_job_.address, ok ? "done" : "TIMED OUT",
               status);
      this->write_stage_ = WS_IDLE;
      break;
    case REQ_SCAN_INFO:
    case REQ_SCAN_COUNT:
    case REQ_SCAN_BLOCK:
      this->handle_scan_(ok, status, d, n);
      break;
    case REQ_SERVER_RAW:
    case REQ_SERVER_MVO:
    case REQ_SERVER_SYS:
    case REQ_SERVER_COUNT:
      this->server_finish_(ok, status, d, n);
      break;
    case REQ_BRIDGE_ICOMM:
      // Rebuild the controller's reply frame for the bridge client (no answer = it times out, as on the bus).
      if (ok && this->client_fd_ >= 0 && n <= MAX_FRAME_DATA) {
        uint8_t *f = this->tx_buf_;
        f[0] = 0xFF;
        f[1] = 0xFF;
        f[2] = 0x22;
        f[3] = 0xDD;
        uint8_t *hdr = f + 4;
        hdr[0] = this->bridge_seq_;
        hdr[1] = 0xFF;
        hdr[2] = 0x00;
        hdr[3] = this->current_.dst;
        hdr[4] = this->current_.drv;
        hdr[5] = 0x00;
        hdr[6] = status;
        hdr[7] = n & 0xFF;
        hdr[8] = n >> 8;
        uint16_t c = crc16_arc(hdr, 9);
        f[13] = c & 0xFF;
        f[14] = c >> 8;
        if (n > 0)
          memcpy(f + 15, d, n);
        c = crc16_arc(d, n);
        f[15 + n] = c & 0xFF;
        f[16 + n] = c >> 8;
        f[17 + n] = 0xFF;
        f[18 + n] = 0xFF;
        ::send(this->client_fd_, f, 19 + n, MSG_DONTWAIT);
      }
      break;
    default:
      break;
  }
}

void GenesisBus::loop() {
  const uint32_t now = App.get_loop_component_start_time();

  uint8_t b;
  while (this->available() && this->read_byte(&b)) {
    if (this->bus_parser_.feed(b))
      this->handle_frame_();
  }

  if (this->busy_) {
    uint32_t timeout = this->current_.drv == DRV_FLASH ? SAVE_FLASH_TIMEOUT_MS : this->reply_timeout_;
    if (this->via_icomm_())
      timeout = std::max(timeout, IC_RAW_TIMEOUT_MS);  // the server queues it behind its own polling
    else if (this->current_.kind == REQ_SERVER_RAW)
      timeout = std::max(timeout, SERVER_RAW_TIMEOUT_MS);  // config-file reads and logons can be slow
    if ((int32_t) (now - this->sent_at_) > (int32_t) timeout) {
      this->timeouts_++;
      ESP_LOGD(TAG, "Timeout dst=%u drv=%02X cmd=%02X", this->current_.dst, this->current_.drv, this->current_.cmd);
      this->finish_(false, 0xFF, nullptr, 0);
    }
  }

  if (this->tcp_port_ != 0)
    this->tcp_loop_(now);
  if (this->server_port_ != 0)
    this->server_loop_(now);

  if (this->active_icomm_) {
    this->icomm_loop_(now);
    // Auto mode: every 10 minutes ask the serial bus again; a live controller switches us back.
    if (this->transport_ == TRANSPORT_AUTO && (int32_t) (now - this->serial_probe_at_) > (int32_t) SERIAL_PROBE_MS) {
      this->serial_probe_at_ = now;
      for (auto &d : this->devices_)
        this->check_status(d.address);
    }
    this->send_next_(now);
    return;
  }

  // Auto mode: no controller answered on the serial bus after the first round -> use iComm instead.
  if (this->transport_ == TRANSPORT_AUTO && !this->icomm_hosts_.empty() && now > AUTO_SWITCH_AFTER_MS &&
      !this->any_alive()) {
    bool all_checked = !this->devices_.empty();
    for (const auto &d : this->devices_)
      all_checked &= d.checked;
    if (all_checked) {
      ESP_LOGW(TAG, "No controller answers on the serial bus, switching to iComm TCP");
      this->switch_transport_(true);
      return;
    }
  }

  // Status checks: every status_interval for each device (the only traffic dead devices get).
  for (auto &d : this->devices_) {
    if (d.checked && (int32_t) (now - d.last_status) > (int32_t) this->status_interval_)
      this->check_status(d.address);
  }

  if (this->polling_ && (int32_t) (now - this->last_poll_) >= (int32_t) this->update_interval_ &&
      this->queue_.size() < 4) {
    this->last_poll_ = now;
    this->poll_outputs_();
  }
  if (this->polling_ && (int32_t) (now - this->last_param_poll_) >= (int32_t) PARAM_POLL_MS &&
      this->queue_.size() < 4) {
    this->last_param_poll_ = now;
    this->poll_params_();
  }

  this->send_next_(now);
}

void GenesisBus::check_status(uint8_t address) {
  if (auto *d = this->find_device_(address))
    d->last_status = App.get_loop_component_start_time();
  this->enqueue_(address, DRV_STATUS, CMD_SYSTEM_READ, REQ_STATUS);
}

void GenesisBus::handle_status_(bool ok, uint8_t status, const uint8_t *d, size_t n) {
  const uint8_t address = this->current_.dst;
  const bool valid = ok && status == 0 && n > 0xBB;
  const bool running = valid && (d[0xAE] & 0x01) != 0;
  if (this->active_icomm_) {
    // Serial probe while on iComm (auto mode only): any running controller means the bus is back.
    if (running && this->transport_ == TRANSPORT_AUTO) {
      ESP_LOGW(TAG, "Controller %u answers on the serial bus, switching back from iComm", address);
      this->switch_transport_(false);
    }
    return;
  }
  auto *dev = this->find_device_(address);
  if (dev == nullptr)
    return;
  const bool first = !dev->checked;
  const bool was = dev->alive;
  if (valid) {
    memcpy(dev->name, d, 20);
    dev->name[20] = '\0';
    dev->firmware = d[0xBB] + d[0xBA] / 100.0f;
    memcpy(&dev->config_key, d + 0x31, 4);
    dev->fw_lo = d[0xBA];
    dev->fw_hi = d[0xBB];
    dev->sys_valid = true;
  }
  if (first || running != was)
    ESP_LOGI(TAG, "Device %u '%s' %s (fw %.2f)", address, dev->name, running ? "ALIVE" : "dead", dev->firmware);
  this->set_device_alive_(address, running);
#ifdef USE_BINARY_SENSOR
  if (first && dev->online_sensor != nullptr)
    dev->online_sensor->publish_state(running);  // set_device_alive_ only publishes changes
#endif
  if (running && !was)
    this->last_poll_ = 0;  // poll straight away
}

void GenesisBus::set_device_alive_(uint8_t address, bool alive) {
  auto *dev = this->find_device_(address);
  if (dev == nullptr)
    return;
  dev->checked = true;
  if (alive)
    dev->seen = true;
  if (dev->alive == alive)
    return;
  dev->alive = alive;
  ESP_LOGI(TAG, "Device %u %s (via %s)", address, alive ? "ALIVE" : "dead", this->active_icomm_ ? "iComm" : "serial");
#ifdef USE_BINARY_SENSOR
  if (dev->online_sensor != nullptr)
    dev->online_sensor->publish_state(alive);
#endif
}

void GenesisBus::publish_point_(Point &p, float value, bool valid) {
#ifdef USE_SENSOR
  if (p.sensor != nullptr)
    p.sensor->publish_state(valid ? value : NAN);
#endif
#ifdef USE_BINARY_SENSOR
  if (p.binary != nullptr && valid)
    p.binary->publish_state(value != 0.0f);
#endif
#ifdef USE_NUMBER
  if (p.number != nullptr && valid)
    p.number->publish_state(value);
#endif
#ifdef USE_CLIMATE
  if (p.climate != nullptr)
    p.climate->on_point(p.climate_role, value, valid);
#endif
}

void GenesisBus::poll_outputs_() {
  // One MVO request per live device and max_points_ unique (block, output) pairs.
  uint8_t buf[REQ_PAYLOAD];
  for (const auto &dev : this->devices_) {
    if (dev.checked && !dev.alive)
      continue;
    uint8_t cnt = 0;
    for (const auto &p : this->points_) {
      if (p.address != dev.address || p.param != PARAM_NONE)
        continue;
      bool dup = false;
      for (uint8_t k = 0; k < cnt && !dup; k++)
        dup = buf[1 + 2 * k] == p.block && buf[2 + 2 * k] == p.output;
      if (dup)
        continue;
      buf[1 + 2 * cnt] = p.block;
      buf[2 + 2 * cnt] = p.output;
      if (++cnt == this->max_points_) {
        buf[0] = cnt;
        if (!this->enqueue_(dev.address, DRV_BLOCK, CMD_READ_OUTPUTS, REQ_MVO, buf, 1 + 2 * cnt))
          return;
        cnt = 0;
      }
    }
    if (cnt > 0) {
      buf[0] = cnt;
      if (!this->enqueue_(dev.address, DRV_BLOCK, CMD_READ_OUTPUTS, REQ_MVO, buf, 1 + 2 * cnt))
        return;
    }
  }
}

void GenesisBus::handle_mvo_(bool ok, uint8_t status, const uint8_t *d, size_t n) {
  const uint8_t address = this->current_.dst;
  const uint8_t cnt = this->current_.payload[0];
  if (!ok)
    return;
  if (status != 0 || n < 1u + 4u * cnt || d[0] != cnt) {
    ESP_LOGW(TAG, "Device %u output read failed (status %u, %u bytes)", address, status, (unsigned) n);
    return;
  }
  this->set_device_alive_(address, true);
  for (uint8_t i = 0; i < cnt; i++) {
    const uint8_t block = this->current_.payload[1 + 2 * i];
    const uint8_t out = this->current_.payload[2 + 2 * i];
    uint32_t raw;
    memcpy(&raw, &d[1 + 4 * i], 4);
    float v;
    memcpy(&v, &raw, 4);
    const bool valid = raw != 0xFFFFFFFFu && std::isfinite(v);
    for (auto &p : this->points_)
      if (p.address == address && p.block == block && p.output == out)
        this->publish_point_(p, v, valid);
  }
}

void GenesisBus::poll_params_() {
  // Parameter points live in the block record: one 2/5 read per unique (device, block).
  for (size_t i = 0; i < this->points_.size(); i++) {
    const Point &p = this->points_[i];
    if (p.param == PARAM_NONE)
      continue;
    const DeviceState *dev = this->find_device_(p.address);
    if (dev == nullptr || (dev->checked && !dev->alive))
      continue;
    bool dup = false;
    for (size_t k = 0; k < i && !dup; k++)
      dup = this->points_[k].param != PARAM_NONE && this->points_[k].address == p.address &&
            this->points_[k].block == p.block;
    if (!dup && !this->enqueue_(p.address, DRV_BLOCK, CMD_READ_BLOCK, REQ_PARAM_READ, &p.block, 1))
      return;
  }
}

void GenesisBus::handle_param_read_(bool ok, uint8_t status, const uint8_t *d, size_t n) {
  const uint8_t address = this->current_.dst;
  const uint8_t block = this->current_.payload[0];
  if (!ok || status != 0 || n < 0x1E)
    return;
  if (d[0x15] != BLOCK_TYPE_DIGITAL_OUTPUT) {
    ESP_LOGW(TAG, "Block %u/%u: type 0x%02X is not a Digital Output, can't read its override", address, block,
             d[0x15]);
    return;
  }
  const size_t base = 0x1E + 6u * d[0x1A] + 4u * d[0x1B];
  if (base + DO_FORCE_VALUE_OFFSET >= n)
    return;
  for (auto &p : this->points_) {
    if (p.address != address || p.block != block || p.param == PARAM_NONE)
      continue;
    const size_t off = p.param == PARAM_FORCED ? DO_FORCED_OFFSET : DO_FORCE_VALUE_OFFSET;
    this->publish_point_(p, d[base + off] != 0 ? 1.0f : 0.0f, true);
  }
}

void GenesisBus::write_user_variable(uint8_t address, uint8_t block, float value) {
  // Same read-patch-write(-save) sequence on serial and iComm (raw passthrough), so only the bytes we mean
  // change. iComm's own field writes rewrite the whole record from iComm's cached copy.
  WriteJob j;
  j.address = address;
  j.block = block;
  j.kind = WJ_USER_VALUE;
  j.value = value;
  if (!this->writes_.push(j))
    ESP_LOGW(TAG, "Write %u/%u dropped: too many pending writes", address, block);
}

void GenesisBus::force_output(uint8_t address, uint8_t block, ForceMode mode) {
  if (mode == FORCE_ON)
    ESP_LOGW(TAG, "Output %u/%u: forcing ON (overrides schedule, switch and fire-alarm logic)", address, block);
  else
    ESP_LOGI(TAG, "Output %u/%u: %s", address, block, mode == FORCE_OFF ? "forcing OFF" : "releasing override");
  WriteJob j;
  j.address = address;
  j.block = block;
  j.kind = WJ_FORCE;
  j.force = mode;
  if (!this->writes_.push(j))
    ESP_LOGW(TAG, "Override %u/%u dropped: too many pending writes", address, block);
}

void GenesisBus::handle_write_read_(bool ok, uint8_t status, const uint8_t *d, size_t n) {
  const WriteJob &j = this->write_job_;
  if (!ok || status != 0 || n < 0x22 || n > MAX_FRAME_DATA) {
    ESP_LOGW(TAG, "Write %u/%u: block read failed (ok=%d status=%u len=%u)", j.address, j.block, ok, status,
             (unsigned) n);
    this->write_stage_ = WS_IDLE;
    return;
  }
  if (j.kind == WJ_FORCE) {
    if (d[0x15] != BLOCK_TYPE_DIGITAL_OUTPUT) {
      ESP_LOGE(TAG, "Override %u/%u: block type 0x%02X is not a Digital Output, refusing", j.address, j.block,
               d[0x15]);
      this->write_stage_ = WS_IDLE;
      return;
    }
    const size_t base = 0x1E + 6u * d[0x1A] + 4u * d[0x1B];
    if (base + DO_FORCE_VALUE_OFFSET >= n) {
      ESP_LOGE(TAG, "Override %u/%u: record too short (%u)", j.address, j.block, (unsigned) n);
      this->write_stage_ = WS_IDLE;
      return;
    }
    this->write_buf_[0] = j.block;
    memcpy(this->write_buf_ + 1, d, n);
    this->write_len_ = n + 1;
    uint8_t *rec = this->write_buf_ + 1;
    const uint8_t forced = j.force == FORCE_RELEASE ? 0 : 1;
    const uint8_t value = j.force == FORCE_ON ? 1 : 0;
    ESP_LOGI(TAG, "Override %u/%u '%.20s': Forced %u -> %u, ForceValue %u -> %u", j.address, j.block,
             (const char *) rec, rec[base + DO_FORCED_OFFSET], forced, rec[base + DO_FORCE_VALUE_OFFSET], value);
    rec[base + DO_FORCE_VALUE_OFFSET] = value;
    rec[base + DO_FORCED_OFFSET] = forced;
    this->write_stage_ = WS_WRITE;
    return;
  }
  if (d[0x15] != BLOCK_TYPE_USER_VARIABLE) {
    ESP_LOGE(TAG, "Write %u/%u: block type 0x%02X is not a User Variable, refusing", j.address, j.block, d[0x15]);
    this->write_stage_ = WS_IDLE;
    return;
  }
  const size_t base = 0x1E + 6u * d[0x1A] + 4u * d[0x1B];
  if (base + 13 > n) {
    ESP_LOGE(TAG, "Write %u/%u: record too short (%u)", j.address, j.block, (unsigned) n);
    this->write_stage_ = WS_IDLE;
    return;
  }
  // Payload for 2/6 = block seq + the whole record with the Value patched.
  this->write_buf_[0] = j.block;
  memcpy(this->write_buf_ + 1, d, n);
  this->write_len_ = n + 1;
  uint8_t *rec = this->write_buf_ + 1;
  float old_v, min_v, max_v;
  memcpy(&old_v, rec + base + 1, 4);
  memcpy(&min_v, rec + base + 5, 4);
  memcpy(&max_v, rec + base + 9, 4);
  float v = j.value;
  if (min_v < max_v && (v < min_v || v > max_v)) {
    ESP_LOGW(TAG, "Write %u/%u: %.2f outside %.2f..%.2f, clamping", j.address, j.block, v, min_v, max_v);
    v = std::max(min_v, std::min(max_v, v));
  }
  memcpy(rec + base + 1, &v, 4);
  ESP_LOGI(TAG, "Write %u/%u '%.20s': %.2f -> %.2f", j.address, j.block, (const char *) rec, old_v, v);
  this->write_stage_ = WS_WRITE;
}

void GenesisBus::scan_blocks(uint8_t address) {
  if (this->scan_address_ != 0) {
    ESP_LOGW(TAG, "A block scan of device %u is already running", this->scan_address_);
    return;
  }
  this->scan_address_ = address;
  this->scan_count_ = 0;
  this->scan_next_ = 1;
  this->scan_waiting_ = false;
  this->enqueue_(address, DRV_SYSTEM, CMD_DEVICEINFO, REQ_SCAN_INFO);
  if (!this->enqueue_(address, DRV_BLOCK, CMD_BLOCK_COUNT, REQ_SCAN_COUNT))
    this->scan_address_ = 0;
}

void GenesisBus::handle_scan_(bool ok, uint8_t status, const uint8_t *d, size_t n) {
  const uint8_t address = this->current_.dst;
  switch (this->current_.kind) {
    case REQ_SCAN_INFO: {
      if (!ok || n < 4) {
        ESP_LOGW(TAG, "Device %u DEVICEINFO failed", address);
        break;
      }
      char name[24] = {0};
      if (n > 12)
        memcpy(name, d + 12, std::min<size_t>(n - 12, sizeof(name) - 1));
      ESP_LOGI(TAG, "Device %u DEVICEINFO type=%u name='%s'", address, d[2] | (d[3] << 8), name);
      break;
    }
    case REQ_SCAN_COUNT:
      if (!ok || status != 0 || n < 1) {
        ESP_LOGW(TAG, "Device %u block count failed", address);
        this->scan_address_ = 0;
        break;
      }
      this->scan_count_ = d[0];
      this->scan_next_ = 1;
      ESP_LOGI(TAG, "Device %u has %u blocks:", address, this->scan_count_);
      if (this->scan_count_ == 0)
        this->scan_address_ = 0;
      break;
    default: {  // REQ_SCAN_BLOCK
      const uint16_t seq = this->scan_next_;
      if (!ok || status != 0 || n < 0x16) {
        ESP_LOGW(TAG, "  %u/%u: read failed", address, seq);
      } else {
        char name[21] = {0};
        memcpy(name, d, 20);
        if (d[0x15] == BLOCK_TYPE_USER_VARIABLE && n >= 0x22) {
          const size_t base = 0x1E + 6u * d[0x1A] + 4u * d[0x1B];
          float v = NAN;
          if (base + 5 <= n)
            memcpy(&v, d + base + 1, 4);
          ESP_LOGI(TAG, "  %u/%-3u type 0x%02X %-20s value=%.2f", address, seq, d[0x15], name, v);
        } else {
          ESP_LOGI(TAG, "  %u/%-3u type 0x%02X %-20s", address, seq, d[0x15], name);
        }
      }
      this->scan_waiting_ = false;
      if (++this->scan_next_ > this->scan_count_) {
        ESP_LOGI(TAG, "Device %u scan done", address);
        this->scan_address_ = 0;
      }
      break;
    }
  }
}

// ------------------------------------------------------------------ transport switching

void GenesisBus::switch_transport_(bool icomm) {
  if (icomm == this->active_icomm_)
    return;
  this->active_icomm_ = icomm;
  for (auto &d : this->devices_) {
    d.alive = false;
    d.checked = false;
    d.seen = false;
    d.failures = 0;
#ifdef USE_BINARY_SENSOR
    if (d.online_sensor != nullptr)
      d.online_sensor->publish_state(false);
#endif
  }
  for (auto &p : this->points_) {
    p.field_idx = p.base_field;
    p.field_failed = false;
    p.readvise = false;
  }
  const uint32_t now = App.get_loop_component_start_time();
  if (icomm) {
    this->serial_probe_at_ = now;
    this->icomm_last_try_ = 0;
  } else {
    this->icomm_close_();
    for (auto &d : this->devices_)
      this->check_status(d.address);
    this->last_poll_ = 0;
  }
}

// ------------------------------------------------------------------ iComm TCP client

void GenesisBus::icomm_close_() {
  if (this->icomm_fd_ >= 0)
    ::close(this->icomm_fd_);
  const bool was_ready = this->icomm_ready_;
  this->icomm_fd_ = -1;
  this->icomm_connecting_ = false;
  this->icomm_ready_ = false;
  this->ic_rx_len_ = 0;
  if (!was_ready)
    return;
  ESP_LOGW(TAG, "iComm connection closed");
  // Whatever closed it (peer, send error, silence, framing), the values are no longer live.
  if (this->active_icomm_) {
    for (auto &d : this->devices_)
      this->set_device_alive_(d.address, false);
  }
  // A raw request in flight will never be answered now; fail it instead of waiting out its timeout.
  if (this->busy_ && this->via_icomm_())
    this->finish_(false, 0xFF, nullptr, 0);
}

void GenesisBus::icomm_begin_(uint16_t type) {
  const uint32_t invoke = this->icomm_invoke_++;
  uint8_t *m = this->ic_tx_;
  m[0] = 0x78;
  m[1] = 0x56;
  m[2] = 0;  // length, filled in by icomm_send_()
  m[3] = 0;
  m[4] = type & 0xFF;
  m[5] = type >> 8;
  m[6] = invoke & 0xFF;
  m[7] = (invoke >> 8) & 0xFF;
  m[8] = (invoke >> 16) & 0xFF;
  m[9] = invoke >> 24;
  this->ic_tx_len_ = 10;
}

void GenesisBus::icomm_put_(const void *data, size_t len) {
  if (this->ic_tx_len_ + len > IC_TX_BUF) {
    ESP_LOGE(TAG, "iComm message too large");
    return;
  }
  memcpy(this->ic_tx_ + this->ic_tx_len_, data, len);
  this->ic_tx_len_ += len;
}

void GenesisBus::icomm_put_point_(uint8_t address, uint8_t block, const char *field) {
  char dev[16] = {0};
  char pt[32] = {0};
  snprintf(dev, sizeof(dev), "%u:%u", this->icomm_conn_, address);
  snprintf(pt, sizeof(pt), "@%u~%s", block, field);
  this->icomm_put_(dev, sizeof(dev));
  this->icomm_put_(pt, sizeof(pt));
}

void GenesisBus::icomm_send_() {
  if (this->icomm_fd_ < 0)
    return;
  const uint16_t len = this->ic_tx_len_ - 4;
  this->ic_tx_[2] = len & 0xFF;
  this->ic_tx_[3] = len >> 8;
  // Non-blocking socket: a message can be accepted in pieces when the send buffer is full (slow or high-latency
  // link). A half-sent message would break the framing, so wait for buffer space, up to IC_SEND_WAIT_MS.
  size_t sent = 0;
  const uint32_t start = millis();
  while (sent < this->ic_tx_len_) {
    ssize_t r = ::send(this->icomm_fd_, this->ic_tx_ + sent, this->ic_tx_len_ - sent, MSG_DONTWAIT);
    if (r > 0) {
      sent += r;
      continue;
    }
    if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
      break;
    const uint32_t waited = millis() - start;
    if (waited >= IC_SEND_WAIT_MS)
      break;
    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(this->icomm_fd_, &wfds);
    struct timeval tv {
      0, (suseconds_t) (std::min<uint32_t>(IC_SEND_WAIT_MS - waited, 50) * 1000)
    };
    ::select(this->icomm_fd_ + 1, nullptr, &wfds, nullptr, &tv);
  }
  if (sent != this->ic_tx_len_) {
    ESP_LOGW(TAG, "iComm send failed (%u of %u bytes)", (unsigned) sent, (unsigned) this->ic_tx_len_);
    this->icomm_close_();
  }
}

void GenesisBus::icomm_advise_(size_t first, size_t count) {
  // Advise: i16 count, then 64-byte records: u32 handle, char[16] "conn:dev", char[32] "@blk~field",
  // u32 min interval, f64 deadband. iComm answers with a type-1 reply, then pushes type-3 updates.
  this->icomm_begin_(IC_ADVISE);
  const uint16_t cnt = count;
  this->icomm_put_(&cnt, 2);
  for (size_t i = first; i < first + count; i++) {
    const Point &p = this->points_[i];
    const uint32_t h = IC_HANDLE_BASE + i * IC_HANDLE_STRIDE + p.field_idx;
    const uint32_t interval = 0;
    const double deadband = 0.005;
    this->icomm_put_(&h, 4);
    this->icomm_put_point_(p.address, p.block, IC_FIELDS[p.field_idx]);
    this->icomm_put_(&interval, 4);
    this->icomm_put_(&deadband, 8);
  }
  this->icomm_send_();
}

void GenesisBus::icomm_raw_(uint8_t dst, uint8_t drv, uint8_t cmd, const uint8_t *payload, size_t len,
                            uint32_t now) {
  // iComm message type 7 (as Gen2Xtract sends it): u32 invoke, then a 32-byte struct
  //   [0..3] FFFFFFFF  [4..5] connection id  [6..7] session (0 = not logged on)  [8..9] device address as ASCII
  //   [10..13] u32 1  [25] flag 03  [26] cmd u16  [28] drv u16  [30] payload length u16
  // followed by the payload (at least one byte). The reply (type 8) carries the controller's reply data.
  this->busy_ = true;
  this->sent_at_ = now;
  this->last_activity_ = now;
  if (!this->icomm_ready_) {
    ESP_LOGW(TAG, "Request %02X/%02X to %u: iComm not connected", drv, cmd, dst);
    this->finish_(false, 0xFF, nullptr, 0);
    return;
  }
  if (dst > 99)
    ESP_LOGW(TAG, "Device %u: iComm raw requests carry the address as 2 ASCII digits", dst);
  uint8_t head[32] = {0};
  head[0] = head[1] = head[2] = head[3] = 0xFF;
  head[4] = this->icomm_conn_;
  if (dst >= 10) {
    head[8] = '0' + (dst / 10) % 10;
    head[9] = '0' + dst % 10;
  } else {
    head[8] = '0' + dst;
  }
  head[10] = 1;
  head[25] = this->current_.flag != 0 ? this->current_.flag : 0x03;
  head[26] = cmd;
  head[28] = drv;
  head[30] = len & 0xFF;
  head[31] = len >> 8;
  this->icomm_raw_invoke_ = this->icomm_invoke_;  // icomm_begin_ takes this invoke id
  this->icomm_begin_(IC_RAW_REQUEST);
  this->icomm_put_(head, sizeof(head));
  const uint8_t pad = 0;
  this->icomm_put_(len > 0 ? payload : &pad, len > 0 ? len : 1);
  ESP_LOGV(TAG, "iComm raw dst=%u drv=%02X cmd=%02X len=%u", dst, drv, cmd, (unsigned) len);
  this->icomm_send_();  // on a send error icomm_close_() fails this request
}

void GenesisBus::icomm_loop_(uint32_t now) {
  if (this->icomm_fd_ < 0) {
    if (this->icomm_hosts_.empty() || !network::is_connected())
      return;
    if (this->icomm_last_try_ != 0 && (int32_t) (now - this->icomm_last_try_) < (int32_t) IC_RECONNECT_MS)
      return;
    this->icomm_last_try_ = now;
    const char *host = this->icomm_hosts_[this->icomm_host_idx_++ % this->icomm_hosts_.size()];
    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    const uint16_t port = this->icomm_port_for_(host);
    addr.sin_port = htons(port);
    if (inet_aton(host, &addr.sin_addr) == 0) {
      ESP_LOGE(TAG, "iComm host '%s' must be an IP address", host);
      return;
    }
    this->icomm_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (this->icomm_fd_ < 0)
      return;
    ::fcntl(this->icomm_fd_, F_SETFL, ::fcntl(this->icomm_fd_, F_GETFL, 0) | O_NONBLOCK);
    int one = 1;
    ::setsockopt(this->icomm_fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    ::connect(this->icomm_fd_, (struct sockaddr *) &addr, sizeof(addr));
    this->icomm_connecting_ = true;
    this->icomm_connect_started_ = now;
    ESP_LOGD(TAG, "Connecting to iComm %s:%u", host, port);
    return;
  }

  if (this->icomm_connecting_) {
    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(this->icomm_fd_, &wfds);
    struct timeval tv {
      0, 0
    };
    if (::select(this->icomm_fd_ + 1, nullptr, &wfds, nullptr, &tv) <= 0) {
      if ((int32_t) (now - this->icomm_connect_started_) > (int32_t) IC_CONNECT_TIMEOUT_MS) {
        ESP_LOGW(TAG, "iComm connect timed out");
        this->icomm_close_();
      }
      return;
    }
    int err = 0;
    socklen_t elen = sizeof(err);
    ::getsockopt(this->icomm_fd_, SOL_SOCKET, SO_ERROR, &err, &elen);
    if (err != 0) {
      ESP_LOGW(TAG, "iComm connect failed (%d)", err);
      this->icomm_close_();
      return;
    }
    this->icomm_connecting_ = false;
    this->icomm_ready_ = true;
    this->icomm_last_rx_ = now;
    ESP_LOGI(TAG, "Connected to iComm, subscribing to %u points", (unsigned) this->points_.size());
    // Hello: u16 protocol version 10, char[32] application, char[32] host (same shape as Supervisor's).
    // Without it iComm 2.38 ignores advise requests.
    this->icomm_begin_(IC_HELLO);
    const uint16_t version = 10;
    // iComm keeps all 64 bytes as the application name and its Current Applications page prints NULs as boxes
    // (Supervisor's row shows dozens), so both fields are space-padded with no terminator; the page collapses
    // the spaces. Name = "ESPHome <friendly name>".
    char name[32];
    char host[32];
    memset(name, ' ', sizeof(name));
    memset(host, ' ', sizeof(host));
    char tmp[sizeof(name) + 1];
    int n;
    if (this->icomm_name_ != nullptr) {
      n = snprintf(tmp, sizeof(tmp), "%s", this->icomm_name_);
    } else {
      uint8_t mac[6];
      get_mac_address_raw(mac);
      n = snprintf(tmp, sizeof(tmp), "ESPHome %s %02X%02X", App.get_friendly_name().c_str(), mac[4], mac[5]);
    }
    memcpy(name, tmp, std::min<size_t>(n > 0 ? n : 0, sizeof(name)));
    this->icomm_put_(&version, 2);
    this->icomm_put_(name, sizeof(name));
    this->icomm_put_(host, sizeof(host));
    this->icomm_send_();
    this->icomm_advise_next_ = 0;  // subscriptions go out one batch at a time (below), not all at once
    this->icomm_advise_waiting_ = false;
    for (auto &p : this->points_)
      p.readvise = false;  // the batches cover every point with its current field name
    return;
  }


  for (int guard = 0; guard < 8; guard++) {
    const size_t space = IC_RX_BUF - this->ic_rx_len_;
    if (space == 0)
      break;
    ssize_t n = ::recv(this->icomm_fd_, this->ic_rx_ + this->ic_rx_len_, space, MSG_DONTWAIT);
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
      this->icomm_close_();
      return;
    }
    if (n < 0)
      break;
    this->icomm_last_rx_ = now;
    this->ic_rx_len_ += n;
  }
  while (this->ic_rx_len_ >= 4) {
    const size_t len = this->ic_rx_[2] | (this->ic_rx_[3] << 8);
    if (this->ic_rx_[0] != 0x78 || this->ic_rx_[1] != 0x56 || 4 + len > IC_RX_BUF) {
      ESP_LOGW(TAG, "iComm framing lost");
      this->icomm_close_();
      return;
    }
    if (this->ic_rx_len_ < 4 + len)
      break;
    this->icomm_handle_(this->ic_rx_ + 4, len);
    if (this->icomm_fd_ < 0)
      return;
    this->ic_rx_len_ -= 4 + len;
    memmove(this->ic_rx_, this->ic_rx_ + 4 + len, this->ic_rx_len_);
  }
  // Subscribe in batches after draining what iComm sent, and only once iComm has answered the previous batch.
  // Sending them all at once deadlocks on a slow link: iComm floods initial values, both buffers fill, and
  // neither side reads.
  if (this->icomm_advise_next_ < this->points_.size() &&
      (!this->icomm_advise_waiting_ || (int32_t) (now - this->icomm_advise_sent_at_) > (int32_t) IC_ADVISE_REPLY_MS)) {
    const size_t first = this->icomm_advise_next_;
    this->icomm_advise_next_ += IC_ADVISE_BATCH;
    this->icomm_advise_waiting_ = true;
    this->icomm_advise_sent_at_ = now;
    this->icomm_advise_(first, std::min(IC_ADVISE_BATCH, this->points_.size() - first));
    if (this->icomm_fd_ < 0)
      return;
  }
  // Points whose field name was rejected get re-subscribed with the next candidate.
  for (size_t i = 0; i < this->points_.size() && this->icomm_fd_ >= 0; i++) {
    if (this->points_[i].readvise) {
      this->points_[i].readvise = false;
      this->icomm_advise_(i, 1);
    }
  }
  if ((int32_t) (now - this->icomm_last_rx_) > (int32_t) IC_SILENCE_MS) {
    ESP_LOGW(TAG, "iComm silent for %u s, reconnecting", (unsigned) (IC_SILENCE_MS / 1000));
    this->icomm_close_();
  }
}

void GenesisBus::icomm_handle_(const uint8_t *m, size_t len) {
  if (len < 6)
    return;
  const uint16_t type = m[0] | (m[1] << 8);
  if (type == IC_RAW_REPLY && len >= 10) {
    // u32 invoke | u16 result (1 = OK) | u16 data length | controller reply data
    const uint32_t inv = m[2] | (m[3] << 8) | (m[4] << 16) | ((uint32_t) m[5] << 24);
    const uint16_t result = m[6] | (m[7] << 8);
    const size_t dlen = std::min<size_t>(m[8] | (m[9] << 8), len - 10);
    if (this->busy_ && this->via_icomm_() && inv == this->icomm_raw_invoke_) {
      this->last_activity_ = App.get_loop_component_start_time();
      if (result != IC_STATUS_OK)
        ESP_LOGW(TAG, "iComm raw request %02X/%02X to %u failed (0x%04X)", this->current_.drv, this->current_.cmd,
                 this->current_.dst, result);
      this->finish_(result == IC_STATUS_OK, result == IC_STATUS_OK ? 0 : 0xFF, m + 10, dlen);
    }
    return;
  }
  if (type == IC_REPLY && len >= 10) {
    const uint16_t req = m[6] | (m[7] << 8);
    const uint16_t result = m[8] | (m[9] << 8);
    if (req == IC_ADVISE)
      this->icomm_advise_waiting_ = false;
    if (req == IC_RAW_REQUEST) {
      // A raw request iComm can't send (e.g. 0xF447 for a controller it has marked dead) is answered with a
      // type-1 error instead of a type-8 reply. Fail it now rather than waiting out the timeout.
      const uint32_t inv = m[2] | (m[3] << 8) | (m[4] << 16) | ((uint32_t) m[5] << 24);
      if (this->busy_ && this->via_icomm_() && inv == this->icomm_raw_invoke_) {
        ESP_LOGD(TAG, "iComm refused raw request %02X/%02X to %u (0x%04X)", this->current_.drv, this->current_.cmd,
                 this->current_.dst, result);
        // iComm only refuses like this for a controller it has marked dead: remember it, so the next device
        // summary for it is answered straight away instead of waiting on iComm again.
        if (result == IC_STATUS_RAW_DEAD)
          this->set_device_alive_(this->current_.dst, false);
        this->finish_(false, 0xFF, nullptr, 0);
      }
      return;
    }
    if (req == IC_WRITE)
      ESP_LOGI(TAG, "iComm write %s (0x%04X)", result == IC_STATUS_OK ? "OK" : "FAILED", result);
    else if (result != IC_STATUS_OK)
      ESP_LOGW(TAG, "iComm request type %u failed (0x%04X)", req, result);
    return;
  }
  if (type != IC_UPDATE || len < 8)
    return;
  const uint16_t count = m[6] | (m[7] << 8);
  size_t o = 8;
  for (uint16_t r = 0; r < count && o + 7 <= len; r++) {
    const uint32_t h = m[o] | (m[o + 1] << 8) | (m[o + 2] << 16) | ((uint32_t) m[o + 3] << 24);
    const uint16_t status = m[o + 4] | (m[o + 5] << 8);
    const uint8_t dtype = m[o + 6];
    o += 7;
    double v = NAN;
    size_t vlen = 0;
    switch (dtype) {
      case 1:  // u16 length + bytes
      case 6:  // u16 length + chars + NUL
        vlen = o + 2 <= len ? 2 + (m[o] | (m[o + 1] << 8)) + (dtype == 6 ? 1 : 0) : len;
        break;
      case 2:
        vlen = 8;
        if (o + 8 <= len)
          memcpy(&v, &m[o], 8);
        break;
      case 3:
        vlen = 2;
        if (o + 2 <= len)
          v = m[o] | (m[o + 1] << 8);
        break;
      case 4:
        vlen = 4;
        if (o + 4 <= len)
          v = (double) (m[o] | (m[o + 1] << 8) | (m[o + 2] << 16) | ((uint32_t) m[o + 3] << 24));
        break;
      case 5:
        vlen = 1;
        if (o < len)
          v = m[o];
        break;
      case 0:  // no value (error statuses such as "bad field" arrive like this)
        break;
      default:
        // Unknown variant: its length is unknown, so the rest of this batch can't be parsed.
        ESP_LOGD(TAG, "iComm update with unknown value type %u, skipping the rest of the batch", dtype);
        return;
    }
    o += vlen;
    if (h < IC_HANDLE_BASE)
      continue;
    const size_t idx = (h - IC_HANDLE_BASE) / IC_HANDLE_STRIDE;
    const uint8_t field = (h - IC_HANDLE_BASE) % IC_HANDLE_STRIDE;
    if (idx >= this->points_.size())
      continue;
    Point &p = this->points_[idx];
    if (field != p.field_idx)
      continue;  // stale update for a field name we've moved past
    this->last_activity_ = App.get_loop_component_start_time();
    if (status == IC_STATUS_OK) {
      this->set_device_alive_(p.address, true);
      this->publish_point_(p, (float) v, !std::isnan(v));
    } else if (status == IC_STATUS_BAD_FIELD) {
      if (p.base_field < IC_AUTO_FIELDS && p.field_idx + 1 < IC_AUTO_FIELDS) {
        this->icomm_begin_(IC_UNADVISE);
        const uint16_t one = 1;
        this->icomm_put_(&one, 2);
        this->icomm_put_(&h, 4);
        this->icomm_send_();
        p.field_idx++;
        p.readvise = true;
      } else if (!p.field_failed) {
        p.field_failed = true;
        if (p.base_field >= IC_AUTO_FIELDS)
          ESP_LOGW(TAG, "Point %u/%u: iComm has no field %s (wrong block type?)", p.address, p.block,
                   IC_FIELDS[p.field_idx]);
        else
          ESP_LOGW(TAG, "Point %u/%u: iComm knows none of OUTPUT/Value/State", p.address, p.block);
      }
    } else if (status == IC_STATUS_DEVICE_DEAD) {
      this->set_device_alive_(p.address, false);
      this->publish_point_(p, NAN, false);
    } else if (status != IC_STATUS_NO_VALUE) {
      ESP_LOGD(TAG, "Point %u/%u status 0x%04X", p.address, p.block, status);
      this->publish_point_(p, NAN, false);
    }
  }
}

// ------------------------------------------------------------------ TCP bridge

void GenesisBus::tcp_setup_() {
  this->listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (this->listen_fd_ < 0) {
    ESP_LOGE(TAG, "TCP socket failed");
    return;
  }
  int one = 1;
  ::setsockopt(this->listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in addr {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(this->tcp_port_);
  if (::bind(this->listen_fd_, (struct sockaddr *) &addr, sizeof(addr)) != 0 || ::listen(this->listen_fd_, 1) != 0) {
    ESP_LOGE(TAG, "TCP bind/listen on %u failed", this->tcp_port_);
    ::close(this->listen_fd_);
    this->listen_fd_ = -1;
    return;
  }
  ::fcntl(this->listen_fd_, F_SETFL, ::fcntl(this->listen_fd_, F_GETFL, 0) | O_NONBLOCK);
}

void GenesisBus::tcp_close_client_() {
  if (this->client_fd_ >= 0) {
    ::close(this->client_fd_);
    ESP_LOGI(TAG, "TCP client disconnected");
  }
  this->client_fd_ = -1;
  this->tcp_parser_.reset();
}

void GenesisBus::tcp_loop_(uint32_t now) {
  if (this->listen_fd_ < 0) {
    // socket() before the network stack is up aborts, so open the listener lazily.
    if (!network::is_connected() ||
        (this->listen_last_try_ != 0 && (int32_t) (now - this->listen_last_try_) < (int32_t) LISTEN_RETRY_MS))
      return;
    this->listen_last_try_ = now;
    this->tcp_setup_();
    if (this->listen_fd_ < 0)
      return;
    ESP_LOGI(TAG, "TCP bridge listening on port %u", this->tcp_port_);
  }
  struct sockaddr_in peer {};
  socklen_t plen = sizeof(peer);
  int fd = ::accept(this->listen_fd_, (struct sockaddr *) &peer, &plen);
  if (fd >= 0) {
    // One client at a time (iComm); a new connection replaces the old one.
    this->tcp_close_client_();
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    this->client_fd_ = fd;
    char ip[16];
    inet_ntoa_r(peer.sin_addr, ip, sizeof(ip));
    ESP_LOGI(TAG, "TCP client connected from %s", ip);
  }
  if (this->client_fd_ < 0)
    return;
  uint8_t buf[256];
  ssize_t n = ::recv(this->client_fd_, buf, sizeof(buf), MSG_DONTWAIT);
  if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
    this->tcp_close_client_();
    return;
  }
  for (ssize_t i = 0; i < n; i++) {
    if (!this->tcp_parser_.feed(buf[i]))
      continue;
    if (this->bridge_pending_) {
      ESP_LOGW(TAG, "Bridge client sent a new frame before the last one went out; dropping it");
      continue;
    }
    // Keep the client's sequence number and addressing; just put the frame on the bus as-is.
    uint8_t *f = this->bridge_buf_;
    const size_t body = this->tcp_parser_.body_len();
    f[0] = 0xFF;
    f[1] = 0xFF;
    f[2] = 0xFF;
    f[3] = 0xFF;
    f[4] = 0x22;
    f[5] = 0xDD;
    memcpy(f + 6, this->tcp_parser_.body(), body);
    f[6 + body] = 0xFF;
    f[7 + body] = 0xFF;
    this->bridge_len_ = body + 8;
    this->bridge_seq_ = this->tcp_parser_.hdr()[0];
    this->bridge_dst_ = this->tcp_parser_.hdr()[1];
    this->bridge_drv_ = this->tcp_parser_.hdr()[2];
    this->bridge_flag_ = this->tcp_parser_.hdr()[5];
    this->bridge_cmd_ = this->tcp_parser_.hdr()[6];
    this->bridge_data_len_ = this->tcp_parser_.data_len();
    this->bridge_pending_ = true;
  }
}


// ------------------------------------------------------------------ iComm server (Supervisor -> ESP)
//
// Supervisor talks to an iComm server on TCP 20000. This answers the same messages, so Supervisor can connect to
// the ESP directly: the session / connection / device list handshake, raw bus commands (type 7, used for logon,
// status, config-file chunks and block records), subscriptions (polled on the bus and pushed as type-3 updates),
// System~ConfigKey reads, and User Variable writes. Raw commands and polls go through the same request engine as
// everything else, so this works on the serial bus and when the ESP itself is relaying through an iComm server.

bool GenesisBus::server_client_connected() const {
  for (const auto &c : this->server_clients_)
    if (c.fd >= 0)
      return true;
  return false;
}

// Output number + 1 (as used by MVO 2/9) for a field name; 0 = not a live output we can poll.
static uint8_t server_output_for_field(const char *f) {
  static constexpr struct {
    const char *name;
    uint8_t out;
  } MAP[] = {{"OUTPUT", 1},  {"Value", 1},  {"State", 1},   {"Direct", 1},  {"Running", 1}, {"Maximum", 1},
             {"Output 1", 1}, {"Reverse", 2}, {"Done", 2},     {"Request", 2}, {"Minimum", 2}, {"Output 2", 2},
             {"Time", 3},    {"Fail", 3},   {"Average", 3}, {"Output 3", 3}, {"Output 4", 4}};
  for (const auto &m : MAP)
    if (strcasecmp(f, m.name) == 0)
      return m.out;
  return 0;
}

// "1:4" -> 4 (connection must be ours). Returns 0 when it can't be parsed.
static uint8_t server_parse_device(const uint8_t *p, size_t max, uint8_t conn) {
  char buf[17] = {0};
  memcpy(buf, p, std::min<size_t>(max, 16));
  unsigned c = 0, d = 0;
  if (sscanf(buf, "%u:%u", &c, &d) != 2 || c != conn || d == 0 || d > 254)
    return 0;
  return d;
}

void GenesisBus::server_close_(size_t idx) {
  ServerClient &c = this->server_clients_[idx];
  if (c.fd >= 0) {
    ::close(c.fd);
    ESP_LOGI(TAG, "iComm server: client %u disconnected", (unsigned) idx);
  }
  c.fd = -1;
  c.rx_len = 0;
  c.raw_pending = false;
  c.raw_inflight = false;
  c.update_invoke_set = false;
  c.gen++;
  // drop its subscriptions
  size_t w = 0;
  for (size_t r = 0; r < this->server_subs_.size(); r++)
    if (this->server_subs_[r].client != idx)
      this->server_subs_[w++] = this->server_subs_[r];
  this->server_subs_.resize(w);
}

void GenesisBus::server_send_(size_t idx, uint16_t type, uint32_t invoke, const void *body, size_t len) {
  ServerClient &c = this->server_clients_[idx];
  if (c.fd < 0)
    return;
  if (10 + len > SERVER_TX_BUF) {
    ESP_LOGE(TAG, "iComm server: reply too large (%u)", (unsigned) len);
    return;
  }
  uint8_t *m = this->server_tx_;
  const uint16_t l = 6 + len;
  m[0] = 0x78;
  m[1] = 0x56;
  m[2] = l & 0xFF;
  m[3] = l >> 8;
  m[4] = type & 0xFF;
  m[5] = type >> 8;
  memcpy(m + 6, &invoke, 4);
  if (len > 0)
    memcpy(m + 10, body, len);
  const size_t total = 10 + len;
  size_t sent = 0;
  const uint32_t start = millis();
  while (sent < total) {
    ssize_t r = ::send(c.fd, m + sent, total - sent, MSG_DONTWAIT);
    if (r > 0) {
      sent += r;
      continue;
    }
    if ((r < 0 && errno != EAGAIN && errno != EWOULDBLOCK) || millis() - start > IC_SEND_WAIT_MS)
      break;
    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(c.fd, &wfds);
    struct timeval tv {
      0, 20000
    };
    ::select(c.fd + 1, nullptr, &wfds, nullptr, &tv);
  }
  if (sent != total) {
    ESP_LOGW(TAG, "iComm server: send to client %u failed", (unsigned) idx);
    this->server_close_(idx);
    return;
  }
  c.last_tx = App.get_loop_component_start_time();
}

bool GenesisBus::server_enqueue_(uint8_t dst, uint8_t drv, uint8_t cmd, RequestKind kind, uint8_t client, uint8_t gen,
                                 uint32_t invoke, const uint8_t *payload, uint8_t len) {
  Request r;
  r.dst = dst;
  r.drv = drv;
  r.cmd = cmd;
  r.kind = kind;
  r.client = client;
  r.client_gen = gen;
  r.invoke = invoke;
  r.len = std::min<uint8_t>(len, REQ_PAYLOAD);
  if (payload != nullptr && r.len > 0)
    memcpy(r.payload, payload, r.len);
  if (!this->queue_.push(r)) {
    ESP_LOGW(TAG, "iComm server: queue full, dropping drv=%02X cmd=%02X for %u", drv, cmd, dst);
    return false;
  }
  return true;
}

void GenesisBus::server_device_summary_(size_t idx, uint8_t gen, uint32_t invoke, uint8_t address) {
  // 0x13 body: name[32], u16 1, u16 0x0A, u32 ConfigKey, firmware (2 bytes as in SYSTEM_READ), u16 block count,
  // u16 3. A dead controller gets an empty body. Always read fresh so ConfigKey and the count are current.
  // A controller we already know is dead is answered straight away (as iComm does): asking it again costs a
  // reply timeout, and through an iComm server a raw request to a dead controller is never answered at all.
  const DeviceState *dev = this->find_device_(address);
  if (dev != nullptr && dev->checked && !dev->alive) {
    this->server_send_(idx, IC_DEV_SUMMARY_REPLY, invoke, nullptr, 0);
    return;
  }
  if (!this->server_enqueue_(address, DRV_STATUS, CMD_SYSTEM_READ, REQ_SERVER_SYS, idx, gen, invoke))
    this->server_send_(idx, IC_DEV_SUMMARY_REPLY, invoke, nullptr, 0);
}

void GenesisBus::server_loop_(uint32_t now) {
  if (this->server_listen_fd_ < 0) {
    if (!network::is_connected() || (this->server_listen_try_ != 0 &&
                                     (int32_t) (now - this->server_listen_try_) < (int32_t) SERVER_LISTEN_RETRY_MS))
      return;
    this->server_listen_try_ = now;
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
      return;
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(this->server_port_);
    if (::bind(fd, (struct sockaddr *) &addr, sizeof(addr)) != 0 || ::listen(fd, 2) != 0) {
      ESP_LOGE(TAG, "iComm server: bind/listen on %u failed", this->server_port_);
      ::close(fd);
      return;
    }
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    this->server_listen_fd_ = fd;
    ESP_LOGI(TAG, "iComm server listening on port %u (Supervisor can connect here)", this->server_port_);
  }

  struct sockaddr_in peer {};
  socklen_t plen = sizeof(peer);
  int fd = ::accept(this->server_listen_fd_, (struct sockaddr *) &peer, &plen);
  if (fd >= 0) {
    size_t slot = MAX_SERVER_CLIENTS;
    for (size_t i = 0; i < MAX_SERVER_CLIENTS && slot == MAX_SERVER_CLIENTS; i++)
      if (this->server_clients_[i].fd < 0)
        slot = i;
    if (slot == MAX_SERVER_CLIENTS) {
      ESP_LOGW(TAG, "iComm server: too many clients, refusing one");
      ::close(fd);
    } else {
      ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
      int one = 1;
      ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
      ServerClient &c = this->server_clients_[slot];
      c.fd = fd;
      c.gen++;
      c.rx_len = 0;
      c.raw_pending = false;
      c.raw_inflight = false;
      c.update_invoke_set = false;
      c.last_tx = now;
      char ip[16];
      inet_ntoa_r(peer.sin_addr, ip, sizeof(ip));
      ESP_LOGI(TAG, "iComm server: client %u connected from %s", (unsigned) slot, ip);
    }
  }

  for (size_t i = 0; i < MAX_SERVER_CLIENTS; i++) {
    ServerClient &c = this->server_clients_[i];
    if (c.fd < 0)
      continue;
    const size_t space = SERVER_RX_BUF - c.rx_len;
    if (space > 0) {
      ssize_t n = ::recv(c.fd, c.rx + c.rx_len, space, MSG_DONTWAIT);
      if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
        this->server_close_(i);
        continue;
      }
      if (n > 0)
        c.rx_len += n;
    }
    while (c.fd >= 0 && c.rx_len >= 4) {
      const size_t len = c.rx[2] | (c.rx[3] << 8);
      if (c.rx[0] != 0x78 || c.rx[1] != 0x56 || 4 + len > SERVER_RX_BUF || len < 6) {
        ESP_LOGW(TAG, "iComm server: framing lost on client %u", (unsigned) i);
        this->server_close_(i);
        break;
      }
      if (c.rx_len < 4 + len)
        break;
      const uint16_t type = c.rx[4] | (c.rx[5] << 8);
      uint32_t invoke;
      memcpy(&invoke, c.rx + 6, 4);
      this->server_handle_(i, type, invoke, c.rx + 10, len - 6);
      if (c.fd < 0)
        break;
      c.rx_len -= 4 + len;
      memmove(c.rx, c.rx + 4 + len, c.rx_len);
    }
    if (c.fd >= 0 && (int32_t) (now - c.last_tx) > (int32_t) SERVER_KEEPALIVE_MS)
      this->server_send_(i, IC_KEEPALIVE, 0, nullptr, 0);
  }

  this->server_poll_(now);
  this->server_flush_updates_();
}

void GenesisBus::server_handle_(size_t idx, uint16_t type, uint32_t invoke, const uint8_t *b, size_t len) {
  ServerClient &c = this->server_clients_[idx];
  uint8_t reply[64];
  switch (type) {
    case IC_HELLO: {
      char app[33] = {0};
      if (len >= 34)
        memcpy(app, b + 2, 32);
      ESP_LOGI(TAG, "iComm server: client %u is '%s'", (unsigned) idx, app);
      const uint16_t hdr[2] = {IC_HELLO, IC_STATUS_OK};
      memcpy(reply, hdr, 4);
      memcpy(reply + 4, SERVER_OK_TAIL, 4);
      this->server_send_(idx, IC_REPLY, invoke, reply, 8);
      break;
    }
    case IC_SESSION: {
      // 0a 00 02 00 00 00 41 00 + 16 bytes (a per-session random key in iComm)
      static constexpr uint8_t HEAD[] = {0x0A, 0x00, 0x02, 0x00, 0x00, 0x00, 0x41, 0x00};
      memcpy(reply, HEAD, sizeof(HEAD));
      for (size_t k = 0; k < 16; k += 4) {
        const uint32_t r = random_uint32();
        memcpy(reply + 8 + k, &r, 4);
      }
      this->server_send_(idx, IC_SESSION_REPLY, invoke, reply, 24);
      break;
    }
    case IC_CONN_LIST: {
      uint8_t body[SERVER_CONN_LIST_LEN] = {0};
      memcpy(body, SERVER_CONN_LIST, sizeof(SERVER_CONN_LIST));
      this->server_send_(idx, IC_CONN_LIST_REPLY, invoke, body, sizeof(body));
      break;
    }
    case IC_CONN_INFO: {
      uint8_t body[SERVER_CONN_INFO_LEN] = {0};
      memcpy(body, SERVER_CONN_INFO, sizeof(SERVER_CONN_INFO));
      this->server_send_(idx, IC_CONN_INFO_REPLY, invoke, body, sizeof(body));
      break;
    }
    case IC_DEV_LIST: {
      // u16 count, then per device: u16 1, u16 1, u16 address, name\0, address as text\0
      uint8_t body[SERVER_TX_BUF - 16];
      size_t o = 2;
      uint16_t count = 0;
      for (const auto &d : this->devices_) {
        char name[24];
        if (d.name[0] != '\0')
          snprintf(name, sizeof(name), "%s", d.name);
        else
          snprintf(name, sizeof(name), "Controller %u", d.address);
        char addr[4];
        snprintf(addr, sizeof(addr), "%u", d.address);
        const size_t need = 6 + strlen(name) + 1 + strlen(addr) + 1;
        if (o + need > sizeof(body))
          break;
        const uint16_t f[3] = {1, 1, d.address};
        memcpy(body + o, f, 6);
        o += 6;
        memcpy(body + o, name, strlen(name) + 1);
        o += strlen(name) + 1;
        memcpy(body + o, addr, strlen(addr) + 1);
        o += strlen(addr) + 1;
        count++;
      }
      memcpy(body, &count, 2);
      this->server_send_(idx, IC_DEV_LIST_REPLY, invoke, body, o);
      break;
    }
    case IC_DEV_SUMMARY: {
      const uint8_t dev = server_parse_device(b, len, this->icomm_conn_);
      if (dev == 0)
        this->server_send_(idx, IC_DEV_SUMMARY_REPLY, invoke, nullptr, 0);
      else
        this->server_device_summary_(idx, c.gen, invoke, dev);
      break;
    }
    case IC_RAW_REQUEST: {
      // 32-byte struct (see icomm_raw_) then the payload
      if (len < 33) {
        const uint16_t r[2] = {0, 0};
        this->server_send_(idx, IC_RAW_REPLY, invoke, r, 4);
        break;
      }
      char dtxt[3] = {(char) b[8], (char) b[9], 0};
      const unsigned dev = (unsigned) atoi(dtxt);
      uint16_t cmd, drv, plen;
      memcpy(&cmd, b + 26, 2);
      memcpy(&drv, b + 28, 2);
      memcpy(&plen, b + 30, 2);
      if (dev == 0 || dev > 254 || plen > MAX_FRAME_DATA || 32 + plen > len || c.raw_pending || c.raw_inflight) {
        ESP_LOGW(TAG, "iComm server: bad or overlapping raw request from client %u", (unsigned) idx);
        const uint16_t r[2] = {0, 0};
        this->server_send_(idx, IC_RAW_REPLY, invoke, r, 4);
        break;
      }
      c.raw_invoke = invoke;
      c.raw_dst = dev;
      c.raw_drv = drv;
      c.raw_cmd = cmd;
      c.raw_flag = b[25];
      c.raw_len = plen;
      memcpy(c.raw_buf, b + 32, plen);
      c.raw_pending = true;
      ESP_LOGV(TAG, "iComm server: raw %02X/%02X to %u from client %u", drv, cmd, dev, (unsigned) idx);
      break;
    }
    case IC_ADVISE: {
      if (len < 2)
        break;
      int16_t count;
      memcpy(&count, b, 2);
      if (!c.update_invoke_set) {
        c.update_invoke = invoke;
        c.update_invoke_set = true;
      }
      for (int16_t r = 0; r < count && 2 + 64u * (r + 1) <= len; r++) {
        const uint8_t *rec = b + 2 + 64 * r;
        ServerSub s;
        s.client = idx;
        s.client_gen = c.gen;
        memcpy(&s.handle, rec, 4);
        s.dev = server_parse_device(rec + 4, 16, this->icomm_conn_);
        char pt[33] = {0};
        memcpy(pt, rec + 20, 32);
        const char *tilde = strchr(pt, '~');
        if (s.dev != 0 && strcasecmp(pt, "System~ConfigKey") == 0) {
          s.out = 0;
        } else if (s.dev != 0 && pt[0] == '@' && tilde != nullptr) {
          s.seq = (uint8_t) atoi(pt + 1);
          s.out = server_output_for_field(tilde + 1);
          if (s.out == 0 || s.seq == 0) {
            s.status = IC_STATUS_BAD_FIELD;  // a parameter or unknown field: not supported for subscriptions
            s.dirty = true;
          }
        } else {
          s.status = IC_STATUS_BAD_FIELD;  // block names aren't resolved
          s.dirty = true;
        }
        if (this->server_subs_.size() >= MAX_SERVER_SUBS) {
          ESP_LOGW(TAG, "iComm server: too many subscriptions");
          break;
        }
        this->server_subs_.push_back(s);
      }
      const uint16_t hdr[2] = {IC_ADVISE, IC_STATUS_OK};
      memcpy(reply, hdr, 4);
      memcpy(reply + 4, SERVER_OK_TAIL, 4);
      this->server_send_(idx, IC_REPLY, invoke, reply, 8);
      this->server_last_poll_ = 0;  // poll straight away
      break;
    }
    case IC_UNADVISE: {
      if (len < 2)
        break;
      int16_t count;
      memcpy(&count, b, 2);
      for (int16_t r = 0; r < count && 2 + 4u * (r + 1) <= len; r++) {
        uint32_t h;
        memcpy(&h, b + 2 + 4 * r, 4);
        size_t w = 0;
        for (size_t k = 0; k < this->server_subs_.size(); k++) {
          const ServerSub &s = this->server_subs_[k];
          if (!(s.client == idx && s.handle == h))
            this->server_subs_[w++] = s;
        }
        this->server_subs_.resize(w);
      }
      break;
    }
    case 0x0005: {  // read: only System~ConfigKey (iComm 2.38 also refuses live outputs here)
      const uint8_t dev = len >= 48 ? server_parse_device(b, 16, this->icomm_conn_) : 0;
      char pt[33] = {0};
      if (len >= 48)
        memcpy(pt, b + 16, 32);
      const DeviceState *d = dev != 0 ? this->find_device_(dev) : nullptr;
      if (d != nullptr && d->sys_valid && strcasecmp(pt, "System~ConfigKey") == 0) {
        const uint16_t hdr[2] = {0x0005, IC_STATUS_OK};
        memcpy(reply, hdr, 4);
        memcpy(reply + 4, &d->config_key, 4);
        this->server_send_(idx, IC_REPLY, invoke, reply, 8);
      } else {
        const uint16_t hdr[2] = {0x0005, IC_STATUS_ERROR};
        this->server_send_(idx, IC_REPLY, invoke, hdr, 4);
      }
      break;
    }
    case IC_WRITE: {  // write: User Variable Value / Output through the safe read-patch-write path
      bool ok = false;
      if (len >= 49) {
        const uint8_t dev = server_parse_device(b, 16, this->icomm_conn_);
        char pt[33] = {0};
        memcpy(pt, b + 16, 32);
        const char *tilde = strchr(pt, '~');
        const uint8_t dtype = b[48];
        double v = NAN;
        if (dtype == 2 && len >= 57)
          memcpy(&v, b + 49, 8);
        else if (dtype == 3 && len >= 51)
          v = b[49] | (b[50] << 8);
        if (dev != 0 && pt[0] == '@' && tilde != nullptr && !std::isnan(v) &&
            (strcasecmp(tilde + 1, "Value") == 0 || strcasecmp(tilde + 1, "Output") == 0)) {
          this->write_user_variable(dev, (uint8_t) atoi(pt + 1), (float) v);
          ok = true;
        } else {
          ESP_LOGW(TAG, "iComm server: write to '%s' not supported", pt);
        }
      }
      uint16_t hdr[2] = {IC_WRITE, ok ? IC_STATUS_OK : IC_STATUS_ERROR};
      memcpy(reply, hdr, 4);
      memcpy(reply + 4, SERVER_OK_TAIL, 4);
      this->server_send_(idx, IC_REPLY, invoke, reply, ok ? 8 : 4);
      break;
    }
    case IC_KEEPALIVE:
      break;
    default:
      ESP_LOGD(TAG, "iComm server: unhandled message type 0x%04X (%u bytes) from client %u", type, (unsigned) len,
               (unsigned) idx);
      break;
  }
}

void GenesisBus::server_poll_(uint32_t now) {
  if (this->server_subs_.empty() || this->queue_.size() >= QUEUE_SIZE / 2)
    return;
  // ConfigKey subscriptions: refresh SYSTEM_READ now and then
  if ((int32_t) (now - this->server_last_sys_) >= (int32_t) SERVER_SYS_MS) {
    this->server_last_sys_ = now;
    uint8_t done[MAX_DEVICES];
    size_t nd = 0;
    for (const auto &s : this->server_subs_) {
      if (s.out != 0 || s.dev == 0 || s.status == IC_STATUS_BAD_FIELD)
        continue;
      bool dup = false;
      for (size_t k = 0; k < nd && !dup; k++)
        dup = done[k] == s.dev;
      if (dup || nd >= MAX_DEVICES)
        continue;
      done[nd++] = s.dev;
      this->server_enqueue_(s.dev, DRV_STATUS, CMD_SYSTEM_READ, REQ_SERVER_SYS, 0xFF, 0, 0);
    }
  }
  if ((int32_t) (now - this->server_last_poll_) < (int32_t) SERVER_POLL_MS)
    return;
  this->server_last_poll_ = now;
  // One MVO per device and max_points_ unique (block, output) pairs, like poll_outputs_().
  uint8_t devs[MAX_DEVICES];
  size_t nd = 0;
  for (const auto &s : this->server_subs_) {
    if (s.out == 0 || s.dev == 0 || s.status == IC_STATUS_BAD_FIELD)
      continue;
    bool dup = false;
    for (size_t k = 0; k < nd && !dup; k++)
      dup = devs[k] == s.dev;
    if (!dup && nd < MAX_DEVICES)
      devs[nd++] = s.dev;
  }
  uint8_t buf[REQ_PAYLOAD];
  for (size_t k = 0; k < nd; k++) {
    uint8_t cnt = 0;
    for (const auto &s : this->server_subs_) {
      if (s.dev != devs[k] || s.out == 0 || s.status == IC_STATUS_BAD_FIELD)
        continue;
      bool dup = false;
      for (uint8_t j = 0; j < cnt && !dup; j++)
        dup = buf[1 + 2 * j] == s.seq && buf[2 + 2 * j] == s.out;
      if (dup)
        continue;
      buf[1 + 2 * cnt] = s.seq;
      buf[2 + 2 * cnt] = s.out;
      if (++cnt == this->max_points_) {
        buf[0] = cnt;
        if (!this->server_enqueue_(devs[k], DRV_BLOCK, CMD_READ_OUTPUTS, REQ_SERVER_MVO, 0xFF, 0, 0, buf, 1 + 2 * cnt))
          return;
        cnt = 0;
      }
    }
    if (cnt > 0) {
      buf[0] = cnt;
      if (!this->server_enqueue_(devs[k], DRV_BLOCK, CMD_READ_OUTPUTS, REQ_SERVER_MVO, 0xFF, 0, 0, buf, 1 + 2 * cnt))
        return;
    }
  }
}

void GenesisBus::server_flush_updates_() {
  // Type 3: u16 count, then records u32 handle, u16 status, u8 value type, value (2 = f64, 4 = u32, 0 = none).
  for (size_t i = 0; i < MAX_SERVER_CLIENTS; i++) {
    ServerClient &c = this->server_clients_[i];
    if (c.fd < 0)
      continue;
    uint8_t body[SERVER_TX_BUF - 16];
    size_t o = 2;
    uint16_t count = 0;
    for (auto &s : this->server_subs_) {
      if (s.client != i || s.client_gen != c.gen || !s.dirty)
        continue;
      if (o + 15 > sizeof(body))
        break;
      memcpy(body + o, &s.handle, 4);
      memcpy(body + o + 4, &s.status, 2);
      o += 6;
      if (s.status != IC_STATUS_OK) {
        body[o++] = 0;
      } else if (s.out == 0) {
        body[o++] = 4;
        memcpy(body + o, &s.raw, 4);
        o += 4;
      } else {
        float f;
        memcpy(&f, &s.raw, 4);
        const double v = f;
        body[o++] = 2;
        memcpy(body + o, &v, 8);
        o += 8;
      }
      s.dirty = false;
      count++;
    }
    if (count == 0)
      continue;
    memcpy(body, &count, 2);
    this->server_send_(i, IC_UPDATE, c.update_invoke, body, o);
  }
}

void GenesisBus::server_finish_(bool ok, uint8_t status, const uint8_t *d, size_t n) {
  const Request &r = this->current_;
  const bool for_client = r.client < MAX_SERVER_CLIENTS && this->server_clients_[r.client].fd >= 0 &&
                          this->server_clients_[r.client].gen == r.client_gen;
  switch (r.kind) {
    case REQ_SERVER_RAW: {
      if (r.client < MAX_SERVER_CLIENTS)
        this->server_clients_[r.client].raw_inflight = false;
      if (!for_client)
        break;
      if (!ok) {
        // Like iComm: a raw request that got no answer (dead controller) is refused with a type-1 0xF447.
        const uint16_t hdr[2] = {IC_RAW_REQUEST, IC_STATUS_RAW_DEAD};
        this->server_send_(r.client, IC_REPLY, r.invoke, hdr, 4);
        break;
      }
      // u16 result (1 = OK), u16 data length, data.
      uint8_t body[4 + MAX_FRAME_DATA];
      const uint16_t result = status == 0 ? IC_STATUS_OK : IC_STATUS_ERROR;
      const uint16_t dl = status == 0 ? std::min<size_t>(n, MAX_FRAME_DATA) : 0;
      memcpy(body, &result, 2);
      memcpy(body + 2, &dl, 2);
      if (dl > 0)
        memcpy(body + 4, d, dl);
      this->server_send_(r.client, IC_RAW_REPLY, r.invoke, body, 4 + dl);
      break;
    }
    case REQ_SERVER_MVO: {
      const uint8_t cnt = r.payload[0];
      const bool good = ok && status == 0 && n >= 1u + 4u * cnt && d[0] == cnt;
      for (uint8_t i = 0; i < cnt; i++) {
        const uint8_t seq = r.payload[1 + 2 * i];
        const uint8_t out = r.payload[2 + 2 * i];
        uint32_t raw = 0xFFFFFFFFu;
        if (good)
          memcpy(&raw, &d[1 + 4 * i], 4);
        const uint16_t st = !good ? IC_STATUS_DEVICE_DEAD : (raw == 0xFFFFFFFFu ? IC_STATUS_NO_VALUE : IC_STATUS_OK);
        for (auto &s : this->server_subs_) {
          if (s.dev != r.dst || s.seq != seq || s.out != out)
            continue;
          if (!s.have || s.status != st || s.raw != raw) {
            s.have = true;
            s.status = st;
            s.raw = raw;
            s.dirty = st != IC_STATUS_NO_VALUE;  // nothing to report until it has a value
          }
        }
      }
      break;
    }
    case REQ_SERVER_SYS: {
      DeviceState *dev = this->find_device_(r.dst);
      const bool valid = ok && status == 0 && n > 0xBB;
      if (valid && dev != nullptr) {
        memcpy(dev->name, d, 20);
        dev->name[20] = '\0';
        memcpy(&dev->config_key, d + 0x31, 4);
        dev->fw_lo = d[0xBA];
        dev->fw_hi = d[0xBB];
        dev->sys_valid = true;
      }
      for (auto &s : this->server_subs_) {
        if (s.dev != r.dst || s.out != 0 || s.status == IC_STATUS_BAD_FIELD)
          continue;
        uint32_t key = 0;
        if (valid)
          memcpy(&key, d + 0x31, 4);
        const uint16_t st = valid ? IC_STATUS_OK : IC_STATUS_DEVICE_DEAD;
        if (!s.have || s.status != st || s.raw != key) {
          s.have = true;
          s.status = st;
          s.raw = key;
          s.dirty = true;
        }
      }
      if (!for_client)
        break;
      if (!valid) {
        this->server_send_(r.client, IC_DEV_SUMMARY_REPLY, r.invoke, nullptr, 0);
        break;
      }
      // second step: block count, then the 0x13 reply. Keep this SYSTEM_READ in the device for the reply.
      if (!this->server_enqueue_(r.dst, DRV_BLOCK, CMD_BLOCK_COUNT, REQ_SERVER_COUNT, r.client, r.client_gen,
                                 r.invoke))
        this->server_send_(r.client, IC_DEV_SUMMARY_REPLY, r.invoke, nullptr, 0);
      break;
    }
    case REQ_SERVER_COUNT: {
      DeviceState *dev = this->find_device_(r.dst);
      if (dev != nullptr && ok && status == 0 && n >= 1)
        dev->block_count = d[0];
      if (!for_client)
        break;
      if (!ok || status != 0 || n < 1) {
        this->server_send_(r.client, IC_DEV_SUMMARY_REPLY, r.invoke, nullptr, 0);
        break;
      }
      uint8_t body[46] = {0};
      if (dev != nullptr)
        snprintf((char *) body, 32, "%s", dev->name);
      const uint16_t one = 1, typ = 0x0A, blocks = d[0], tail = 3;
      memcpy(body + 32, &one, 2);
      memcpy(body + 34, &typ, 2);
      if (dev != nullptr) {
        memcpy(body + 36, &dev->config_key, 4);
        body[40] = dev->fw_lo;
        body[41] = dev->fw_hi;
      }
      memcpy(body + 42, &blocks, 2);
      memcpy(body + 44, &tail, 2);
      this->server_send_(r.client, IC_DEV_SUMMARY_REPLY, r.invoke, body, sizeof(body));
      break;
    }
    default:
      break;
  }
}

// ------------------------------------------------------------- Number

#ifdef USE_NUMBER
void GenesisNumber::dump_config() { LOG_NUMBER("", "Genesis setpoint", this); }

void GenesisNumber::control(float value) {
  this->bus_->write_user_variable(this->address_, this->block_, value);
  this->publish_state(value);
}
#endif

#ifdef USE_CLIMATE
// ------------------------------------------------------------- Climate

// After a write, ignore polled targets for a while: the next poll can still carry the old value.
static constexpr uint32_t CLIMATE_WRITE_SETTLE_MS = 15000;
static constexpr float CLIMATE_VALVE_OPEN = 1.0f;  // percent

void GenesisClimate::setup() {
  this->mode = climate::CLIMATE_MODE_AUTO;
  this->action = climate::CLIMATE_ACTION_IDLE;
}

void GenesisClimate::dump_config() {
  LOG_CLIMATE("", "Genesis zone", this);
  ESP_LOGCONFIG(TAG, "  Device %u, setpoint block %u%s%s%s", this->address_, this->setpoint_block_,
                this->has_valve_ ? ", valve" : "", this->has_fan_ ? ", fan status" : "",
                this->has_heater_ ? ", heaters" : "");
  if (this->fan_output_block_ != 0)
    ESP_LOGCONFIG(TAG, "  Off = force supply-fan output block %u off", this->fan_output_block_);
}

climate::ClimateTraits GenesisClimate::traits() {
  climate::ClimateTraits traits;
  uint32_t flags = climate::CLIMATE_SUPPORTS_CURRENT_TEMPERATURE;
  if (this->has_valve_ || this->has_fan_ || this->has_heater_)
    flags |= climate::CLIMATE_SUPPORTS_ACTION;
  traits.add_feature_flags(flags);
  if (this->fan_output_block_ != 0)
    traits.add_supported_mode(climate::CLIMATE_MODE_OFF);
  traits.add_supported_mode(climate::CLIMATE_MODE_AUTO);
  traits.set_visual_min_temperature(15.0f);
  traits.set_visual_max_temperature(30.0f);
  traits.set_visual_target_temperature_step(0.5f);
  traits.set_visual_current_temperature_step(0.1f);
  return traits;
}

climate::ClimateAction GenesisClimate::compute_action_() const {
  // Fan stopped = off. Otherwise the zone's own heater stages, then the AHU's chilled-water valve; a running
  // fan with neither is just moving air (fan). Idle only when the fan state isn't known.
  if (this->has_fan_ && this->fan_ == 0)
    return climate::CLIMATE_ACTION_OFF;
  for (int8_t h : this->heater_)
    if (h == 1)
      return climate::CLIMATE_ACTION_HEATING;
  if (this->has_valve_ && !std::isnan(this->valve_) && this->valve_ > CLIMATE_VALVE_OPEN)
    return climate::CLIMATE_ACTION_COOLING;
  if (this->has_fan_ && this->fan_ == 1)
    return climate::CLIMATE_ACTION_FAN;
  return climate::CLIMATE_ACTION_IDLE;
}

void GenesisClimate::on_point(uint8_t role, float value, bool valid) {
  bool changed = false;
  switch (role) {
    case ROLE_CURRENT: {
      const float v = valid ? value : NAN;
      // Only publish meaningful changes; iComm repeats every value about once a second.
      if (std::isnan(v) != std::isnan(this->current_temperature) ||
          (!std::isnan(v) && std::fabs(v - this->current_temperature) >= 0.05f)) {
        this->current_temperature = v;
        changed = true;
      }
      break;
    }
    case ROLE_TARGET:
      if (valid &&
          (!this->target_written_ || (int32_t) (App.get_loop_component_start_time() - this->last_write_) >
                                         (int32_t) CLIMATE_WRITE_SETTLE_MS) &&
          (std::isnan(this->target_temperature) || std::fabs(value - this->target_temperature) >= 0.01f)) {
        this->target_temperature = value;
        changed = true;
      }
      break;
    case ROLE_VALVE:
      this->valve_ = valid ? value : NAN;
      break;
    case ROLE_FAN:
      this->fan_ = valid ? (value != 0.0f ? 1 : 0) : -1;
      break;
    case ROLE_FORCED:
      this->forced_ = valid ? (value != 0.0f ? 1 : 0) : -1;
      break;
    case ROLE_FORCE_VALUE:
      this->force_value_ = valid ? (value != 0.0f ? 1 : 0) : -1;
      break;
    case ROLE_HEATER_1:
    case ROLE_HEATER_2:
      this->heater_[role - ROLE_HEATER_1] = valid ? (value != 0.0f ? 1 : 0) : -1;
      break;
    default:
      break;
  }
  // Re-check on every update (iComm only pushes Forced when it changes), but not right after our own write:
  // the controller can still report the old override for a few seconds.
  // Forced with ForceValue not read yet can't be told apart (off vs on), so wait for both.
  if (this->fan_output_block_ != 0 && (this->forced_ == 0 || (this->forced_ == 1 && this->force_value_ >= 0)) &&
      (!this->mode_written_ ||
       (int32_t) (App.get_loop_component_start_time() - this->last_mode_write_) > (int32_t) CLIMATE_WRITE_SETTLE_MS)) {
    const climate::ClimateMode m = this->mode_from_override_();
    if (m != this->mode) {
      this->mode = m;
      changed = true;
    }
  }
  const climate::ClimateAction action = this->compute_action_();
  if (action != this->action) {
    this->action = action;
    changed = true;
  }
  if (changed)
    this->publish_state();
}

climate::ClimateMode GenesisClimate::mode_from_override_() const {
  // Forced off = Off. Forced on (Force on button, or Supervisor) = Fan only: not selectable, but it makes the
  // override obvious. Auto or Off replaces it.
  if (this->forced_ == 1)
    return this->force_value_ == 1 ? climate::CLIMATE_MODE_FAN_ONLY : climate::CLIMATE_MODE_OFF;
  return climate::CLIMATE_MODE_AUTO;
}

void GenesisClimate::control(const climate::ClimateCall &call) {
  const uint32_t now = App.get_loop_component_start_time();
  if (call.get_target_temperature().has_value()) {
    const float t = *call.get_target_temperature();
    this->bus_->write_user_variable(this->address_, this->setpoint_block_, t);
    this->target_temperature = t;
    this->last_write_ = now;
    this->target_written_ = true;
  }
  if (call.get_mode().has_value()) {
    const climate::ClimateMode want = *call.get_mode();
    if (this->fan_output_block_ == 0) {
      if (want != climate::CLIMATE_MODE_AUTO)
        ESP_LOGW(TAG, "%s: no fan_output_block configured, only Auto is available", this->get_name().c_str());
    } else if (want == climate::CLIMATE_MODE_OFF) {
      this->bus_->force_output(this->address_, this->fan_output_block_, FORCE_OFF);
      this->mode = climate::CLIMATE_MODE_OFF;
      this->last_mode_write_ = now;
      this->mode_written_ = true;
    } else if (this->mode != climate::CLIMATE_MODE_AUTO || this->forced_ == 1) {
      // Auto: hand the fan back to its schedule / switch logic. Also clears a force-on.
      this->bus_->force_output(this->address_, this->fan_output_block_, FORCE_RELEASE);
      this->mode = climate::CLIMATE_MODE_AUTO;
      this->last_mode_write_ = now;
      this->mode_written_ = true;
    }
  }
  this->publish_state();
}
#endif

}  // namespace innotech_genesis
}  // namespace esphome
