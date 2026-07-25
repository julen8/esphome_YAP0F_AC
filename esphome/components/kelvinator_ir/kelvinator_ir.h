#pragma once

#include "esphome/components/climate_ir/climate_ir.h"
#include "esphome/components/select/select.h"
#include "esphome/components/switch/switch.h"
#include "kelvinator_protocol.h"

namespace esphome {
namespace kelvinator_ir {

union KelvinatorCommand {
  // 一条常规命令由两个 8 字节数据块组成，共 16 字节。三个成员共享同一段内存：
  // raw64 用于整块收发，raw8 用于访问实测协议字节，位域用于读写已知功能。
  uint64_t raw64[2];
  uint8_t raw8[16];
  struct {
    // 第 0 字节：运行模式、电源、基础风速和自动扫风；最高位还被睡眠 1/3 使用。
    uint8_t mode : 3;
    uint8_t power : 1;
    uint8_t basic_fan : 2;
    uint8_t swing_auto : 1;
    uint8_t : 1;
    // 第 1 字节：目标温度减 16，例如 0x0A 表示 26°C。
    uint8_t temperature : 4;
    uint8_t : 4;
    // 第 2 字节：强劲、显示灯、净化和电辅热关闭标志。
    uint8_t : 4;
    uint8_t turbo : 1;
    uint8_t light : 1;
    uint8_t ion_filter : 1;
    uint8_t auxiliary_heat_off : 1;
    // 第 3 字节：第一数据块命令标记，正常值固定为 0x50，按原始字节赋值。
    uint8_t : 8;
    // 第 4 字节：上下扫风位置和左右扫风开关。
    uint8_t swing_vertical : 4;
    uint8_t swing_horizontal : 1;
    uint8_t : 3;
    // 第 5~6 字节：定时相关字段，当前功能不使用。
    uint8_t pad0[2];
    // 第 7 字节：低半字节含电辅热自动标志，高半字节为第一块校验和。
    uint8_t : 3;
    uint8_t auxiliary_heat_auto : 1;
    uint8_t checksum1 : 4;
    // 第 8~10 字节：重复第 0~2 字节，用于校验两段是否属于同一条命令。
    uint8_t pad1[3];
    // 第 11 字节：第二数据块命令标记，正常值固定为 0x70。
    uint8_t : 8;
    // 第 12 字节：第 7 位为静音；低位由睡眠 2/3/4 复用。
    uint8_t : 7;
    uint8_t quiet : 1;
    // 第 13 字节：睡眠 3 参数，实测固定为 0xA9。
    uint8_t : 8;
    // 第 14 字节：高 3 位保存完整风速，低半字节还可能包含睡眠参数。
    uint8_t : 4;
    uint8_t fan : 3;
    uint8_t : 1;
    // 第 15 字节：低半字节为功能标志（E享为 0x3），高半字节为第二块校验和。
    uint8_t : 4;
    uint8_t checksum2 : 4;
  };
};

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
  void set_display_state(bool state);
  void set_auxiliary_heat_mode(KelvinatorAuxiliaryHeatMode mode);
  void set_sleep_mode(KelvinatorSleepMode mode);

 protected:
  // 字符串必须与 HA 中显示的自定义预设名称完全一致。
  static constexpr const char *QUIET_PRESET = "静音";
  static constexpr const char *ESHARE_PRESET = "E享";

  void control(const climate::ClimateCall &call) override;
  void transmit_state() override;
  bool on_receive(remote_base::RemoteReceiveData data) override;

  bool light_{true};
  KelvinatorAuxiliaryHeatMode auxiliary_heat_mode_{KELVINATOR_AUXILIARY_HEAT_AUTO};
  KelvinatorSleepMode sleep_mode_{KELVINATOR_SLEEP_OFF};
  KelvinatorDisplaySwitch *display_switch_{nullptr};
  KelvinatorAuxiliaryHeatSelect *auxiliary_heat_select_{nullptr};
  KelvinatorSleepModeSelect *sleep_mode_select_{nullptr};
  // ESP32-C3 的 RMT 会把两个 8 字节数据块拆成两次回调，先暂存 0x50 块，
  // 收到对应的 0x70 块后再合并。时间戳用于阻止旧块与下一条命令错误配对。
  optional<uint64_t> pending_first_block_;
  uint32_t pending_first_block_at_{0};
};

}  // namespace kelvinator_ir
}  // namespace esphome
