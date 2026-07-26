#include "kelvinator_ir.h"
#include <array>
#include <cstring>
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace kelvinator_ir {

static const char *const TAG = "climate.kelvinator_ir";

static constexpr uint8_t MODE_AUTO = 0;
static constexpr uint8_t MODE_COOL = 1;
static constexpr uint8_t MODE_DRY = 2;
static constexpr uint8_t MODE_FAN = 3;
static constexpr uint8_t MODE_HEAT = 4;

// 第 3 字节的高半字节用于区分常规第一块、常规第二块和睡眠曲线块。
static constexpr uint8_t COMMAND_BLOCK_1_MARKER = 0x50;
static constexpr uint8_t COMMAND_BLOCK_2_MARKER = 0x70;
static constexpr uint8_t SLEEP_CURVE_BLOCK_MARKER = 0x80;
// E享标志位于第 15 字节低半字节；高半字节由校验函数在发送前覆盖。
static constexpr uint8_t ESHARE_MARKER = 0x03;
// 左右风向由第 4 字节高半字节离散编码（实机抓包）：
// 关闭=0x0, 扫风=0x1, 位置1=0x2, 位置2=0x3, 位置3=0x4, 位置4=0x5,
// 位置5=0x6, 向两边=0xC, 交替扫风=0xD。
static constexpr uint8_t HORIZONTAL_CODE_OFF = 0x0;
static constexpr uint8_t HORIZONTAL_CODE_SWING = 0x1;
static constexpr uint8_t HORIZONTAL_CODE_POS_1 = 0x2;
static constexpr uint8_t HORIZONTAL_CODE_POS_2 = 0x3;
static constexpr uint8_t HORIZONTAL_CODE_POS_3 = 0x4;
static constexpr uint8_t HORIZONTAL_CODE_POS_4 = 0x5;
static constexpr uint8_t HORIZONTAL_CODE_POS_5 = 0x6;
static constexpr uint8_t HORIZONTAL_CODE_OUTWARD = 0xC;
static constexpr uint8_t HORIZONTAL_CODE_ALTERNATING = 0xD;
static constexpr uint8_t COOL_MODE_MARKER = 0x08;
// 遥控器两块之间只有约 40ms，500ms 足以容纳正常抖动，同时避免跨命令配对。
static constexpr uint32_t SPLIT_BLOCK_TIMEOUT_MS = 500;

// 显式字节索引和掩码，避免依赖编译器位域布局。
static constexpr uint8_t BYTE_0 = 0;
static constexpr uint8_t BYTE_1 = 1;
static constexpr uint8_t BYTE_2 = 2;
static constexpr uint8_t BYTE_3 = 3;
static constexpr uint8_t BYTE_4 = 4;
static constexpr uint8_t BYTE_7 = 7;
static constexpr uint8_t BYTE_8 = 8;
static constexpr uint8_t BYTE_9 = 9;
static constexpr uint8_t BYTE_10 = 10;
static constexpr uint8_t BYTE_11 = 11;
static constexpr uint8_t BYTE_12 = 12;
static constexpr uint8_t BYTE_13 = 13;
static constexpr uint8_t BYTE_14 = 14;
static constexpr uint8_t BYTE_15 = 15;

static constexpr uint8_t MASK_MODE = 0x07;
static constexpr uint8_t MASK_POWER = 0x08;
static constexpr uint8_t MASK_BASIC_FAN = 0x30;
static constexpr uint8_t MASK_SWING_AUTO = 0x40;
static constexpr uint8_t MASK_SLEEP13 = 0x80;

static constexpr uint8_t MASK_TEMPERATURE = 0x0F;

static constexpr uint8_t MASK_TURBO = 0x10;
static constexpr uint8_t MASK_LIGHT = 0x20;
static constexpr uint8_t MASK_AUXILIARY_HEAT_OFF = 0x80;

static constexpr uint8_t MASK_SWING_VERTICAL = 0x0F;
static constexpr uint8_t MASK_SWING_HORIZONTAL_CODE = 0xF0;

static constexpr uint8_t MASK_AUXILIARY_HEAT_AUTO = 0x08;

static constexpr uint8_t MASK_SLEEP_MODE_2 = 0x01;
static constexpr uint8_t MASK_SLEEP_MODE_4 = 0x08;
static constexpr uint8_t MASK_QUIET = 0x80;

static constexpr uint8_t MASK_FAN = 0x70;
static constexpr uint8_t MASK_FEATURE_MARKERS = 0x0F;
static constexpr uint8_t SLEEP_MODE_3_MARKER = 0xA9;

static constexpr uint8_t SHIFT_MODE = 0;
static constexpr uint8_t SHIFT_BASIC_FAN = 4;
static constexpr uint8_t SHIFT_TEMPERATURE = 0;
static constexpr uint8_t SHIFT_SWING_VERTICAL = 0;
static constexpr uint8_t SHIFT_SWING_HORIZONTAL_CODE = 4;
static constexpr uint8_t SHIFT_FAN = 4;
static constexpr uint8_t SHIFT_FEATURE_MARKERS = 0;
static constexpr uint8_t TEMPERATURE_MIN = 16;
static constexpr uint8_t TEMPERATURE_MAX = 30;

static uint8_t encode_horizontal_direction(KelvinatorDirectionMode mode);
static KelvinatorDirectionMode decode_horizontal_direction(uint8_t code);

struct KelvinatorFrameState {
  uint8_t mode{MODE_AUTO};
  bool power{false};
  uint8_t temperature{25};
  bool turbo{false};
  bool quiet{false};
  bool light{true};
  KelvinatorAuxiliaryHeatMode auxiliary_heat_mode{KELVINATOR_AUXILIARY_HEAT_AUTO};
  uint8_t basic_fan{0};
  KelvinatorDirectionMode vertical_direction_mode{KELVINATOR_DIRECTION_SWING};
  KelvinatorDirectionMode horizontal_direction_mode{KELVINATOR_DIRECTION_OFF};
  bool eshare{false};
  bool cool_mode{false};
  KelvinatorSleepMode sleep_mode{KELVINATOR_SLEEP_OFF};
};

static inline uint8_t read_bits(uint8_t value, uint8_t mask, uint8_t shift) { return (value & mask) >> shift; }

static inline void write_bits(uint8_t &target, uint8_t mask, uint8_t shift, uint8_t value) {
  target = static_cast<uint8_t>((target & ~mask) | ((value << shift) & mask));
}

static inline bool read_flag(uint8_t value, uint8_t mask) { return (value & mask) != 0; }

static inline void write_flag(uint8_t &target, uint8_t mask, bool enabled) {
  if (enabled)
    target = static_cast<uint8_t>(target | mask);
  else
    target = static_cast<uint8_t>(target & ~mask);
}

static inline uint8_t clamp_temperature(uint8_t temperature) {
  if (temperature < TEMPERATURE_MIN)
    return TEMPERATURE_MIN;
  if (temperature > TEMPERATURE_MAX)
    return TEMPERATURE_MAX;
  return temperature;
}

static inline uint8_t encode_basic_fan_mode(climate::ClimateFanMode fan_mode) {
  switch (fan_mode) {
    case climate::CLIMATE_FAN_LOW:
      return 1;
    case climate::CLIMATE_FAN_MEDIUM:
      return 2;
    case climate::CLIMATE_FAN_HIGH:
      return 3;
    default:
      return 0;
  }
}

static inline climate::ClimateFanMode decode_basic_fan_mode(uint8_t basic_fan) {
  switch (basic_fan) {
    case 1:
      return climate::CLIMATE_FAN_LOW;
    case 2:
      return climate::CLIMATE_FAN_MEDIUM;
    case 3:
      return climate::CLIMATE_FAN_HIGH;
    default:
      return climate::CLIMATE_FAN_AUTO;
  }
}

static std::array<uint8_t, 16> pack_regular_command(const KelvinatorFrameState &state) {
  std::array<uint8_t, 16> bytes{};
  const uint8_t clamped_temperature = clamp_temperature(state.temperature);

  write_bits(bytes[BYTE_0], MASK_MODE, SHIFT_MODE, state.mode);
  write_flag(bytes[BYTE_0], MASK_POWER, state.power);
  write_bits(bytes[BYTE_0], MASK_BASIC_FAN, SHIFT_BASIC_FAN, state.basic_fan);
  const bool swing_auto = state.vertical_direction_mode == KELVINATOR_DIRECTION_SWING ||
                          state.horizontal_direction_mode == KELVINATOR_DIRECTION_SWING ||
                          state.horizontal_direction_mode == KELVINATOR_DIRECTION_ALTERNATING;
  write_flag(bytes[BYTE_0], MASK_SWING_AUTO, swing_auto);

  write_bits(bytes[BYTE_1], MASK_TEMPERATURE, SHIFT_TEMPERATURE,
             static_cast<uint8_t>(clamped_temperature - TEMPERATURE_MIN));

  write_flag(bytes[BYTE_2], MASK_TURBO, state.turbo);
  write_flag(bytes[BYTE_2], MASK_LIGHT, state.light);
  write_flag(bytes[BYTE_2], MASK_AUXILIARY_HEAT_OFF, state.auxiliary_heat_mode == KELVINATOR_AUXILIARY_HEAT_OFF);

  bytes[BYTE_3] = COMMAND_BLOCK_1_MARKER;
  write_bits(bytes[BYTE_4], MASK_SWING_VERTICAL, SHIFT_SWING_VERTICAL,
             static_cast<uint8_t>(state.vertical_direction_mode));
  write_bits(bytes[BYTE_4], MASK_SWING_HORIZONTAL_CODE, SHIFT_SWING_HORIZONTAL_CODE,
             encode_horizontal_direction(state.horizontal_direction_mode));

  write_flag(bytes[BYTE_7], MASK_AUXILIARY_HEAT_AUTO, state.auxiliary_heat_mode == KELVINATOR_AUXILIARY_HEAT_AUTO);

  bytes[BYTE_8] = bytes[BYTE_0];
  bytes[BYTE_9] = bytes[BYTE_1];
  bytes[BYTE_10] = bytes[BYTE_2];
  bytes[BYTE_11] = COMMAND_BLOCK_2_MARKER;

  write_flag(bytes[BYTE_12], MASK_QUIET, state.quiet);

  // Byte14 的风速字段与 Byte0 的 basic_fan 保持一致。
  write_bits(bytes[BYTE_14], MASK_FAN, SHIFT_FAN, state.basic_fan & 0x07);

  uint8_t feature_markers = 0;
  if (state.cool_mode && state.power && state.mode == MODE_COOL)
    feature_markers |= COOL_MODE_MARKER;
  if (state.eshare)
    feature_markers = ESHARE_MARKER;
  write_bits(bytes[BYTE_15], MASK_FEATURE_MARKERS, SHIFT_FEATURE_MARKERS, feature_markers);

  switch (state.sleep_mode) {
    case KELVINATOR_SLEEP_1:
      write_flag(bytes[BYTE_0], MASK_SLEEP13, true);
      break;
    case KELVINATOR_SLEEP_2:
      write_flag(bytes[BYTE_12], MASK_SLEEP_MODE_2, true);
      break;
    case KELVINATOR_SLEEP_3:
      write_flag(bytes[BYTE_0], MASK_SLEEP13, true);
      write_flag(bytes[BYTE_12], MASK_SLEEP_MODE_2, true);
      bytes[BYTE_13] = SLEEP_MODE_3_MARKER;
      break;
    case KELVINATOR_SLEEP_4:
      write_flag(bytes[BYTE_0], MASK_SLEEP13, true);
      write_flag(bytes[BYTE_12], MASK_SLEEP_MODE_4, true);
      break;
    default:
      break;
  }

  // 睡眠位会改写第 0 字节，镜像位在最后统一同步。
  bytes[BYTE_8] = bytes[BYTE_0];
  return bytes;
}

static KelvinatorFrameState unpack_regular_command(const std::array<uint8_t, 16> &bytes) {
  KelvinatorFrameState state;
  state.mode = read_bits(bytes[BYTE_0], MASK_MODE, SHIFT_MODE);
  state.power = read_flag(bytes[BYTE_0], MASK_POWER);
  state.temperature = static_cast<uint8_t>(read_bits(bytes[BYTE_1], MASK_TEMPERATURE, SHIFT_TEMPERATURE) + 16);
  state.turbo = read_flag(bytes[BYTE_2], MASK_TURBO);
  state.light = read_flag(bytes[BYTE_2], MASK_LIGHT);

  if (read_flag(bytes[BYTE_2], MASK_AUXILIARY_HEAT_OFF))
    state.auxiliary_heat_mode = KELVINATOR_AUXILIARY_HEAT_OFF;
  else if (read_flag(bytes[BYTE_7], MASK_AUXILIARY_HEAT_AUTO))
    state.auxiliary_heat_mode = KELVINATOR_AUXILIARY_HEAT_AUTO;
  else
    state.auxiliary_heat_mode = KELVINATOR_AUXILIARY_HEAT_ON;

  state.basic_fan = read_bits(bytes[BYTE_0], MASK_BASIC_FAN, SHIFT_BASIC_FAN);

  const uint8_t vertical = read_bits(bytes[BYTE_4], MASK_SWING_VERTICAL, SHIFT_SWING_VERTICAL);
  state.vertical_direction_mode =
      vertical <= KELVINATOR_DIRECTION_POSITION_5 ? static_cast<KelvinatorDirectionMode>(vertical)
                                                  : KELVINATOR_DIRECTION_SWING;
  state.horizontal_direction_mode =
      decode_horizontal_direction(read_bits(bytes[BYTE_4], MASK_SWING_HORIZONTAL_CODE, SHIFT_SWING_HORIZONTAL_CODE));

  state.quiet = read_flag(bytes[BYTE_12], MASK_QUIET);
  const uint8_t feature_markers = read_bits(bytes[BYTE_15], MASK_FEATURE_MARKERS, SHIFT_FEATURE_MARKERS);
  state.eshare = state.power && feature_markers == ESHARE_MARKER;
  state.cool_mode = state.power && state.mode == MODE_COOL && ((feature_markers & COOL_MODE_MARKER) != 0);

  const bool sleep13_flag = read_flag(bytes[BYTE_0], MASK_SLEEP13);
  const bool sleep_mode_2_flag = read_flag(bytes[BYTE_12], MASK_SLEEP_MODE_2);
  const bool sleep_mode_4_flag = read_flag(bytes[BYTE_12], MASK_SLEEP_MODE_4);
  if (sleep13_flag && sleep_mode_4_flag)
    state.sleep_mode = KELVINATOR_SLEEP_4;
  else if (sleep13_flag && sleep_mode_2_flag && bytes[BYTE_13] == SLEEP_MODE_3_MARKER)
    state.sleep_mode = KELVINATOR_SLEEP_3;
  else if (!sleep13_flag && sleep_mode_2_flag)
    state.sleep_mode = KELVINATOR_SLEEP_2;
  else if (sleep13_flag && !sleep_mode_2_flag && !sleep_mode_4_flag)
    state.sleep_mode = KELVINATOR_SLEEP_1;
  else
    state.sleep_mode = KELVINATOR_SLEEP_OFF;

  return state;
}

static uint8_t encode_horizontal_direction(KelvinatorDirectionMode mode) {
  switch (mode) {
    case KELVINATOR_DIRECTION_SWING:
      return HORIZONTAL_CODE_SWING;
    case KELVINATOR_DIRECTION_POSITION_1:
      return HORIZONTAL_CODE_POS_1;
    case KELVINATOR_DIRECTION_POSITION_2:
      return HORIZONTAL_CODE_POS_2;
    case KELVINATOR_DIRECTION_POSITION_3:
      return HORIZONTAL_CODE_POS_3;
    case KELVINATOR_DIRECTION_POSITION_4:
      return HORIZONTAL_CODE_POS_4;
    case KELVINATOR_DIRECTION_POSITION_5:
      return HORIZONTAL_CODE_POS_5;
    case KELVINATOR_DIRECTION_OUTWARD:
      return HORIZONTAL_CODE_OUTWARD;
    case KELVINATOR_DIRECTION_ALTERNATING:
      return HORIZONTAL_CODE_ALTERNATING;
    default:
      return HORIZONTAL_CODE_OFF;
  }
}

static KelvinatorDirectionMode decode_horizontal_direction(uint8_t code) {
  switch (code) {
    case HORIZONTAL_CODE_SWING:
      return KELVINATOR_DIRECTION_SWING;
    case HORIZONTAL_CODE_POS_1:
      return KELVINATOR_DIRECTION_POSITION_1;
    case HORIZONTAL_CODE_POS_2:
      return KELVINATOR_DIRECTION_POSITION_2;
    case HORIZONTAL_CODE_POS_3:
      return KELVINATOR_DIRECTION_POSITION_3;
    case HORIZONTAL_CODE_POS_4:
      return KELVINATOR_DIRECTION_POSITION_4;
    case HORIZONTAL_CODE_POS_5:
      return KELVINATOR_DIRECTION_POSITION_5;
    case HORIZONTAL_CODE_OUTWARD:
      return KELVINATOR_DIRECTION_OUTWARD;
    case HORIZONTAL_CODE_ALTERNATING:
      return KELVINATOR_DIRECTION_ALTERNATING;
    default:
      return KELVINATOR_DIRECTION_OFF;
  }
}

void KelvinatorDisplaySwitch::write_state(bool state) { this->parent_->set_display_state(state); }

void KelvinatorAuxiliaryHeatSelect::control(size_t index) {
  this->parent_->set_auxiliary_heat_mode(static_cast<KelvinatorAuxiliaryHeatMode>(index));
}

void KelvinatorSleepModeSelect::control(size_t index) {
  this->parent_->set_sleep_mode(static_cast<KelvinatorSleepMode>(index));
}

void KelvinatorVerticalDirectionSelect::control(size_t index) {
  this->parent_->set_vertical_direction_mode(static_cast<KelvinatorDirectionMode>(index));
}

void KelvinatorHorizontalDirectionSelect::control(size_t index) {
  this->parent_->set_horizontal_direction_mode(static_cast<KelvinatorDirectionMode>(index));
}

void KelvinatorCoolModeSwitch::write_state(bool state) { this->parent_->set_cool_mode_state(state); }

void KelvinatorIR::set_display_state(bool state) {
  // 显示灯是整条空调状态的一部分，不能只发送单独的开关码。
  this->light_ = state;
  this->transmit_state();
  if (this->display_switch_ != nullptr)
    this->display_switch_->publish_state(state);
}

void KelvinatorIR::set_auxiliary_heat_mode(KelvinatorAuxiliaryHeatMode mode) {
  // 电辅热由两个协议位组合表示：关闭位和自动位，具体映射在 transmit_state() 中完成。
  this->auxiliary_heat_mode_ = mode;
  this->transmit_state();
  if (this->auxiliary_heat_select_ != nullptr)
    this->auxiliary_heat_select_->publish_state(static_cast<size_t>(mode));
}

void KelvinatorIR::set_sleep_mode(KelvinatorSleepMode mode) {
  // 实测遥控器进入任意睡眠模式时会同时打开静音；关闭睡眠时也会清除静音。
  // 进入睡眠后，用户仍可单独把 climate 预设切回“无”来关闭静音，sleep_mode_ 不受影响。
  this->sleep_mode_ = mode;
  if (mode == KELVINATOR_SLEEP_OFF) {
    this->clear_custom_preset_();
  } else {
    this->set_custom_preset_(QUIET_PRESET);
  }
  // 睡眠和强劲不能同时生效，选择睡眠时显式清除原生 BOOST 预设。
  this->preset = climate::CLIMATE_PRESET_NONE;
  this->transmit_state();
  if (this->sleep_mode_select_ != nullptr)
    this->sleep_mode_select_->publish_state(static_cast<size_t>(mode));
  this->publish_state();
}

void KelvinatorIR::set_vertical_direction_mode(KelvinatorDirectionMode mode) {
  this->vertical_direction_mode_ = mode;
  if (mode != KELVINATOR_DIRECTION_OFF)
    this->swing_mode = this->horizontal_direction_mode_ == KELVINATOR_DIRECTION_OFF
                           ? climate::CLIMATE_SWING_VERTICAL
                           : climate::CLIMATE_SWING_BOTH;
  else
    this->swing_mode = this->horizontal_direction_mode_ == KELVINATOR_DIRECTION_OFF
                           ? climate::CLIMATE_SWING_OFF
                           : climate::CLIMATE_SWING_HORIZONTAL;
  this->transmit_state();
  if (this->vertical_direction_select_ != nullptr)
    this->vertical_direction_select_->publish_state(static_cast<size_t>(mode));
  this->publish_state();
}

void KelvinatorIR::set_horizontal_direction_mode(KelvinatorDirectionMode mode) {
  this->horizontal_direction_mode_ = mode;
  if (mode != KELVINATOR_DIRECTION_OFF)
    this->swing_mode = this->vertical_direction_mode_ == KELVINATOR_DIRECTION_OFF
                           ? climate::CLIMATE_SWING_HORIZONTAL
                           : climate::CLIMATE_SWING_BOTH;
  else
    this->swing_mode = this->vertical_direction_mode_ == KELVINATOR_DIRECTION_OFF
                           ? climate::CLIMATE_SWING_OFF
                           : climate::CLIMATE_SWING_VERTICAL;
  this->transmit_state();
  if (this->horizontal_direction_select_ != nullptr)
    this->horizontal_direction_select_->publish_state(static_cast<size_t>(mode));
  this->publish_state();
}

void KelvinatorIR::set_cool_mode_state(bool state) {
  if (state && this->mode != climate::CLIMATE_MODE_COOL) {
    ESP_LOGW(TAG, "Cool mode can only be enabled in COOL mode");
    this->cool_mode_ = false;
    if (this->cool_mode_switch_ != nullptr)
      this->cool_mode_switch_->publish_state(false);
    return;
  }
  this->cool_mode_ = state;
  this->transmit_state();
  if (this->cool_mode_switch_ != nullptr)
    this->cool_mode_switch_->publish_state(this->cool_mode_);
}

void KelvinatorIR::control(const climate::ClimateCall &call) {
  // ESPHome 会把一次 HA 操作包装成 ClimateCall。这里先维护预设间关系，
  // 再交给 ClimateIR 更新通用的模式、温度、风速并调用 transmit_state()。
  if (call.has_custom_preset()) {
    if (call.get_custom_preset() == ESHARE_PRESET) {
      // 遥控器进入 E享时会切到自动模式和 25°C，但进入后仍允许继续调整风速。
      // 同步修改本地 climate 状态，避免 HA 必须等待红外接收回读才能显示正确值。
      this->mode = climate::CLIMATE_MODE_HEAT_COOL;
      this->target_temperature = 25;
      this->sleep_mode_ = KELVINATOR_SLEEP_OFF;
      this->cool_mode_ = false;
      if (this->sleep_mode_select_ != nullptr)
        this->sleep_mode_select_->publish_state(static_cast<size_t>(this->sleep_mode_));
      if (this->cool_mode_switch_ != nullptr)
        this->cool_mode_switch_->publish_state(false);
    }
    this->set_custom_preset_(call.get_custom_preset());
  } else if (call.get_preset().has_value()) {
    // 选择原生预设（无/强劲）时清除静音或 E享；BOOST 还会退出睡眠。
    this->clear_custom_preset_();
    if (*call.get_preset() == climate::CLIMATE_PRESET_BOOST) {
      this->sleep_mode_ = KELVINATOR_SLEEP_OFF;
      if (this->sleep_mode_select_ != nullptr)
        this->sleep_mode_select_->publish_state(static_cast<size_t>(this->sleep_mode_));
    }
  } else if ((call.get_mode().has_value() || call.get_target_temperature().has_value()) &&
             this->has_custom_preset() && this->get_custom_preset() == ESHARE_PRESET) {
    // E享只允许调整风速；修改模式或温度意味着回到普通控制，因此先清除 E享标志。
    this->clear_custom_preset_();
  } else if (call.get_fan_mode().has_value() &&
             !(this->has_custom_preset() && this->get_custom_preset() == ESHARE_PRESET)) {
    // 普通状态下手动调风会退出静音/强劲；E享内调风是遥控器支持的例外。
    this->preset = climate::CLIMATE_PRESET_NONE;
    this->clear_custom_preset_();
  }

  if (call.get_swing_mode().has_value()) {
    switch (*call.get_swing_mode()) {
      case climate::CLIMATE_SWING_OFF:
        this->vertical_direction_mode_ = KELVINATOR_DIRECTION_OFF;
        this->horizontal_direction_mode_ = KELVINATOR_DIRECTION_OFF;
        break;
      case climate::CLIMATE_SWING_VERTICAL:
        if (this->vertical_direction_mode_ == KELVINATOR_DIRECTION_OFF)
          this->vertical_direction_mode_ = KELVINATOR_DIRECTION_SWING;
        this->horizontal_direction_mode_ = KELVINATOR_DIRECTION_OFF;
        break;
      case climate::CLIMATE_SWING_HORIZONTAL:
        this->vertical_direction_mode_ = KELVINATOR_DIRECTION_OFF;
        if (this->horizontal_direction_mode_ == KELVINATOR_DIRECTION_OFF)
          this->horizontal_direction_mode_ = KELVINATOR_DIRECTION_SWING;
        break;
      case climate::CLIMATE_SWING_BOTH:
        if (this->vertical_direction_mode_ == KELVINATOR_DIRECTION_OFF)
          this->vertical_direction_mode_ = KELVINATOR_DIRECTION_SWING;
        if (this->horizontal_direction_mode_ == KELVINATOR_DIRECTION_OFF)
          this->horizontal_direction_mode_ = KELVINATOR_DIRECTION_SWING;
        break;
      default:
        break;
    }
  }

  if (call.get_mode().has_value() && *call.get_mode() != climate::CLIMATE_MODE_COOL && this->cool_mode_) {
    this->cool_mode_ = false;
    if (this->cool_mode_switch_ != nullptr)
      this->cool_mode_switch_->publish_state(false);
  }
  climate_ir::ClimateIR::control(call);
}

void KelvinatorIR::transmit_state() {
  KelvinatorFrameState state;
  // 原生 BOOST 存在 preset 中；静音和 E享存在 custom preset 中；
  // 睡眠独立保存，才能表达“睡眠开启但静音关闭”这一真实遥控器状态。
  const bool quiet = this->has_custom_preset() && this->get_custom_preset() == QUIET_PRESET;
  const bool eshare = this->has_custom_preset() && this->get_custom_preset() == ESHARE_PRESET;
  state.power = this->mode != climate::CLIMATE_MODE_OFF;
  // 将 HA 的标准 ClimateMode 转成 Kelvinator 的 3 bit 原生模式值。
  switch (this->mode) {
    case climate::CLIMATE_MODE_COOL:
      state.mode = MODE_COOL;
      break;
    case climate::CLIMATE_MODE_DRY:
      state.mode = MODE_DRY;
      break;
    case climate::CLIMATE_MODE_FAN_ONLY:
      state.mode = MODE_FAN;
      break;
    case climate::CLIMATE_MODE_HEAT:
      state.mode = MODE_HEAT;
      break;
    default:
      state.mode = MODE_AUTO;
      break;
  }
  state.temperature = clamp_temperature(static_cast<uint8_t>(this->target_temperature));
  state.turbo = this->preset.value_or(climate::CLIMATE_PRESET_NONE) == climate::CLIMATE_PRESET_BOOST;
  state.quiet = quiet;
  state.light = this->light_;
  state.auxiliary_heat_mode = this->auxiliary_heat_mode_;
  // 协议同时保存 basic_fan（Byte 0）和 fan（Byte 14），两者必须保持一致。
  // 当前 HA 只暴露自动/低/中/高，对应协议值 0~3。
  state.basic_fan = encode_basic_fan_mode(this->fan_mode.value_or(climate::CLIMATE_FAN_AUTO));
  if (eshare) {
    // E享的标志虽然独立存在，但遥控器在进入时还会把模式和温度重置为自动、25°C。
    state.mode = MODE_AUTO;
    state.temperature = 25;
  }
  state.vertical_direction_mode = this->vertical_direction_mode_;
  state.horizontal_direction_mode = this->horizontal_direction_mode_;
  state.eshare = eshare;
  state.cool_mode = this->cool_mode_;
  state.sleep_mode = this->sleep_mode_;

  // 使用显式掩码将语义状态打包为协议字节，避免依赖位域布局。
  const std::array<uint8_t, 16> bytes = pack_regular_command(state);
  const bool sleep_mode_3 = state.sleep_mode == KELVINATOR_SLEEP_3;

  uint64_t block0 = 0;
  uint64_t block1 = 0;
  std::memcpy(&block0, bytes.data(), sizeof(uint64_t));
  std::memcpy(&block1, bytes.data() + 8, sizeof(uint64_t));

  // 常规命令先放入 0x50 和 0x70 两块，校验和在所有功能位写完后统一计算。
  remote_base::KelvinatorData payload;
  payload.data = {block0, block1};
  if (sleep_mode_3) {
    // 睡眠 3 比其他睡眠多发送一个 0x80 块。0xBB 0xBB 0xAB 是实测遥控器
    // 发出的固定温度曲线参数；不要把它并入常规 16 字节状态或随意归零。
    uint64_t sleep_curve_block{};
    uint8_t *sleep_curve_bytes = reinterpret_cast<uint8_t *>(&sleep_curve_block);
    sleep_curve_bytes[0] = bytes[BYTE_8];
    sleep_curve_bytes[1] = bytes[BYTE_9];
    sleep_curve_bytes[2] = bytes[BYTE_10];
    sleep_curve_bytes[3] = SLEEP_CURVE_BLOCK_MARKER;
    sleep_curve_bytes[4] = 0xBB;
    sleep_curve_bytes[5] = 0xBB;
    sleep_curve_bytes[6] = 0xAB;
    payload.data.push_back(sleep_curve_block);
  }
  payload.apply_checksum();
  auto transmit = this->transmitter_->transmit();
  remote_base::KelvinatorProtocol().encode(transmit.get_data(), payload);
  transmit.perform();
}

bool KelvinatorIR::on_receive(remote_base::RemoteReceiveData data) {
  // 底层 decoder 先验证单个 8 字节块的时序与校验和；失败说明不是本协议或数据损坏。
  auto decoded = remote_base::KelvinatorProtocol().decode(data);
  if (!decoded.has_value() || decoded->data.empty())
    return false;

  if (decoded->data.size() == 1) {
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&decoded->data[0]);
    if (bytes[3] == COMMAND_BLOCK_1_MARKER) {
      // ESP32-C3 的 RMT 会把常规命令拆成两个回调。收到 0x50 块时只缓存，
      // 等 0x70 块到达后再发布，避免 HA 看到半条命令形成的中间状态。
      this->pending_first_block_ = decoded->data[0];
      this->pending_first_block_at_ = millis();
      ESP_LOGD(TAG, "Received first Kelvinator block");
      return true;
    }
    if (bytes[3] != COMMAND_BLOCK_2_MARKER || !this->pending_first_block_.has_value())
      return false;
    // 超时说明缓存属于更早的遥控器命令，必须丢弃而不能与当前第二块拼接。
    if (millis() - this->pending_first_block_at_ > SPLIT_BLOCK_TIMEOUT_MS) {
      this->pending_first_block_.reset();
      ESP_LOGW(TAG, "Discarding stale first Kelvinator block");
      return false;
    }
    decoded->data.insert(decoded->data.begin(), *this->pending_first_block_);
    this->pending_first_block_.reset();
    ESP_LOGD(TAG, "Received second Kelvinator block; publishing state");
  }

  // 协议要求第二块的第 8~10 字节重复第一块的第 0~2 字节。若不一致，说明两个 RMT 回调
  // 来自不同的遥控器命令，即使它们各自校验通过也不能组合发布。
  const uint8_t *first_block = reinterpret_cast<const uint8_t *>(&decoded->data[0]);
  const uint8_t *second_block = reinterpret_cast<const uint8_t *>(&decoded->data[1]);
  if (std::memcmp(first_block, second_block, 3) != 0) {
    ESP_LOGW(TAG, "Discarding mismatched Kelvinator command blocks");
    return false;
  }

  std::array<uint8_t, 16> bytes{};
  std::memcpy(bytes.data(), &decoded->data[0], sizeof(uint64_t));
  std::memcpy(bytes.data() + 8, &decoded->data[1], sizeof(uint64_t));
  if (bytes[BYTE_3] != COMMAND_BLOCK_1_MARKER || bytes[BYTE_11] != COMMAND_BLOCK_2_MARKER) {
    ESP_LOGW(TAG, "Discarding Kelvinator command with invalid block markers");
    return false;
  }
  const uint8_t basic_fan_from_byte0 = read_bits(bytes[BYTE_0], MASK_BASIC_FAN, SHIFT_BASIC_FAN);
  const uint8_t fan_from_byte14 = read_bits(bytes[BYTE_14], MASK_FAN, SHIFT_FAN);
  if (basic_fan_from_byte0 != fan_from_byte14) {
    ESP_LOGW(TAG, "Discarding Kelvinator command with mismatched fan bytes (B0=%u, B14=%u)", basic_fan_from_byte0,
             fan_from_byte14);
    return false;
  }
  const KelvinatorFrameState frame = unpack_regular_command(bytes);
  if (!frame.power) {
    this->mode = climate::CLIMATE_MODE_OFF;
  } else {
    switch (frame.mode) {
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
  this->target_temperature = clamp_temperature(frame.temperature);
  // 接收时使用 basic_fan，与遥控器主显示和 Byte 0 的基础风速保持一致。
  this->fan_mode = decode_basic_fan_mode(frame.basic_fan);
  this->vertical_direction_mode_ = frame.vertical_direction_mode;
  this->horizontal_direction_mode_ = frame.horizontal_direction_mode;

  const bool vertical_swing = this->vertical_direction_mode_ != KELVINATOR_DIRECTION_OFF;
  const bool horizontal_swing = this->horizontal_direction_mode_ != KELVINATOR_DIRECTION_OFF;
  if (vertical_swing && horizontal_swing)
    this->swing_mode = climate::CLIMATE_SWING_BOTH;
  else if (vertical_swing)
    this->swing_mode = climate::CLIMATE_SWING_VERTICAL;
  else if (horizontal_swing)
    this->swing_mode = climate::CLIMATE_SWING_HORIZONTAL;
  else
    this->swing_mode = climate::CLIMATE_SWING_OFF;

  if (this->vertical_direction_select_ != nullptr)
    this->vertical_direction_select_->publish_state(static_cast<size_t>(this->vertical_direction_mode_));
  if (this->horizontal_direction_select_ != nullptr)
    this->horizontal_direction_select_->publish_state(static_cast<size_t>(this->horizontal_direction_mode_));

  // E享使用第 15 字节低半字节的独立标志。睡眠档位由第 0 字节最高位与
  // 第 12 字节低位组合判断；睡眠和 quiet 可以同时存在，也可以只保留睡眠。
  const bool eshare = frame.eshare;
  this->sleep_mode_ = frame.sleep_mode;
  if (this->sleep_mode_select_ != nullptr)
    this->sleep_mode_select_->publish_state(static_cast<size_t>(this->sleep_mode_));

  // climate 一次只能显示一个预设，因此按 E享 > 静音 > 强劲的优先级发布。
  // 睡眠不参与该优先级，它通过独立 select 发布，从而可与静音同时显示。
  if (!frame.power) {
    this->clear_custom_preset_();
    this->preset = climate::CLIMATE_PRESET_NONE;
  } else if (eshare) {
    this->set_custom_preset_(ESHARE_PRESET);
  } else if (frame.quiet) {
    this->set_custom_preset_(QUIET_PRESET);
  } else {
    this->clear_custom_preset_();
    this->preset = frame.turbo ? climate::CLIMATE_PRESET_BOOST : climate::CLIMATE_PRESET_NONE;
  }

  this->light_ = frame.light;
  if (this->display_switch_ != nullptr)
    this->display_switch_->publish_state(this->light_);

  this->cool_mode_ = frame.cool_mode;
  if (this->cool_mode_switch_ != nullptr)
    this->cool_mode_switch_->publish_state(this->cool_mode_);

  if (frame.power && frame.mode == MODE_HEAT) {
    // 电辅热只在开机且制热时具有明确语义；其他模式不覆盖 HA 中最后已知档位。
    this->auxiliary_heat_mode_ = frame.auxiliary_heat_mode;
    if (this->auxiliary_heat_select_ != nullptr)
      this->auxiliary_heat_select_->publish_state(static_cast<size_t>(this->auxiliary_heat_mode_));
  }
  this->publish_state();
  return true;
}

}  // namespace kelvinator_ir
}  // namespace esphome
