#pragma once

#include "esphome/components/climate_ir/climate_ir.h"
#include "esphome/components/select/select.h"
#include "esphome/components/switch/switch.h"
#include "kelvinator_protocol.h"

namespace esphome {
namespace kelvinator_ir {

union KelvinatorCommand {
  uint64_t raw64[2];
  uint8_t raw8[16];
  struct {
    uint8_t mode : 3;
    uint8_t power : 1;
    uint8_t basic_fan : 2;
    uint8_t swing_auto : 1;
    uint8_t : 1;
    uint8_t temperature : 4;
    uint8_t : 4;
    uint8_t : 4;
    uint8_t turbo : 1;
    uint8_t light : 1;
    uint8_t ion_filter : 1;
    uint8_t auxiliary_heat_off : 1;
    uint8_t : 8;
    uint8_t swing_vertical : 4;
    uint8_t swing_horizontal : 1;
    uint8_t : 3;
    uint8_t pad0[2];
    uint8_t : 3;
    uint8_t auxiliary_heat_auto : 1;
    uint8_t checksum1 : 4;
    uint8_t pad1[3];
    uint8_t : 8;
    uint8_t : 7;
    uint8_t quiet : 1;
    uint8_t : 8;
    uint8_t : 4;
    uint8_t fan : 3;
    uint8_t : 1;
    uint8_t : 4;
    uint8_t checksum2 : 4;
  };
};

enum KelvinatorAuxiliaryHeatMode : uint8_t {
  KELVINATOR_AUXILIARY_HEAT_OFF = 0,
  KELVINATOR_AUXILIARY_HEAT_AUTO = 1,
  KELVINATOR_AUXILIARY_HEAT_ON = 2,
};

class KelvinatorIR;

class KelvinatorDisplaySwitch : public switch_::Switch {
 public:
  explicit KelvinatorDisplaySwitch(KelvinatorIR *parent) : parent_(parent) {}

 protected:
  void write_state(bool state) override;
  KelvinatorIR *parent_;
};

class KelvinatorAuxiliaryHeatSelect : public select::Select {
 public:
  explicit KelvinatorAuxiliaryHeatSelect(KelvinatorIR *parent) : parent_(parent) {}

 protected:
  void control(size_t index) override;
  KelvinatorIR *parent_;
};

class KelvinatorIR : public climate_ir::ClimateIR {
 public:
  KelvinatorIR()
      : climate_ir::ClimateIR(16, 30, 1.0f, true, true,
                              {climate::CLIMATE_FAN_AUTO, climate::CLIMATE_FAN_LOW, climate::CLIMATE_FAN_MEDIUM,
                               climate::CLIMATE_FAN_HIGH},
                              {climate::CLIMATE_SWING_OFF, climate::CLIMATE_SWING_VERTICAL,
                               climate::CLIMATE_SWING_HORIZONTAL, climate::CLIMATE_SWING_BOTH},
                              {climate::CLIMATE_PRESET_NONE, climate::CLIMATE_PRESET_BOOST}) {
    this->set_supported_custom_presets({QUIET_PRESET});
  }

  void set_light(bool enabled) { this->light_ = enabled; }
  void set_display_switch(KelvinatorDisplaySwitch *display_switch) { this->display_switch_ = display_switch; }
  void set_auxiliary_heat_select(KelvinatorAuxiliaryHeatSelect *auxiliary_heat_select) {
    this->auxiliary_heat_select_ = auxiliary_heat_select;
  }
  void set_display_state(bool state);
  void set_auxiliary_heat_mode(KelvinatorAuxiliaryHeatMode mode);

 protected:
  static constexpr const char *QUIET_PRESET = "静音";

  void control(const climate::ClimateCall &call) override;
  void transmit_state() override;
  bool on_receive(remote_base::RemoteReceiveData data) override;

  bool light_{true};
  KelvinatorAuxiliaryHeatMode auxiliary_heat_mode_{KELVINATOR_AUXILIARY_HEAT_AUTO};
  KelvinatorDisplaySwitch *display_switch_{nullptr};
  KelvinatorAuxiliaryHeatSelect *auxiliary_heat_select_{nullptr};
  optional<uint64_t> pending_first_block_;
};

}  // namespace kelvinator_ir
}  // namespace esphome
