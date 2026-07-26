#pragma once

#include "esphome/components/climate_ir/climate_ir.h"
#include "esphome/components/select/select.h"
#include "esphome/components/switch/switch.h"
#include "kelvinator_protocol.h"

namespace esphome {
namespace kelvinator_ir {

// 枚举值必须与 climate.py 中电辅热 select 的选项索引保持一致。
enum KelvinatorAuxiliaryHeatMode : uint8_t {
  KELVINATOR_AUXILIARY_HEAT_OFF = 0,
  KELVINATOR_AUXILIARY_HEAT_AUTO = 1,
  KELVINATOR_AUXILIARY_HEAT_ON = 2,
};

// 睡眠是独立于 climate preset 的状态，因此使用单独的 select 和枚举。
// 这样进入睡眠后仍可单独关闭静音，而不会同时退出睡眠。
enum KelvinatorSleepMode : uint8_t {
  KELVINATOR_SLEEP_OFF = 0,
  KELVINATOR_SLEEP_1 = 1,
  KELVINATOR_SLEEP_2 = 2,
  KELVINATOR_SLEEP_3 = 3,
  KELVINATOR_SLEEP_4 = 4,
};

enum KelvinatorDirectionMode : uint8_t {
  KELVINATOR_DIRECTION_OFF = 0,
  KELVINATOR_DIRECTION_SWING = 1,
  KELVINATOR_DIRECTION_POSITION_1 = 2,
  KELVINATOR_DIRECTION_POSITION_2 = 3,
  KELVINATOR_DIRECTION_POSITION_3 = 4,
  KELVINATOR_DIRECTION_POSITION_4 = 5,
  KELVINATOR_DIRECTION_POSITION_5 = 6,
  KELVINATOR_DIRECTION_OUTWARD = 7,
  KELVINATOR_DIRECTION_ALTERNATING = 8,
};

class KelvinatorIR;

// 将 HA 的显示灯 switch 操作转交给主空调组件统一构造并发送完整红外状态。
class KelvinatorDisplaySwitch : public switch_::Switch {
 public:
  explicit KelvinatorDisplaySwitch(KelvinatorIR *parent) : parent_(parent) {}

 protected:
  void write_state(bool state) override;
  KelvinatorIR *parent_;
};

// 将 HA 的三档电辅热 select 操作转交给主空调组件。
class KelvinatorAuxiliaryHeatSelect : public select::Select {
 public:
  explicit KelvinatorAuxiliaryHeatSelect(KelvinatorIR *parent) : parent_(parent) {}

 protected:
  void control(size_t index) override;
  KelvinatorIR *parent_;
};

// 将 HA 的五档睡眠 select 操作转交给主空调组件。
class KelvinatorSleepModeSelect : public select::Select {
 public:
  explicit KelvinatorSleepModeSelect(KelvinatorIR *parent) : parent_(parent) {}

 protected:
  void control(size_t index) override;
  KelvinatorIR *parent_;
};

class KelvinatorVerticalDirectionSelect : public select::Select {
 public:
  explicit KelvinatorVerticalDirectionSelect(KelvinatorIR *parent) : parent_(parent) {}

 protected:
  void control(size_t index) override;
  KelvinatorIR *parent_;
};

class KelvinatorHorizontalDirectionSelect : public select::Select {
 public:
  explicit KelvinatorHorizontalDirectionSelect(KelvinatorIR *parent) : parent_(parent) {}

 protected:
  void control(size_t index) override;
  KelvinatorIR *parent_;
};

class KelvinatorCoolModeSwitch : public switch_::Switch {
 public:
  explicit KelvinatorCoolModeSwitch(KelvinatorIR *parent) : parent_(parent) {}

 protected:
  void write_state(bool state) override;
  KelvinatorIR *parent_;
};

class KelvinatorIR : public climate_ir::ClimateIR {
 public:
  // 温度范围 16~30°C，步进 1°C；支持四档风速、四种扫风组合、无预设和强劲。
  // 静音与 E享通过自定义 preset 暴露，睡眠则由独立 select 暴露。
  KelvinatorIR()
      : climate_ir::ClimateIR(16, 30, 1.0f, true, true,
                              {climate::CLIMATE_FAN_AUTO, climate::CLIMATE_FAN_LOW, climate::CLIMATE_FAN_MEDIUM,
                               climate::CLIMATE_FAN_HIGH},
                              {climate::CLIMATE_SWING_OFF, climate::CLIMATE_SWING_VERTICAL,
                               climate::CLIMATE_SWING_HORIZONTAL, climate::CLIMATE_SWING_BOTH},
                              {climate::CLIMATE_PRESET_NONE, climate::CLIMATE_PRESET_BOOST}) {
                  this->set_supported_custom_presets({QUIET_PRESET, ESHARE_PRESET});
  }

  void set_light(bool enabled) { this->light_ = enabled; }
  void set_display_switch(KelvinatorDisplaySwitch *display_switch) { this->display_switch_ = display_switch; }
  void set_auxiliary_heat_select(KelvinatorAuxiliaryHeatSelect *auxiliary_heat_select) {
    this->auxiliary_heat_select_ = auxiliary_heat_select;
  }
  void set_sleep_mode_select(KelvinatorSleepModeSelect *sleep_mode_select) {
    this->sleep_mode_select_ = sleep_mode_select;
  }
  void set_vertical_direction_select(KelvinatorVerticalDirectionSelect *vertical_direction_select) {
    this->vertical_direction_select_ = vertical_direction_select;
  }
  void set_horizontal_direction_select(KelvinatorHorizontalDirectionSelect *horizontal_direction_select) {
    this->horizontal_direction_select_ = horizontal_direction_select;
  }
  void set_cool_mode_switch(KelvinatorCoolModeSwitch *cool_mode_switch) {
    this->cool_mode_switch_ = cool_mode_switch;
  }
  void set_display_state(bool state);
  void set_auxiliary_heat_mode(KelvinatorAuxiliaryHeatMode mode);
  void set_sleep_mode(KelvinatorSleepMode mode);
  void set_vertical_direction_mode(KelvinatorDirectionMode mode);
  void set_horizontal_direction_mode(KelvinatorDirectionMode mode);
  void set_cool_mode_state(bool state);

 protected:
  // 字符串必须与 HA 中显示的自定义预设名称完全一致。
  static constexpr const char *QUIET_PRESET = "静音";
  static constexpr const char *ESHARE_PRESET = "E享";

  void control(const climate::ClimateCall &call) override;
  void transmit_state() override;
  bool on_receive(remote_base::RemoteReceiveData data) override;

  bool light_{true};
  bool cool_mode_{false};
  KelvinatorAuxiliaryHeatMode auxiliary_heat_mode_{KELVINATOR_AUXILIARY_HEAT_AUTO};
  KelvinatorSleepMode sleep_mode_{KELVINATOR_SLEEP_OFF};
  KelvinatorDirectionMode vertical_direction_mode_{KELVINATOR_DIRECTION_SWING};
  KelvinatorDirectionMode horizontal_direction_mode_{KELVINATOR_DIRECTION_OFF};
  KelvinatorDisplaySwitch *display_switch_{nullptr};
  KelvinatorAuxiliaryHeatSelect *auxiliary_heat_select_{nullptr};
  KelvinatorSleepModeSelect *sleep_mode_select_{nullptr};
  KelvinatorVerticalDirectionSelect *vertical_direction_select_{nullptr};
  KelvinatorHorizontalDirectionSelect *horizontal_direction_select_{nullptr};
  KelvinatorCoolModeSwitch *cool_mode_switch_{nullptr};
  // ESP32-C3 的 RMT 会把两个 8 字节数据块拆成两次回调，先暂存 0x50 块，
  // 收到对应的 0x70 块后再合并。时间戳用于阻止旧块与下一条命令错误配对。
  optional<uint64_t> pending_first_block_;
  uint32_t pending_first_block_at_{0};
};

}  // namespace kelvinator_ir
}  // namespace esphome
