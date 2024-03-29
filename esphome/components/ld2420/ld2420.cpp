#include "ld2420.h"
#include "esphome/core/helpers.h"

/*
Configure commands - little endian

No command can exceed 64 bytes, otherwise they would need be to be split up into multiple sends.

All send command frames will have:
  Header = FD FC FB FA, Bytes 0 - 3, uint32_t 0xFAFBFCFD
  Length, bytes 4 - 5, uint16_t 0x0002, must be at least 2 for the command byte if no addon data.
  Command bytes 6 - 7, uint16_t
  Footer = 04 03 02 01 - uint32_t 0x01020304, Always last 4 Bytes.
Receive
  Error bytes 8-9 uint16_t, 0 = success, all other positive values = error

Enable config mode:
Send:
  UART Tx: FD FC FB FA 04 00 FF 00 02 00 04 03 02 01
  Command = FF 00 - uint16_t 0x00FF
  Protocol version = 02 00, can be 1 or 2 - uint16_t 0x0002
Reply:
  UART Rx: FD FC FB FA 06 00 FF 01 00 00 02 00 04 03 02 01

Disable config mode:
Send:
  UART Tx: FD FC FB FA 02 00 FE 00 04 03 02 01
  Command = FE 00 - uint16_t 0x00FE
Receive:
  UART Rx: FD FC FB FA 04 00 FE 01 00 00 04 03 02 01

Configure system parameters:

UART Tx: FD FC FB FA 08 00 12 00 00 00 64 00 00 00 04 03 02 01  Set system parms
Command = 12 00 - uint16_t 0x0012, Param
There are three documented parameters for modes:
  00 64 = Basic status mode
    This mode outputs text as presence "ON" or  "OFF" and "Range XXXX"
    where XXXX is a decimal value for distance in cm
  00 04 = Energy output mode
    This mode outputs detailed signal energy values for each gate and the target distance.
    The data format consist of the following.
    Header HH, Length LL, Persence PP, Distance DD, Range Gate GG, 16 Gate Energies EE, Footer FF
    HH HH HH HH LL LL PP DD DD GG GG EE EE .. 16x   .. FF FF FF FF
    F4 F3 F2 F1 00 23 00 00 00 00 01 00 00 .. .. .. .. F8 F7 F6 F5
  00 00 = debug output mode
    This mode outputs detailed values consisting of 20 Dopplers, 16 Ranges for a total 20 * 16 * 4 bytes
    The data format consist of the following.
    Header HH, Doppler DD, Range RR, Footer FF
    HH HH HH HH DD DD DD DD .. 20x   .. RR RR RR RR .. 16x   .. FF FF FF FF
    AA BF 10 14 00 00 00 00 .. .. .. .. 00 00 00 00 .. .. .. .. FD FC FB FA

Configure gate sensitivity parameters:
UART Tx: FD FC FB FA 0E 00 07 00 10 00 60 EA 00 00 20 00 60 EA 00 00 04 03 02 01
Command = 12 00 - uint16_t 0x0007
Gate 0 high thresh = 10 00 uint16_t 0x0010, Threshold value = 60 EA 00 00 uint32_t 0x0000EA60
Gate 0 low thresh = 20 00 uint16_t 0x0020, Threshold value = 60 EA 00 00 uint32_t 0x0000EA60
*/

namespace esphome {
namespace ld2420 {

static const char *const TAG = "ld2420";

float LD2420Component::get_setup_priority() const { return setup_priority::BUS; }

void LD2420Component::dump_config() {
  ESP_LOGCONFIG(TAG, "LD2420:");
  ESP_LOGCONFIG(TAG, "  Firmware Version : %7s", this->ld2420_firmware_ver_);
  ESP_LOGCONFIG(TAG, "LD2420 Number:");
  LOG_NUMBER(TAG, "  Gate Timeout:", this->gate_timeout_number_);
  LOG_NUMBER(TAG, "  Gate Max Distance:", this->max_gate_distance_number_);
  LOG_NUMBER(TAG, "  Gate Min Distance:", this->min_gate_distance_number_);
  LOG_NUMBER(TAG, "  Gate Select:", this->gate_select_number_);
  LOG_NUMBER(TAG, "  Gate Move Threshold:", this->gate_move_threshold_number_);
  LOG_NUMBER(TAG, "  Gate Still Threshold::", this->gate_still_threshold_number_);
  LOG_BUTTON(TAG, "  Apply Config:", this->apply_config_button_);
  LOG_BUTTON(TAG, "  Revert Edits:", this->revert_config_button_);
  LOG_BUTTON(TAG, "  Factory Reset:", this->factory_reset_button_);
  LOG_BUTTON(TAG, "  Restart Module:", this->restart_module_button_);
  ESP_LOGCONFIG(TAG, "LD2420 Select:");
  LOG_SELECT(TAG, "  Operating Mode", this->operating_selector_);
  if (this->get_firmware_int_(ld2420_firmware_ver_) < CALIBRATE_VERSION_MIN) {
    ESP_LOGW(TAG, "LD2420 Firmware Version %s and older are only supported in Simple Mode", ld2420_firmware_ver_);
  }
}

uint8_t LD2420Component::calc_checksum(void *data, size_t size) {
  uint8_t checksum = 0;
  uint8_t *data_bytes = (uint8_t *) data;
  for (size_t i = 0; i < size; i++) {
    checksum ^= data_bytes[i];  // XOR operation
  }
  return checksum;
}

int LD2420Component::get_firmware_int_(const char *version_string) {
  std::string version_str = version_string;
  if (version_str[0] == 'v') {
    version_str = version_str.substr(1);
  }
  version_str.erase(remove(version_str.begin(), version_str.end(), '.'), version_str.end());
  int version_integer = stoi(version_str);
  return version_integer;
}

void LD2420Component::setup() {
  ESP_LOGCONFIG(TAG, "Setting up LD2420...");
  if (this->set_config_mode(true) == LD2420_ERROR_TIMEOUT) {
    ESP_LOGE(TAG, "LD2420 module has failed to respond, check baud rate and serial connections.");
    this->mark_failed();
    return;
  }
  this->get_min_max_distances_timeout_();
#ifdef USE_NUMBER
  this->init_gate_config_numbers();
#endif
  this->get_firmware_version_();
  const char *pfw = ld2420_firmware_ver_;
  std::string fw_str(pfw);

  for (auto &listener : listeners_) {
    listener->on_fw_version(fw_str);
  }

  for (uint8_t gate = 0; gate < LD2420_TOTAL_GATES; gate++) {
    delay_microseconds_safe(65);
    this->get_gate_threshold_(gate);
  }

  memcpy(&new_config_, &current_config_, sizeof(current_config_));
  if (get_firmware_int_(ld2420_firmware_ver_) < CALIBRATE_VERSION_MIN) {
    this->set_operating_mode(OP_SIMPLE_MODE_STRING);
    this->operating_selector_->publish_state(OP_SIMPLE_MODE_STRING);
    this->set_mode_(CMD_SYSTEM_MODE_SIMPLE);
    ESP_LOGW(TAG, "LD2420 Frimware Version %s and older are only supported in Simple Mode", ld2420_firmware_ver_);
  } else {
    this->set_mode_(CMD_SYSTEM_MODE_ENERGY);
    this->operating_selector_->publish_state(OP_NORMAL_MODE_STRING);
  }
#ifdef USE_NUMBER
  this->init_gate_config_numbers();
#endif
  this->set_system_mode(this->system_mode_);
  this->set_config_mode(false);
  ESP_LOGCONFIG(TAG, "LD2420 setup complete.");
}

void LD2420Component::apply_config_action() {
  const uint8_t checksum = calc_checksum(&new_config_, sizeof(new_config_));
  if (checksum == calc_checksum(&current_config_, sizeof(current_config_))) {
    ESP_LOGCONFIG(TAG, "No configuration change detected");
    return;
  }
  ESP_LOGCONFIG(TAG, "Reconfiguring LD2420...");
  if (this->set_config_mode(true) == LD2420_ERROR_TIMEOUT) {
    ESP_LOGE(TAG, "LD2420 module has failed to respond, check baud rate and serial connections.");
    this->mark_failed();
    return;
  }
  if (this->new_config_.max_gate != this->current_config_.max_gate ||
      this->new_config_.min_gate != this->current_config_.min_gate ||
      this->new_config_.timeout != this->current_config_.timeout) {
    this->set_min_max_distances_timeout(this->new_config_.max_gate, this->new_config_.min_gate,
                                        this->new_config_.timeout);
  };

  for (uint8_t gate = 0; gate < LD2420_TOTAL_GATES; gate++) {
    if (this->new_config_.still_thresh[gate] != this->current_config_.still_thresh[gate] ||
        this->new_config_.move_thresh[gate] != this->current_config_.move_thresh[gate])
      this->set_gate_threshold(gate);
    delay_microseconds_safe(65);
  }
  memcpy(&current_config_, &new_config_, sizeof(new_config_));
  this->set_system_mode(this->system_mode_);
  this->set_config_mode(false);  // Disable config mode to save new values in LD2420 nvm
#ifdef USE_NUMBER
  this->init_gate_config_numbers();
#endif
  this->set_operating_mode(OP_NORMAL_MODE_STRING);
  ESP_LOGCONFIG(TAG, "LD2420 reconfig complete.");
}

void LD2420Component::factory_reset_action() {
  ESP_LOGCONFIG(TAG, "Setiing factory defaults...");
  if (this->set_config_mode(true) == LD2420_ERROR_TIMEOUT) {
    ESP_LOGE(TAG, "LD2420 module has failed to respond, check baud rate and serial connections.");
    this->mark_failed();
    return;
  }

  this->set_min_max_distances_timeout(FACTORY_MAX_GATE, FACTORY_MIN_GATE, FACTORY_TIMEOUT);
  for (uint8_t gate = 0; gate < LD2420_TOTAL_GATES; gate++) {
    new_config_.move_thresh[gate] = FACTORY_MOVE_THRESH[gate];
    new_config_.still_thresh[gate] = FACTORY_STILL_THRESH[gate];
    delay_microseconds_safe(65);
    this->set_gate_threshold(gate);
  }
  memcpy(&current_config_, &new_config_, sizeof(new_config_));
#ifdef USE_NUMBER
  this->init_gate_config_numbers();
#endif
  this->set_system_mode(this->system_mode_);
  this->set_config_mode(false);
  this->send_module_restart();
  ESP_LOGCONFIG(TAG, "LD2420 factory reset complete.");
}

void LD2420Component::restart_module_action() {
  ESP_LOGCONFIG(TAG, "Restarting LD2420 module...");
  this->send_module_restart();
  ESP_LOGCONFIG(TAG, "LD2420 Restarted.");
  delay_microseconds_safe(25000);
  this->set_config_mode(true);
  this->set_system_mode(this->system_mode_);
  this->set_config_mode(false);
}

void LD2420Component::revert_config_action() {
  memcpy(&new_config_, &current_config_, sizeof(current_config_));
#ifdef USE_NUMBER
  this->init_gate_config_numbers();
#endif
  ESP_LOGCONFIG(TAG, "Reverted config number edits.");
}

void LD2420Component::loop() {
  // If there is a active send command do not process it here, the send command call will handle it.
  if (!get_cmd_active_()) {
    if (!available())
      return;
    static uint8_t buffer[2048];
    static uint8_t rx_data;
    while (available()) {
      rx_data = read();
      this->readline_(rx_data, buffer, sizeof(buffer));
    }
  }
}

void LD2420Component::update_radar_data(uint16_t const *gate_energy, uint8_t sample_number) {
  for (uint8_t gate = 0; gate < LD2420_TOTAL_GATES; ++gate) {
    radar_data[gate][sample_number] = gate_energy[gate];
  }
  total_sample_number_counter++;
}

void LD2420Component::auto_calibrate_sensitivity() {
  // Calculate average and peak values for each gate
  for (uint8_t gate = 0; gate < LD2420_TOTAL_GATES; ++gate) {
    uint32_t sum = 0;
    uint16_t peak = 0;

    for (uint8_t sample_number = 0; sample_number < CALIBRATE_SAMPLES; ++sample_number) {
      // Calculate average
      sum += radar_data[gate][sample_number];

      // Calculate max value
      if (radar_data[gate][sample_number] > peak) {
        peak = radar_data[gate][sample_number];
      }
    }

    // Store average and peak values
    gate_avg[gate] = sum / CALIBRATE_SAMPLES;
    if (gate_peak[gate] < peak)
      gate_peak[gate] = peak;
    uint32_t calculated_value = static_cast<uint32_t>(gate_peak[gate]) * CALIBRATE_MOVE_FACTOR + CALIBRATE_MOVE_BASE;
    this->new_config_.move_thresh[gate] = static_cast<uint16_t>(calculated_value <= 65535 ? calculated_value : 65535);
    calculated_value = static_cast<uint32_t>(gate_peak[gate]) * CALIBRATE_STILL_FACTOR + CALIBRATE_STILL_BASE;
    this->new_config_.still_thresh[gate] = static_cast<uint16_t>(calculated_value <= 65535 ? calculated_value : 65535);
  }
}

void LD2420Component::report_gate_data() {
  for (uint8_t gate = 0; gate < LD2420_TOTAL_GATES; ++gate) {
    // Output results
    ESP_LOGI(TAG, "Gate: %2d Avg: %5d Peak: %5d", gate, gate_avg[gate], gate_peak[gate]);
  }
  ESP_LOGI(TAG, "Total samples: %d", total_sample_number_counter);
}

void LD2420Component::set_operating_mode(const std::string &state) {
  // If unsupported firmware ignore mode select
  if (get_firmware_int_(ld2420_firmware_ver_) >= CALIBRATE_VERSION_MIN) {
    this->current_operating_mode = OP_MODE_TO_UINT.at(state);
    // Entering Auto Calibrate we need to clear the privoiuos data collection
    this->operating_selector_->publish_state(state);
    if (current_operating_mode == OP_CALIBRATE_MODE) {
      ESP_LOGI(TAG, "Calibration active");
      this->set_calibration(true);
      for (uint8_t gate = 0; gate < LD2420_TOTAL_GATES; gate++) {
        this->gate_avg[gate] = 0;
        this->gate_peak[gate] = 0;
        for (uint8_t i = 0; i < CALIBRATE_SAMPLES; i++) {
          this->radar_data[gate][i] = 0;
        }
        this->total_sample_number_counter = 0;
      }
    } else {
      // Set the current data back so we don't have new data that can be applied in error.
      if (get_calibration())
        memcpy(&new_config_, &current_config_, sizeof(current_config_));
      this->set_calibration(false);
    }
  } else {
    this->current_operating_mode = OP_SIMPLE_MODE;
    this->operating_selector_->publish_state(OP_SIMPLE_MODE_STRING);
  }
}

void LD2420Component::readline_(int rx_data, uint8_t *buffer, int len) {
  static int pos = 0;

  if (rx_data >= 0) {
    if (pos < len - 1) {
      buffer[pos++] = rx_data;
      buffer[pos] = 0;
    } else {
      pos = 0;
    }
    if (pos >= 4) {
      if (memcmp(&buffer[pos - 4], &CMD_FRAME_FOOTER, sizeof(CMD_FRAME_FOOTER)) == 0) {
        this->set_cmd_active_(false);  // Set command state to inactive after responce.
        this->handle_ack_data_(buffer, pos);
        pos = 0;
      } else if ((buffer[pos - 2] == 0x0D && buffer[pos - 1] == 0x0A) && (get_mode_() == CMD_SYSTEM_MODE_SIMPLE)) {
        this->handle_simple_mode_(buffer, pos);
        pos = 0;
      } else if ((memcmp(&buffer[pos - 4], &ENERGY_FRAME_FOOTER, sizeof(ENERGY_FRAME_FOOTER)) == 0) &&
                 (get_mode_() == CMD_SYSTEM_MODE_ENERGY)) {
        this->handle_energy_mode_(buffer, pos);
        pos = 0;
      }
    }
  }
}

void LD2420Component::handle_energy_mode_(uint8_t *buffer, int len) {
  uint8_t index = 6;  // Start at presence byte position
  uint16_t range;
  const uint8_t elements = sizeof(gate_energy_) / sizeof(gate_energy_[0]);
  this->set_presence_(buffer[index]);
  index++;
  memcpy(&range, &buffer[index], sizeof(range));
  index += sizeof(range);
  this->set_distance_(range);
  for (uint8_t i = 0; i < elements; i++) {  // NOLINT
    memcpy(&gate_energy_[i], &buffer[index], sizeof(gate_energy_[0]));
    index += sizeof(gate_energy_[0]);
  }

  if ((current_operating_mode == OP_CALIBRATE_MODE)) {
    this->update_radar_data(gate_energy_, sample_number_counter);
    this->sample_number_counter > CALIBRATE_SAMPLES ? this->sample_number_counter = 0 : this->sample_number_counter++;
  }

  // Resonable refresh rate for home assistant database size health
  const int32_t current_millis = millis();
  if (current_millis - last_periodic_millis < REFRESH_RATE_MS)
    return;
  last_periodic_millis = current_millis;
  for (auto &listener : this->listeners_) {
    listener->on_distance(get_distance_());
    listener->on_presence(get_presence_());
    listener->on_energy(gate_energy_, sizeof(gate_energy_) / sizeof(gate_energy_[0]));
  }

  if ((current_operating_mode == OP_CALIBRATE_MODE)) {
    this->auto_calibrate_sensitivity();
    if (current_millis - report_periodic_millis > REFRESH_RATE_MS * CALIBRATE_REPORT_INTERVAL) {
      report_periodic_millis = current_millis;
      this->report_gate_data();
    }
  }
}

void LD2420Component::handle_simple_mode_(const uint8_t *inbuf, int len) {
  const uint8_t bufsize = 16;
  uint8_t index{0};
  uint8_t pos{0};
  char *endptr{nullptr};
  char outbuf[bufsize]{0};
  while (true) {
    if (inbuf[pos - 2] == 'O' && inbuf[pos - 1] == 'F' && inbuf[pos] == 'F') {
      set_presence_(false);
    } else if (inbuf[pos - 1] == 'O' && inbuf[pos] == 'N') {
      set_presence_(true);
    }
    if (inbuf[pos] >= '0' && inbuf[pos] <= '9') {
      if (index < bufsize - 1) {
        outbuf[index++] = inbuf[pos];
        pos++;
      }
    } else {
      if (pos < len - 1) {
        pos++;
      } else {
        break;
      }
    }
  }
  outbuf[index] = '\0';
  if (index > 1)
    set_distance_(strtol(outbuf, &endptr, 10));

  if (get_mode_() == CMD_SYSTEM_MODE_SIMPLE) {
    // Resonable refresh rate for home assistant database size health
    const int32_t current_millis = millis();
    if (current_millis - last_normal_periodic_millis < REFRESH_RATE_MS)
      return;
    last_normal_periodic_millis = current_millis;
    for (auto &listener : this->listeners_)
      listener->on_distance(get_distance_());
    for (auto &listener : this->listeners_)
      listener->on_presence(get_presence_());
  }
}

void LD2420Component::handle_ack_data_(uint8_t *buffer, int len) {
  cmd_reply_.command = buffer[CMD_FRAME_COMMAND];
  cmd_reply_.length = buffer[CMD_FRAME_DATA_LENGTH];
  uint8_t reg_element = 0;
  uint8_t data_element = 0;
  uint16_t data_pos = 0;
  if (cmd_reply_.length > CMD_MAX_BYTES) {
    ESP_LOGW(TAG, "LD2420 reply - received frame is corrupt, data length exceeds 64 bytes.");
    return;
  } else if (cmd_reply_.length < 2) {
    ESP_LOGW(TAG, "LD2420 reply - received frame is corrupt, data length is less than 2 bytes.");
    return;
  }
  memcpy(&cmd_reply_.error, &buffer[CMD_ERROR_WORD], sizeof(cmd_reply_.error));
  const char *result = cmd_reply_.error ? "failure" : "success";
  if (cmd_reply_.error > 0) {
    return;
  };
  cmd_reply_.ack = true;
  switch ((uint16_t) cmd_reply_.command) {
    case (CMD_ENABLE_CONF):
      ESP_LOGD(TAG, "LD2420 reply - set config enable: CMD = %2X %s", CMD_ENABLE_CONF, result);
      break;
    case (CMD_DISABLE_CONF):
      ESP_LOGD(TAG, "LD2420 reply - set config disable: CMD = %2X %s", CMD_DISABLE_CONF, result);
      break;
    case (CMD_READ_REGISTER):
      ESP_LOGD(TAG, "LD2420 reply - read register: CMD = %2X %s", CMD_READ_REGISTER, result);
      // TODO Read/Write register is not implemented yet, this will get flushed out to a proper header file
      data_pos = 0x0A;
      for (uint16_t index = 0; index < (CMD_REG_DATA_REPLY_SIZE *  // NOLINT
                                        ((buffer[CMD_FRAME_DATA_LENGTH] - 4) / CMD_REG_DATA_REPLY_SIZE));
           index += CMD_REG_DATA_REPLY_SIZE) {
        memcpy(&cmd_reply_.data[reg_element], &buffer[data_pos + index], sizeof(CMD_REG_DATA_REPLY_SIZE));
        byteswap(cmd_reply_.data[reg_element]);
        reg_element++;
      }
      break;
    case (CMD_WRITE_REGISTER):
      ESP_LOGD(TAG, "LD2420 reply - write register: CMD = %2X %s", CMD_WRITE_REGISTER, result);
      break;
    case (CMD_WRITE_ABD_PARAM):
      ESP_LOGD(TAG, "LD2420 reply - write gate parameter(s): %2X %s", CMD_WRITE_ABD_PARAM, result);
      break;
    case (CMD_READ_ABD_PARAM):
      ESP_LOGD(TAG, "LD2420 reply - read gate parameter(s): %2X %s", CMD_READ_ABD_PARAM, result);
      data_pos = CMD_ABD_DATA_REPLY_START;
      for (uint16_t index = 0; index < (CMD_ABD_DATA_REPLY_SIZE *  // NOLINT
                                        ((buffer[CMD_FRAME_DATA_LENGTH] - 4) / CMD_ABD_DATA_REPLY_SIZE));
           index += CMD_ABD_DATA_REPLY_SIZE) {
        memcpy(&cmd_reply_.data[data_element], &buffer[data_pos + index], sizeof(cmd_reply_.data[data_element]));
        byteswap(cmd_reply_.data[data_element]);
        data_element++;
      }
      break;
    case (CMD_WRITE_SYS_PARAM):
      ESP_LOGD(TAG, "LD2420 reply - set system parameter(s): %2X %s", CMD_WRITE_SYS_PARAM, result);
      break;
    case (CMD_READ_VERSION):
      memcpy(this->ld2420_firmware_ver_, &buffer[12], buffer[10]);
      ESP_LOGD(TAG, "LD2420 reply - module firmware version: %7s %s", this->ld2420_firmware_ver_, result);
      break;
    default:
      break;
  }
}

int LD2420Component::send_cmd_from_array(CmdFrameT frame) {
  uint8_t error = 0;
  uint8_t ack_buffer[64];
  uint8_t cmd_buffer[64];
  uint16_t loop_count;
  cmd_reply_.ack = false;
  this->set_cmd_active_(true);
  uint8_t retry = 3;
  while (retry) {
    // TODO setup a dynamic method e.g. millis time count etc. to tune for non ESP32 240Mhz devices
    // this is ok for now since the module firmware is changing like the weather atm
    frame.length = 0;
    loop_count = 1250;
    uint16_t frame_data_bytes = frame.data_length + 2;  // Always add two bytes for the cmd size

    memcpy(&cmd_buffer[frame.length], &frame.header, sizeof(frame.header));
    frame.length += sizeof(frame.header);

    memcpy(&cmd_buffer[frame.length], &frame_data_bytes, sizeof(frame.data_length));
    frame.length += sizeof(frame.data_length);

    memcpy(&cmd_buffer[frame.length], &frame.command, sizeof(frame.command));
    frame.length += sizeof(frame.command);

    for (uint16_t index = 0; index < frame.data_length; index++) {
      memcpy(&cmd_buffer[frame.length], &frame.data[index], sizeof(frame.data[index]));
      frame.length += sizeof(frame.data[index]);
    }

    memcpy(cmd_buffer + frame.length, &frame.footer, sizeof(frame.footer));
    frame.length += sizeof(frame.footer);
    for (uint16_t index = 0; index < frame.length; index++) {
      this->write_byte(cmd_buffer[index]);
    }

    delay_microseconds_safe(500);  // give the module a moment to process it
    error = 0;
    if (frame.command == CMD_RESTART)
      return 0;  // restart does not reply exit now

    while (!cmd_reply_.ack) {
      while (available()) {
        this->readline_(read(), ack_buffer, sizeof(ack_buffer));
      }
      delay_microseconds_safe(250);
      if (loop_count <= 0) {
        error = LD2420_ERROR_TIMEOUT;
        retry--;
        break;
      }
      loop_count--;
    }
    if (cmd_reply_.ack)
      retry = 0;
    if (cmd_reply_.error > 0)
      handle_cmd_error(error);
  }
  return error;
}

uint8_t LD2420Component::set_config_mode(bool enable) {
  CmdFrameT cmd_frame;
  cmd_frame.data_length = 0;
  cmd_frame.header = CMD_FRAME_HEADER;
  cmd_frame.command = enable ? CMD_ENABLE_CONF : CMD_DISABLE_CONF;
  if (enable) {
    memcpy(&cmd_frame.data[0], &CMD_PROTOCOL_VER, sizeof(CMD_PROTOCOL_VER));
    cmd_frame.data_length += sizeof(CMD_PROTOCOL_VER);
  }
  cmd_frame.footer = CMD_FRAME_FOOTER;
  ESP_LOGD(TAG, "Sending set config %s command: %2X", enable ? "enable" : "disable", cmd_frame.command);
  return this->send_cmd_from_array(cmd_frame);
}

// Sends a restart and set system running mode to normal
void LD2420Component::send_module_restart() { this->ld2420_restart(); }

void LD2420Component::ld2420_restart() {
  CmdFrameT cmd_frame;
  cmd_frame.data_length = 0;
  cmd_frame.header = CMD_FRAME_HEADER;
  cmd_frame.command = CMD_RESTART;
  cmd_frame.footer = CMD_FRAME_FOOTER;
  ESP_LOGD(TAG, "Sending restart command: %2X", cmd_frame.command);
  this->send_cmd_from_array(cmd_frame);
}

void LD2420Component::get_reg_value_(uint16_t reg) {
  CmdFrameT cmd_frame;
  cmd_frame.data_length = 0;
  cmd_frame.header = CMD_FRAME_HEADER;
  cmd_frame.command = CMD_READ_REGISTER;
  cmd_frame.data[1] = reg;
  cmd_frame.data_length += 2;
  cmd_frame.footer = CMD_FRAME_FOOTER;
  ESP_LOGD(TAG, "Sending read register %4X command: %2X", reg, cmd_frame.command);
  this->send_cmd_from_array(cmd_frame);
}

void LD2420Component::set_reg_value(uint16_t reg, uint16_t value) {
  CmdFrameT cmd_frame;
  cmd_frame.data_length = 0;
  cmd_frame.header = CMD_FRAME_HEADER;
  cmd_frame.command = CMD_WRITE_REGISTER;
  memcpy(&cmd_frame.data[cmd_frame.data_length], &reg, sizeof(CMD_REG_DATA_REPLY_SIZE));
  cmd_frame.data_length += 2;
  memcpy(&cmd_frame.data[cmd_frame.data_length], &value, sizeof(CMD_REG_DATA_REPLY_SIZE));
  cmd_frame.data_length += 2;
  cmd_frame.footer = CMD_FRAME_FOOTER;
  ESP_LOGD(TAG, "Sending write register %4X command: %2X data = %4X", reg, cmd_frame.command, value);
  this->send_cmd_from_array(cmd_frame);
}

void LD2420Component::handle_cmd_error(uint8_t error) { ESP_LOGI(TAG, "Command failed: %s", ERR_MESSAGE[error]); }

int LD2420Component::get_gate_threshold_(uint8_t gate) {
  uint8_t error;
  CmdFrameT cmd_frame;
  cmd_frame.data_length = 0;
  cmd_frame.header = CMD_FRAME_HEADER;
  cmd_frame.command = CMD_READ_ABD_PARAM;
  memcpy(&cmd_frame.data[cmd_frame.data_length], &CMD_GATE_MOVE_THRESH[gate], sizeof(CMD_GATE_MOVE_THRESH[gate]));
  cmd_frame.data_length += 2;
  memcpy(&cmd_frame.data[cmd_frame.data_length], &CMD_GATE_STILL_THRESH[gate], sizeof(CMD_GATE_STILL_THRESH[gate]));
  cmd_frame.data_length += 2;
  cmd_frame.footer = CMD_FRAME_FOOTER;
  ESP_LOGD(TAG, "Sending read gate %d high/low theshold command: %2X", gate, cmd_frame.command);
  error = this->send_cmd_from_array(cmd_frame);
  if (error == 0) {
    current_config_.move_thresh[gate] = cmd_reply_.data[0];
    current_config_.still_thresh[gate] = cmd_reply_.data[1];
  }
  return error;
}

int LD2420Component::get_min_max_distances_timeout_() {
  uint8_t error;
  CmdFrameT cmd_frame;
  cmd_frame.data_length = 0;
  cmd_frame.header = CMD_FRAME_HEADER;
  cmd_frame.command = CMD_READ_ABD_PARAM;
  memcpy(&cmd_frame.data[cmd_frame.data_length], &CMD_MIN_GATE_REG,
         sizeof(CMD_MIN_GATE_REG));  // Register: global min detect gate number
  cmd_frame.data_length += sizeof(CMD_MIN_GATE_REG);
  memcpy(&cmd_frame.data[cmd_frame.data_length], &CMD_MAX_GATE_REG,
         sizeof(CMD_MAX_GATE_REG));  // Register: global max detect gate number
  cmd_frame.data_length += sizeof(CMD_MAX_GATE_REG);
  memcpy(&cmd_frame.data[cmd_frame.data_length], &CMD_TIMEOUT_REG,
         sizeof(CMD_TIMEOUT_REG));  // Register: global delay time
  cmd_frame.data_length += sizeof(CMD_TIMEOUT_REG);
  cmd_frame.footer = CMD_FRAME_FOOTER;
  ESP_LOGD(TAG, "Sending read gate min max and timeout command: %2X", cmd_frame.command);
  error = this->send_cmd_from_array(cmd_frame);
  if (error == 0) {
    current_config_.min_gate = (uint16_t) cmd_reply_.data[0];
    current_config_.max_gate = (uint16_t) cmd_reply_.data[1];
    current_config_.timeout = (uint16_t) cmd_reply_.data[2];
  }
  return error;
}

void LD2420Component::set_system_mode(uint16_t mode) {
  CmdFrameT cmd_frame;
  uint16_t unknown_parm = 0x0000;
  cmd_frame.data_length = 0;
  cmd_frame.header = CMD_FRAME_HEADER;
  cmd_frame.command = CMD_WRITE_SYS_PARAM;
  memcpy(&cmd_frame.data[cmd_frame.data_length], &CMD_SYSTEM_MODE, sizeof(CMD_SYSTEM_MODE));
  cmd_frame.data_length += sizeof(CMD_SYSTEM_MODE);
  memcpy(&cmd_frame.data[cmd_frame.data_length], &mode, sizeof(mode));
  cmd_frame.data_length += sizeof(mode);
  memcpy(&cmd_frame.data[cmd_frame.data_length], &unknown_parm, sizeof(unknown_parm));
  cmd_frame.data_length += sizeof(unknown_parm);
  cmd_frame.footer = CMD_FRAME_FOOTER;
  ESP_LOGD(TAG, "Sending write system mode command: %2X", cmd_frame.command);
  if (this->send_cmd_from_array(cmd_frame) == 0)
    set_mode_(mode);
}

void LD2420Component::get_firmware_version_() {
  CmdFrameT cmd_frame;
  cmd_frame.data_length = 0;
  cmd_frame.header = CMD_FRAME_HEADER;
  cmd_frame.command = CMD_READ_VERSION;
  cmd_frame.footer = CMD_FRAME_FOOTER;

  ESP_LOGD(TAG, "Sending read firmware version command: %2X", cmd_frame.command);
  this->send_cmd_from_array(cmd_frame);
}

void LD2420Component::set_min_max_distances_timeout(uint32_t max_gate_distance, uint32_t min_gate_distance,  // NOLINT
                                                    uint32_t timeout) {
  // Header H, Length L, Register R, Value V, Footer F
  //                        |Min Gate         |Max Gate         |Timeout          |
  // HH HH HH HH LL LL CC CC RR RR VV VV VV VV RR RR VV VV VV VV RR RR VV VV VV VV FF FF FF FF
  // FD FC FB FA 14 00 07 00 00 00 01 00 00 00 01 00 09 00 00 00 04 00 0A 00 00 00 04 03 02 01 e.g.

  CmdFrameT cmd_frame;
  cmd_frame.data_length = 0;
  cmd_frame.header = CMD_FRAME_HEADER;
  cmd_frame.command = CMD_WRITE_ABD_PARAM;
  memcpy(&cmd_frame.data[cmd_frame.data_length], &CMD_MIN_GATE_REG,
         sizeof(CMD_MIN_GATE_REG));  // Register: global min detect gate number
  cmd_frame.data_length += sizeof(CMD_MIN_GATE_REG);
  memcpy(&cmd_frame.data[cmd_frame.data_length], &min_gate_distance, sizeof(min_gate_distance));
  cmd_frame.data_length += sizeof(min_gate_distance);
  memcpy(&cmd_frame.data[cmd_frame.data_length], &CMD_MAX_GATE_REG,
         sizeof(CMD_MAX_GATE_REG));  // Register: global max detect gate number
  cmd_frame.data_length += sizeof(CMD_MAX_GATE_REG);
  memcpy(&cmd_frame.data[cmd_frame.data_length], &max_gate_distance, sizeof(max_gate_distance));
  cmd_frame.data_length += sizeof(max_gate_distance);
  memcpy(&cmd_frame.data[cmd_frame.data_length], &CMD_TIMEOUT_REG,
         sizeof(CMD_TIMEOUT_REG));  // Register: global delay time
  cmd_frame.data_length += sizeof(CMD_TIMEOUT_REG);
  memcpy(&cmd_frame.data[cmd_frame.data_length], &timeout, sizeof(timeout));
  ;
  cmd_frame.data_length += sizeof(timeout);
  cmd_frame.footer = CMD_FRAME_FOOTER;

  ESP_LOGD(TAG, "Sending write gate min max and timeout command: %2X", cmd_frame.command);
  this->send_cmd_from_array(cmd_frame);
}

void LD2420Component::set_gate_threshold(uint8_t gate) {
  // Header H, Length L, Command C, Register R, Value V, Footer F
  // HH HH HH HH LL LL CC CC RR RR VV VV VV VV RR RR VV VV VV VV FF FF FF FF
  // FD FC FB FA 14 00 07 00 10 00 00 FF 00 00 00 01 00 0F 00 00 04 03 02 01

  uint16_t move_threshold_gate = CMD_GATE_MOVE_THRESH[gate];
  uint16_t still_threshold_gate = CMD_GATE_STILL_THRESH[gate];
  CmdFrameT cmd_frame;
  cmd_frame.data_length = 0;
  cmd_frame.header = CMD_FRAME_HEADER;
  cmd_frame.command = CMD_WRITE_ABD_PARAM;
  memcpy(&cmd_frame.data[cmd_frame.data_length], &move_threshold_gate, sizeof(move_threshold_gate));
  cmd_frame.data_length += sizeof(move_threshold_gate);
  memcpy(&cmd_frame.data[cmd_frame.data_length], &this->new_config_.move_thresh[gate],
         sizeof(this->new_config_.move_thresh[gate]));
  cmd_frame.data_length += sizeof(this->new_config_.move_thresh[gate]);
  memcpy(&cmd_frame.data[cmd_frame.data_length], &still_threshold_gate, sizeof(still_threshold_gate));
  cmd_frame.data_length += sizeof(still_threshold_gate);
  memcpy(&cmd_frame.data[cmd_frame.data_length], &this->new_config_.still_thresh[gate],
         sizeof(this->new_config_.still_thresh[gate]));
  cmd_frame.data_length += sizeof(this->new_config_.still_thresh[gate]);
  cmd_frame.footer = CMD_FRAME_FOOTER;
  ESP_LOGD(TAG, "Sending set gate %4X sensitivity command: %2X", gate, cmd_frame.command);
  this->send_cmd_from_array(cmd_frame);
}

#ifdef USE_NUMBER
void LD2420Component::init_gate_config_numbers() {
  this->gate_timeout_number_->publish_state(this->new_config_.timeout);
  this->gate_select_number_->publish_state(0);
  this->min_gate_distance_number_->publish_state(this->current_config_.min_gate);
  this->max_gate_distance_number_->publish_state(this->current_config_.max_gate);
  const uint8_t gate = static_cast<uint8_t>(this->gate_select_number_->state);
  this->gate_still_threshold_number_->publish_state(static_cast<uint16_t>(this->current_config_.still_thresh[gate]));
  this->gate_move_threshold_number_->publish_state(static_cast<uint16_t>(this->current_config_.move_thresh[gate]));
}
#endif

}  // namespace ld2420
}  // namespace esphome
