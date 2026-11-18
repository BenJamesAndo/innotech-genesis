#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/components/uart/uart.h"
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#ifdef USE_NUMBER
#include "esphome/components/number/number.h"
#endif
#ifdef USE_BUTTON
#include "esphome/components/button/button.h"
#endif
#ifdef USE_CLIMATE
#include "esphome/components/climate/climate.h"
#endif

namespace esphome {
namespace innotech_genesis {

// Innotech Genesis II "Net" protocol (see gcc-bms/protocol/PROTOCOL.md):
//   FF FF FF FF 22 DD | hdr[9] | crc16 | data[n] | crc16 | FF FF
//   request hdr: seq, dst, drv, src(FF), 0, flag, cmd, len16
//   reply hdr:   seq, FF, 0, src, drv, 0, status, len16
// CRC-16/ARC (poly 0xA001 reflected, init 0) over header and data separately.
//
// All buffers are fixed-size: nothing is allocated on the heap after setup().

static constexpr size_t MAX_DEVICES = 16;
static constexpr size_t MAX_POINTS = 192;
static constexpr size_t MAX_ICOMM_HOSTS = 4;
static constexpr size_t MAX_FRAME_DATA = 512;                 // data bytes in one Net frame
static constexpr size_t FRAME_BUF = 11 + MAX_FRAME_DATA + 2;  // header+crc, data, crc
static constexpr size_t TX_BUF = 6 + FRAME_BUF + 2;           // FF FF FF FF 22 DD .. FF FF
static constexpr size_t QUEUE_SIZE = 24;
static constexpr size_t REQ_PAYLOAD = 64;  // MVO with 30 points = 61 bytes
static constexpr uint8_t MAX_POINTS_PER_MESSAGE = 30;
static constexpr size_t MAX_PENDING_WRITES = 8;
static constexpr size_t IC_RX_BUF = 4608;  // iComm batches updates up to 4096 bytes
static constexpr size_t IC_TX_BUF = 1400;  // advise batch of 20 points = 1292 bytes

class GenesisClimate;

uint16_t crc16_arc(const uint8_t *data, size_t len, uint16_t crc = 0);

// Incremental frame parser (sync hunt + CRC checks) into a fixed buffer.
// The frame accessors stay valid until the next feed().
class FrameParser {
 public:
  // Feed one byte; returns true when a complete, CRC-valid frame is in the buffer.
  bool feed(uint8_t b);
  void reset() {
    this->state_ = 0;
    this->len_ = 0;
  }
  const uint8_t *hdr() const { return this->buf_; }
  const uint8_t *data() const { return this->buf_ + 11; }
  uint16_t data_len() const { return this->need_; }
  // Header + crc + data + crc, i.e. the frame without sync/padding.
  const uint8_t *body() const { return this->buf_; }
  size_t body_len() const { return 11u + this->need_ + 2u; }
  uint32_t crc_errors{0};

 protected:
  uint8_t state_{0};  // 0 = hunting, 1 = header, 2 = data
  uint8_t prev_{0};
  uint16_t need_{0};
  size_t len_{0};
  uint8_t buf_[FRAME_BUF];
};

// What a bus request is for; replaces per-request callbacks.
enum RequestKind : uint8_t {
  REQ_STATUS,       // 9/1 status read
  REQ_MVO,          // 2/9 read many outputs; payload = n + (block, output) pairs
  REQ_WRITE_READ,   // 2/5 read block before patching a User Variable
  REQ_WRITE_BLOCK,  // 2/6 write the patched block (payload in write_buf_)
  REQ_SAVE_FLASH,   // 1/2
  REQ_SCAN_INFO,    // 0/FF device info
  REQ_SCAN_COUNT,   // 2/8 block count
  REQ_SCAN_BLOCK,   // 2/5 one block during a scan
  REQ_BRIDGE,       // raw frame from the TCP bridge client
  REQ_PARAM_READ,   // 2/5 read a block record for parameter points (e.g. a Digital Output's Forced flag)
};

// Parameter points: values that live in the block record (section 2) instead of the live outputs.
enum ParamField : uint8_t {
  PARAM_NONE = 0,
  PARAM_FORCED = 1,       // Digital Output: manual override active (u8 at param base + 4)
  PARAM_FORCE_VALUE = 2,  // Digital Output: value used while forced (u8 at param base + 5)
};

enum WriteKind : uint8_t {
  WJ_USER_VALUE = 0,  // User Variable Value (setpoints, timers)
  WJ_FORCE = 1,       // Digital Output manual override (see ForceMode)
};

// Digital Output manual override. FORCE_ON bypasses the controller's own logic (schedules, after-hours
// switch, fire-alarm input), so it is only offered as a separate, deliberate button.
enum ForceMode : uint8_t {
  FORCE_RELEASE = 0,  // Forced = 0, ForceValue = 0: back to the controller's logic
  FORCE_OFF = 1,      // Forced = 1, ForceValue = 0
  FORCE_ON = 2,       // Forced = 1, ForceValue = 1
};

struct Request {
  uint8_t dst{0};
  uint8_t drv{0};
  uint8_t cmd{0};
  uint8_t kind{REQ_STATUS};
  uint8_t len{0};
  uint8_t payload[REQ_PAYLOAD];
};

struct WriteJob {
  uint8_t address{0};
  uint8_t block{0};
  uint8_t kind{WJ_USER_VALUE};
  uint8_t force{FORCE_RELEASE};  // WJ_FORCE: ForceMode
  float value{0};
};

struct DeviceState {
  uint8_t address{0};
  bool alive{false};
  bool checked{false};
  uint8_t failures{0};
  uint32_t last_status{0};
  float firmware{0};
  char name[21]{};
#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *online_sensor{nullptr};
#endif
};

struct Point {
  uint8_t address{0};
  uint8_t block{0};
  uint8_t output{1};
  uint8_t param{PARAM_NONE};
  // iComm transport: which field name is being tried ("OUTPUT", "Value", "State"); parameter points use a
  // fixed name ("Forced", "ForceValue") from base_field and never fall back.
  uint8_t base_field{0};
  uint8_t field_idx{0};
  bool field_failed{false};
  bool readvise{false};
#ifdef USE_SENSOR
  sensor::Sensor *sensor{nullptr};
#endif
#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *binary{nullptr};
#endif
#ifdef USE_NUMBER
  number::Number *number{nullptr};
#endif
#ifdef USE_CLIMATE
  GenesisClimate *climate{nullptr};
  uint8_t climate_role{0};
#endif
};

enum Transport : uint8_t { TRANSPORT_SERIAL = 0, TRANSPORT_ICOMM = 1, TRANSPORT_AUTO = 2 };

class GenesisBus : public Component, public uart::UARTDevice {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;

  void set_flow_control_pin(GPIOPin *pin) { this->flow_control_pin_ = pin; }
  void set_tcp_port(uint16_t port) { this->tcp_port_ = port; }
  void set_update_interval(uint32_t ms) { this->update_interval_ = ms; }
  void set_status_interval(uint32_t ms) { this->status_interval_ = ms; }
  void set_reply_timeout(uint32_t ms) { this->reply_timeout_ = ms; }
  void set_max_points(uint8_t n) { this->max_points_ = n > MAX_POINTS_PER_MESSAGE ? MAX_POINTS_PER_MESSAGE : n; }
  void set_save_to_flash(bool v) { this->save_to_flash_ = v; }
  void set_polling(bool v) { this->polling_ = v; }
  void set_transport(uint8_t t) { this->transport_ = t; }
  void add_icomm_host(const char *host) {
    this->icomm_hosts_.push_back(host);
    this->icomm_config_hosts_.push_back(host);
  }
  void set_icomm_port(uint16_t port) { this->icomm_port_ = port; }
  void set_icomm_connection(uint8_t conn) { this->icomm_conn_ = conn; }
  void set_icomm_name(const char *name) { this->icomm_name_ = name; }
  // Runtime settings (e.g. from template text / select entities in the release firmware).
  // set_icomm_server: an IP address replaces icomm_hosts; an empty string goes back to the YAML's icomm_hosts.
  void set_icomm_server(const std::string &host);
  // set_transport_mode: 0 serial, 1 icomm, 2 auto (same order as the `transport` option).
  void set_transport_mode(uint8_t transport);
  void add_device(uint8_t address);

#ifdef USE_SENSOR
  // icomm_field: 0 = try OUTPUT/Value/State, else a fixed index into IC_FIELDS (e.g. a PID's Direct/Reverse).
  void add_sensor(uint8_t address, uint8_t block, uint8_t output, sensor::Sensor *s, uint8_t icomm_field = 0);
#endif
#ifdef USE_BINARY_SENSOR
  void add_binary_point(uint8_t address, uint8_t block, uint8_t output, binary_sensor::BinarySensor *s);
  void set_online_sensor(uint8_t address, binary_sensor::BinarySensor *s);
#endif
#ifdef USE_NUMBER
  void add_number(uint8_t address, uint8_t block, number::Number *n);
#endif
#ifdef USE_CLIMATE
  void add_climate_point(uint8_t address, uint8_t block, uint8_t role, GenesisClimate *c);
  void add_climate_param(uint8_t address, uint8_t block, uint8_t param, uint8_t role, GenesisClimate *c);
#endif

  // High-level operations
  void write_user_variable(uint8_t address, uint8_t block, float value);
  // Manual override of a Digital Output (ForceMode).
  void force_output(uint8_t address, uint8_t block, ForceMode mode);
  void scan_blocks(uint8_t address);
  void check_status(uint8_t address);

  // Status for the LED / diagnostics
  bool any_alive() const;
  bool all_alive() const;
  bool tcp_client_connected() const { return this->client_fd_ >= 0; }
  uint32_t last_activity() const { return this->last_activity_; }
  uint32_t frames_ok() const { return this->frames_ok_; }
  uint32_t timeouts() const { return this->timeouts_; }
  bool using_icomm() const { return this->active_icomm_; }
  bool icomm_connected() const { return this->icomm_ready_; }

 protected:
  DeviceState *find_device_(uint8_t address);
  Point *add_point_(uint8_t address, uint8_t block, uint8_t output);
  bool enqueue_(uint8_t dst, uint8_t drv, uint8_t cmd, RequestKind kind, const uint8_t *payload = nullptr,
                uint8_t len = 0);

  // serial bus
  void send_next_(uint32_t now);
  void send_raw_(const uint8_t *buf, size_t len, uint32_t now);
  void transmit_(uint8_t dst, uint8_t drv, uint8_t cmd, const uint8_t *payload, size_t len, uint32_t now);
  void handle_frame_();
  void finish_(bool ok, uint8_t status, const uint8_t *data, size_t len);
  void handle_status_(bool ok, uint8_t status, const uint8_t *d, size_t n);
  void handle_mvo_(bool ok, uint8_t status, const uint8_t *d, size_t n);
  void handle_write_read_(bool ok, uint8_t status, const uint8_t *d, size_t n);
  void handle_scan_(bool ok, uint8_t status, const uint8_t *d, size_t n);
  void handle_param_read_(bool ok, uint8_t status, const uint8_t *d, size_t n);
  void poll_outputs_();
  void poll_params_();
  void publish_point_(Point &p, float value, bool valid);
  void set_device_alive_(uint8_t address, bool alive);
  void switch_transport_(bool icomm);

  // TCP bridge for an iComm "TCP/IP" connection
  void tcp_setup_();
  void tcp_loop_(uint32_t now);
  void tcp_close_client_();

  // iComm TCP client (the protocol Supervisor speaks to iComm on port 20000)
  void icomm_loop_(uint32_t now);
  void icomm_close_();
  void icomm_begin_(uint16_t type);
  void icomm_put_(const void *data, size_t len);
  void icomm_put_point_(uint8_t address, uint8_t block, const char *field);
  void icomm_send_();
  void icomm_advise_(size_t first, size_t count);
  void icomm_handle_(const uint8_t *m, size_t len);
  // Bus command through iComm (message type 7), answered by type 8 into finish_().
  void icomm_raw_(uint8_t dst, uint8_t drv, uint8_t cmd, const uint8_t *payload, size_t len, uint32_t now);
  bool via_icomm_() const;

  // config
  GPIOPin *flow_control_pin_{nullptr};
  uint16_t tcp_port_{0};
  uint32_t update_interval_{5000};
  uint32_t status_interval_{60000};
  uint32_t reply_timeout_{1000};
  uint8_t max_points_{20};
  bool save_to_flash_{true};
  bool polling_{true};
  uint8_t transport_{TRANSPORT_AUTO};

  StaticVector<DeviceState, MAX_DEVICES> devices_;
  StaticVector<Point, MAX_POINTS> points_;
  StaticRingBuffer<Request, QUEUE_SIZE> queue_;

  // the request on the wire
  bool busy_{false};
  Request current_;
  uint8_t current_seq_{0};
  uint32_t sent_at_{0};
  uint8_t seq_{0x40};
  uint8_t tx_buf_[TX_BUF];
  FrameParser bus_parser_;

  // setpoint writes: one read-patch-write-save sequence at a time
  StaticRingBuffer<WriteJob, MAX_PENDING_WRITES> writes_;
  enum WriteStage : uint8_t { WS_IDLE, WS_READ, WS_WRITE, WS_SAVE, WS_WAIT };
  WriteStage write_stage_{WS_IDLE};
  WriteJob write_job_{};
  uint8_t write_buf_[1 + MAX_FRAME_DATA];
  size_t write_len_{0};

  // block scan: one controller at a time, one block per request
  uint8_t scan_address_{0};
  uint16_t scan_next_{0};
  uint16_t scan_count_{0};
  bool scan_waiting_{false};

  uint32_t last_poll_{0};
  uint32_t last_param_poll_{0};
  uint32_t last_activity_{0};
  uint32_t frames_ok_{0};
  uint32_t timeouts_{0};

  // TCP bridge
  int listen_fd_{-1};
  int client_fd_{-1};
  uint32_t listen_last_try_{0};
  FrameParser tcp_parser_;
  bool bridge_pending_{false};
  uint8_t bridge_buf_[TX_BUF];
  size_t bridge_len_{0};
  uint8_t bridge_dst_{0};
  uint8_t bridge_seq_{0};

  // iComm client
  bool active_icomm_{false};
  StaticVector<const char *, MAX_ICOMM_HOSTS> icomm_hosts_;
  StaticVector<const char *, MAX_ICOMM_HOSTS> icomm_config_hosts_;  // from YAML, restored by set_icomm_server("")
  uint8_t icomm_host_idx_{0};
  uint16_t icomm_port_{20000};
  uint8_t icomm_conn_{1};
  const char *icomm_name_{nullptr};  // nullptr = "ESPHome <friendly name> <MAC suffix>"
  char icomm_server_[16]{};          // runtime iComm server (set_icomm_server), dotted IPv4
  int icomm_fd_{-1};
  bool icomm_connecting_{false};
  bool icomm_ready_{false};
  uint32_t icomm_last_try_{0};
  uint32_t icomm_connect_started_{0};
  uint32_t icomm_invoke_{1};
  uint32_t icomm_last_rx_{0};
  size_t icomm_advise_next_{SIZE_MAX};  // next point index to subscribe after connecting
  bool icomm_advise_waiting_{false};    // an advise batch is out and iComm hasn't replied yet
  uint32_t icomm_raw_invoke_{0};        // invoke id of the raw request in flight
  uint32_t icomm_advise_sent_at_{0};
  uint32_t serial_probe_at_{0};
  uint8_t ic_rx_[IC_RX_BUF];
  size_t ic_rx_len_{0};
  uint8_t ic_tx_[IC_TX_BUF];
  size_t ic_tx_len_{0};
};

#ifdef USE_NUMBER
class GenesisNumber : public number::Number, public Component {
 public:
  void set_bus(GenesisBus *bus) { this->bus_ = bus; }
  void set_target(uint8_t address, uint8_t block) {
    this->address_ = address;
    this->block_ = block;
  }
  void dump_config() override;

 protected:
  void control(float value) override;
  GenesisBus *bus_{nullptr};
  uint8_t address_{0};
  uint8_t block_{0};
};
#endif

#ifdef USE_CLIMATE
// One zone: current temperature (sensor block), target (User Variable setpoint block), and optionally the
// AHU's chilled-water valve, supply-fan status and the zone's duct-heater stages for the action
// (off / heating / cooling / fan). Heating vs cooling is decided by the controller's own zone loop. Auto = the panel's schedules decide when
// the AHU runs. With fan_output_block set, Off forces the AHU's supply-fan output off (a manual override,
// shared by every zone on that AHU) and Auto releases it. A force-on (separate button, or made in Supervisor)
// shows as Fan only; it can't be selected here, and Auto or Off replaces it.
class GenesisClimate : public climate::Climate, public Component {
 public:
  enum Role : uint8_t {
    ROLE_CURRENT = 1,
    ROLE_TARGET = 2,
    ROLE_VALVE = 3,
    ROLE_FAN = 4,
    ROLE_FORCED = 5,
    ROLE_FORCE_VALUE = 6,
    ROLE_HEATER_1 = 7,  // zone electric duct heater stages (EDH n/1, n/2)
    ROLE_HEATER_2 = 8,
  };
  static constexpr uint8_t MAX_HEATERS = 2;

  void set_bus(GenesisBus *bus) { this->bus_ = bus; }
  void set_target(uint8_t address, uint8_t setpoint_block) {
    this->address_ = address;
    this->setpoint_block_ = setpoint_block;
  }
  void set_has_valve(bool v) { this->has_valve_ = v; }
  void set_has_fan(bool v) { this->has_fan_ = v; }
  void set_fan_output_block(uint8_t block) { this->fan_output_block_ = block; }
  void set_has_heater(bool v) { this->has_heater_ = v; }
  void setup() override;
  void dump_config() override;
  void on_point(uint8_t role, float value, bool valid);

 protected:
  climate::ClimateTraits traits() override;
  void control(const climate::ClimateCall &call) override;
  climate::ClimateAction compute_action_() const;
  climate::ClimateMode mode_from_override_() const;

  GenesisBus *bus_{nullptr};
  uint8_t address_{0};
  uint8_t setpoint_block_{0};
  uint8_t fan_output_block_{0};
  bool has_valve_{false};
  bool has_fan_{false};
  bool has_heater_{false};
  float valve_{NAN};
  int8_t fan_{-1};
  int8_t heater_[MAX_HEATERS]{-1, -1};
  int8_t forced_{-1};       // fan output Forced flag (-1 = not read yet)
  int8_t force_value_{-1};  // fan output ForceValue
  uint32_t last_write_{0};
  uint32_t last_mode_write_{0};
  bool target_written_{false};  // the settle windows only apply after our own writes
  bool mode_written_{false};
};
#endif

#ifdef USE_BUTTON
class GenesisScanButton : public button::Button, public Component {
 public:
  void set_bus(GenesisBus *bus) { this->bus_ = bus; }
  void set_address(uint8_t address) { this->address_ = address; }

 protected:
  void press_action() override { this->bus_->scan_blocks(this->address_); }
  GenesisBus *bus_{nullptr};
  uint8_t address_{0};
};

// Forces a Digital Output (an AHU supply fan) ON, overriding its schedule, switch and fire-alarm logic.
// Release it by setting one of that AHU's climate zones back to Auto.
class GenesisForceOnButton : public button::Button, public Component {
 public:
  void set_bus(GenesisBus *bus) { this->bus_ = bus; }
  void set_target(uint8_t address, uint8_t block) {
    this->address_ = address;
    this->block_ = block;
  }

 protected:
  void press_action() override { this->bus_->force_output(this->address_, this->block_, FORCE_ON); }
  GenesisBus *bus_{nullptr};
  uint8_t address_{0};
  uint8_t block_{0};
};
#endif

}  // namespace innotech_genesis
}  // namespace esphome
