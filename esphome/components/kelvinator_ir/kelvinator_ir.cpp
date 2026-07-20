#include "kelvinator_ir.h"
#include "esphome/core/log.h"

namespace esphome {
namespace kelvinator_ir {

static const char *const TAG = "climate.kelvinator_ir";

static const uint8_t MODE_AUTO = 0;
static const uint8_t MODE_COOL = 1;
static const uint8_t MODE_DRY = 2;
static const uint8_t MODE_FAN = 3;
static const uint8_t MODE_HEAT = 4;

void KelvinatorDisplaySwitch::write_state(bool state) { this->parent_->set_display_state(state); }

void KelvinatorAuxiliaryHeatSelect::control(size_t index) {
  this->parent_->set_auxiliary_heat_mode(static_cast<KelvinatorAuxiliaryHeatMode>(index));
}

void KelvinatorIR::set_display_state(bool state) {
  this->light_ = state;
  this->transmit_state();
  this->display_switch_->publish_state(state);
}

void KelvinatorIR::set_auxiliary_heat_mode(KelvinatorAuxiliaryHeatMode mode) {
  this->auxiliary_heat_mode_ = mode;
  this->transmit_state();
  this->auxiliary_heat_select_->publish_state(static_cast<size_t>(mode));
}

void KelvinatorIR::control(const climate::ClimateCall &call) {
  if (call.has_custom_preset()) {
    this->set_custom_preset_(call.get_custom_preset());
  } else if (call.get_preset().has_value()) {
    this->clear_custom_preset_();
  } else if (call.get_fan_mode().has_value()) {
    this->preset = climate::CLIMATE_PRESET_NONE;
    this->clear_custom_preset_();
  }
  climate_ir::ClimateIR::control(call);
}

void KelvinatorIR::transmit_state() {
  KelvinatorCommand command{};
  command.power = this->mode != climate::CLIMATE_MODE_OFF;
  switch (this->mode) {
    case climate::CLIMATE_MODE_COOL:
      command.mode = MODE_COOL;
      break;
    case climate::CLIMATE_MODE_DRY:
      command.mode = MODE_DRY;
      break;
    case climate::CLIMATE_MODE_FAN_ONLY:
      command.mode = MODE_FAN;
      break;
    case climate::CLIMATE_MODE_HEAT:
      command.mode = MODE_HEAT;
      break;
    default:
      command.mode = MODE_AUTO;
      break;
  }
  command.temperature = static_cast<uint8_t>(this->target_temperature) - 16;
  command.turbo = this->preset.value_or(climate::CLIMATE_PRESET_NONE) == climate::CLIMATE_PRESET_BOOST;
  command.quiet = this->has_custom_preset() && this->get_custom_preset() == QUIET_PRESET;
  command.light = this->light_;
  command.auxiliary_heat_off = this->auxiliary_heat_mode_ == KELVINATOR_AUXILIARY_HEAT_OFF;
  command.auxiliary_heat_auto = this->auxiliary_heat_mode_ == KELVINATOR_AUXILIARY_HEAT_AUTO;
  command.basic_fan = 0;
  if (this->fan_mode == climate::CLIMATE_FAN_LOW)
    command.basic_fan = 1;
  else if (this->fan_mode == climate::CLIMATE_FAN_MEDIUM)
    command.basic_fan = 2;
  else if (this->fan_mode == climate::CLIMATE_FAN_HIGH)
    command.basic_fan = 3;
  command.swing_auto = this->swing_mode == climate::CLIMATE_SWING_VERTICAL ||
                       this->swing_mode == climate::CLIMATE_SWING_BOTH;
  command.swing_vertical = command.swing_auto ? 1 : 0;
  command.swing_horizontal = this->swing_mode == climate::CLIMATE_SWING_HORIZONTAL ||
                             this->swing_mode == climate::CLIMATE_SWING_BOTH;
  command.raw8[3] = 0x50;
  command.raw8[8] = command.raw8[0];
  command.raw8[9] = command.raw8[1];
  command.raw8[10] = command.raw8[2];
  command.raw8[11] = 0x70;

  remote_base::KelvinatorData payload;
  payload.data = {command.raw64[0], command.raw64[1]};
  payload.apply_checksum();
  auto transmit = this->transmitter_->transmit();
  remote_base::KelvinatorProtocol().encode(transmit.get_data(), payload);
  transmit.perform();
}

bool KelvinatorIR::on_receive(remote_base::RemoteReceiveData data) {
  auto decoded = remote_base::KelvinatorProtocol().decode(data);
  if (!decoded.has_value() || decoded->data.empty())
    return false;

  if (decoded->data.size() == 1) {
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&decoded->data[0]);
    if (bytes[3] == 0x50) {
      this->pending_first_block_ = decoded->data[0];
      ESP_LOGD(TAG, "Received first Kelvinator block");
      return true;
    }
    if (bytes[3] != 0x70 || !this->pending_first_block_.has_value())
      return false;
    decoded->data.insert(decoded->data.begin(), *this->pending_first_block_);
    this->pending_first_block_.reset();
    ESP_LOGD(TAG, "Received second Kelvinator block; publishing state");
  }

  KelvinatorCommand command{};
  command.raw64[0] = decoded->data[0];
  command.raw64[1] = decoded->data[1];
  if (!command.power) {
    this->mode = climate::CLIMATE_MODE_OFF;
  } else {
    switch (command.mode) {
      case MODE_COOL:
        this->mode = climate::CLIMATE_MODE_COOL;
        break;
      case MODE_DRY:
        this->mode = climate::CLIMATE_MODE_DRY;
        break;
      case MODE_FAN:
        this->mode = climate::CLIMATE_MODE_FAN_ONLY;
        break;
      case MODE_HEAT:
        this->mode = climate::CLIMATE_MODE_HEAT;
        break;
      default:
        this->mode = climate::CLIMATE_MODE_HEAT_COOL;
        break;
    }
  }
  this->target_temperature = command.temperature + 16;
  switch (command.basic_fan) {
    case 1:
      this->fan_mode = climate::CLIMATE_FAN_LOW;
      break;
    case 2:
      this->fan_mode = climate::CLIMATE_FAN_MEDIUM;
      break;
    case 3:
      this->fan_mode = climate::CLIMATE_FAN_HIGH;
      break;
    default:
      this->fan_mode = climate::CLIMATE_FAN_AUTO;
      break;
  }
  const bool vertical_swing = command.swing_vertical == 1;
  const bool horizontal_swing = command.swing_horizontal;
  if (vertical_swing && horizontal_swing)
    this->swing_mode = climate::CLIMATE_SWING_BOTH;
  else if (vertical_swing)
    this->swing_mode = climate::CLIMATE_SWING_VERTICAL;
  else if (horizontal_swing)
    this->swing_mode = climate::CLIMATE_SWING_HORIZONTAL;
  else
    this->swing_mode = climate::CLIMATE_SWING_OFF;

  if (command.quiet) {
    this->set_custom_preset_(QUIET_PRESET);
  } else {
    this->clear_custom_preset_();
    this->preset = command.turbo ? climate::CLIMATE_PRESET_BOOST : climate::CLIMATE_PRESET_NONE;
  }

  this->light_ = command.light;
  if (this->display_switch_ != nullptr)
    this->display_switch_->publish_state(this->light_);

  if (command.power && command.mode == MODE_HEAT) {
    if (command.auxiliary_heat_off)
      this->auxiliary_heat_mode_ = KELVINATOR_AUXILIARY_HEAT_OFF;
    else if (command.auxiliary_heat_auto)
      this->auxiliary_heat_mode_ = KELVINATOR_AUXILIARY_HEAT_AUTO;
    else
      this->auxiliary_heat_mode_ = KELVINATOR_AUXILIARY_HEAT_ON;
    if (this->auxiliary_heat_select_ != nullptr)
      this->auxiliary_heat_select_->publish_state(static_cast<size_t>(this->auxiliary_heat_mode_));
  }
  this->publish_state();
  return true;
}

}  // namespace kelvinator_ir
}  // namespace esphome
