#include "kelvinator_ir.h"
#include <cstring>
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace kelvinator_ir {

static const char *const TAG = "climate.kelvinator_ir";

static const uint8_t MODE_AUTO = 0;
static const uint8_t MODE_COOL = 1;
static const uint8_t MODE_DRY = 2;
static const uint8_t MODE_FAN = 3;
static const uint8_t MODE_HEAT = 4;

// 第 3 字节的高半字节用于区分常规第一块、常规第二块和睡眠曲线块。
static const uint8_t COMMAND_BLOCK_1_MARKER = 0x50;
static const uint8_t COMMAND_BLOCK_2_MARKER = 0x70;
static const uint8_t SLEEP_CURVE_BLOCK_MARKER = 0x80;
// E享标志位于第 15 字节低半字节；高半字节由校验函数在发送前覆盖。
static const uint8_t ESHARE_MARKER = 0x03;
// 左右风向由第 4 字节高半字节离散编码（实机抓包）：
// 关闭=0x0, 扫风=0x1, 位置1=0x2, 位置2=0x3, 位置3=0x4, 位置4=0x5,
// 位置5=0x6, 定向向两边=0xC, 交替扫风=0xD。
static const uint8_t HORIZONTAL_CODE_OFF = 0x0;
static const uint8_t HORIZONTAL_CODE_SWING = 0x1;
static const uint8_t HORIZONTAL_CODE_POS_1 = 0x2;
static const uint8_t HORIZONTAL_CODE_POS_2 = 0x3;
static const uint8_t HORIZONTAL_CODE_POS_3 = 0x4;
static const uint8_t HORIZONTAL_CODE_POS_4 = 0x5;
static const uint8_t HORIZONTAL_CODE_POS_5 = 0x6;
static const uint8_t HORIZONTAL_CODE_OUTWARD = 0xC;
static const uint8_t HORIZONTAL_CODE_ALTERNATING = 0xD;
static const uint8_t COOL_MODE_MARKER = 0x08;
// 遥控器两块之间只有约 40ms，500ms 足以容纳正常抖动，同时避免跨命令配对。
static const uint32_t SPLIT_BLOCK_TIMEOUT_MS = 500;

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
  this->display_switch_->publish_state(state);
}

void KelvinatorIR::set_auxiliary_heat_mode(KelvinatorAuxiliaryHeatMode mode) {
  // 电辅热由两个协议位组合表示：关闭位和自动位，具体映射在 transmit_state() 中完成。
  this->auxiliary_heat_mode_ = mode;
  this->transmit_state();
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
  this->horizontal_direction_select_->publish_state(static_cast<size_t>(mode));
  this->publish_state();
}

void KelvinatorIR::set_cool_mode_state(bool state) {
  if (state && this->mode != climate::CLIMATE_MODE_COOL) {
    ESP_LOGW(TAG, "Cool mode can only be enabled in COOL mode");
    this->cool_mode_ = false;
    this->cool_mode_switch_->publish_state(false);
    return;
  }
  this->cool_mode_ = state;
  this->transmit_state();
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
      if (this->sleep_mode_select_ != nullptr)
        this->sleep_mode_select_->publish_state(static_cast<size_t>(this->sleep_mode_));
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
  // 每次都从零初始化完整状态，避免上一条命令的睡眠、E享或校验位残留。
  KelvinatorCommand command{};
  // 原生 BOOST 存在 preset 中；静音和 E享存在 custom preset 中；
  // 睡眠独立保存，才能表达“睡眠开启但静音关闭”这一真实遥控器状态。
  const bool quiet = this->has_custom_preset() && this->get_custom_preset() == QUIET_PRESET;
  const bool eshare = this->has_custom_preset() && this->get_custom_preset() == ESHARE_PRESET;
  const bool sleep_mode_1 = this->sleep_mode_ == KELVINATOR_SLEEP_1;
  const bool sleep_mode_2 = this->sleep_mode_ == KELVINATOR_SLEEP_2;
  const bool sleep_mode_3 = this->sleep_mode_ == KELVINATOR_SLEEP_3;
  const bool sleep_mode_4 = this->sleep_mode_ == KELVINATOR_SLEEP_4;
  command.power = this->mode != climate::CLIMATE_MODE_OFF;
  // 将 HA 的标准 ClimateMode 转成 Kelvinator 的 3 bit 原生模式值。
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
  command.quiet = quiet;
  command.light = this->light_;
  command.auxiliary_heat_off = this->auxiliary_heat_mode_ == KELVINATOR_AUXILIARY_HEAT_OFF;
  command.auxiliary_heat_auto = this->auxiliary_heat_mode_ == KELVINATOR_AUXILIARY_HEAT_AUTO;
  // 协议同时保存 basic_fan（Byte 0）和 fan（Byte 14），两者必须保持一致。
  // 当前 HA 只暴露自动/低/中/高，对应协议值 0~3。
  command.basic_fan = 0;
  if (this->fan_mode == climate::CLIMATE_FAN_LOW)
    command.basic_fan = 1;
  else if (this->fan_mode == climate::CLIMATE_FAN_MEDIUM)
    command.basic_fan = 2;
  else if (this->fan_mode == climate::CLIMATE_FAN_HIGH)
    command.basic_fan = 3;
  command.fan = command.basic_fan;
  if (eshare) {
    // E享的标志虽然独立存在，但遥控器在进入时还会把模式和温度重置为自动、25°C。
    command.mode = MODE_AUTO;
    command.temperature = 9;
  }
  command.swing_vertical = static_cast<uint8_t>(this->vertical_direction_mode_);
  const uint8_t horizontal_code = encode_horizontal_direction(this->horizontal_direction_mode_);
  command.raw8[4] = static_cast<uint8_t>((horizontal_code << 4) | (command.raw8[4] & 0x0F));
  command.swing_auto = this->vertical_direction_mode_ == KELVINATOR_DIRECTION_SWING ||
                       this->horizontal_direction_mode_ == KELVINATOR_DIRECTION_SWING ||
                       this->horizontal_direction_mode_ == KELVINATOR_DIRECTION_ALTERNATING;
  command.raw8[3] = COMMAND_BLOCK_1_MARKER;
  command.raw8[8] = command.raw8[0];
  command.raw8[9] = command.raw8[1];
  command.raw8[10] = command.raw8[2];
  command.raw8[11] = COMMAND_BLOCK_2_MARKER;
  if (this->cool_mode_ && this->mode == climate::CLIMATE_MODE_COOL)
    command.raw8[15] = (command.raw8[15] & 0xF0) | ((command.raw8[15] & 0x0F) | COOL_MODE_MARKER);
  // E享写入第 15 字节低半字节；高半字节是校验和，稍后由 apply_checksum() 生成。
  if (eshare)
    command.raw8[15] = (command.raw8[15] & 0xF0) | ESHARE_MARKER;
  // 睡眠位与静音位相互独立。选择睡眠时默认打开静音，但之后可以只清除 quiet，
  // 不修改下面这些睡眠位，因此睡眠 select 会继续保持原来的档位。
  if (sleep_mode_1)
    command.raw8[0] |= 0x80;
  else if (sleep_mode_2)
    command.raw8[12] |= 0x01;
  else if (sleep_mode_3) {
    command.raw8[0] |= 0x80;
    command.raw8[12] |= 0x01;
    command.raw8[13] = 0xA9;
  } else if (sleep_mode_4) {
    command.raw8[0] |= 0x80;
    command.raw8[12] |= 0x08;
  }
  // 第 8 字节必须重复最终的第 0 字节。睡眠 1/3 修改了第 0 字节最高位，所以需要再次复制。
  command.raw8[8] = command.raw8[0];

  // 常规命令先放入 0x50 和 0x70 两块，校验和在所有功能位写完后统一计算。
  remote_base::KelvinatorData payload;
  payload.data = {command.raw64[0], command.raw64[1]};
  if (sleep_mode_3) {
    // 睡眠 3 比其他睡眠多发送一个 0x80 块。0xBB 0xBB 0xAB 是实测遥控器
    // 发出的固定温度曲线参数；不要把它并入常规 16 字节状态或随意归零。
    uint64_t sleep_curve_block{};
    uint8_t *bytes = reinterpret_cast<uint8_t *>(&sleep_curve_block);
    bytes[0] = command.raw8[0];
    bytes[1] = command.raw8[1];
    bytes[2] = command.raw8[2];
    bytes[3] = SLEEP_CURVE_BLOCK_MARKER;
    bytes[4] = 0xBB;
    bytes[5] = 0xBB;
    bytes[6] = 0xAB;
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

  KelvinatorCommand command{};
  // 两个已配对的 uint64_t 直接装入联合体，之后可通过位域解析所有已知状态。
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
  // 接收时使用 basic_fan，与遥控器主显示和 Byte 0 的基础风速保持一致。
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
  if (command.swing_vertical <= KELVINATOR_DIRECTION_POSITION_5)
    this->vertical_direction_mode_ = static_cast<KelvinatorDirectionMode>(command.swing_vertical);
  else
    this->vertical_direction_mode_ = KELVINATOR_DIRECTION_SWING;

  this->horizontal_direction_mode_ = decode_horizontal_direction(command.raw8[4] >> 4);

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
  const bool eshare = command.power && (command.raw8[15] & 0x0F) == ESHARE_MARKER;
  const bool sleep_mode_1 = (command.raw8[0] & 0x80) && (command.raw8[12] & 0x09) == 0;
  const bool sleep_mode_2 = !(command.raw8[0] & 0x80) && (command.raw8[12] & 0x01);
  const bool sleep_mode_3 = (command.raw8[0] & 0x80) && (command.raw8[12] & 0x01);
  const bool sleep_mode_4 = (command.raw8[0] & 0x80) && (command.raw8[12] & 0x08);
  if (sleep_mode_4)
    this->sleep_mode_ = KELVINATOR_SLEEP_4;
  else if (sleep_mode_3)
    this->sleep_mode_ = KELVINATOR_SLEEP_3;
  else if (sleep_mode_2)
    this->sleep_mode_ = KELVINATOR_SLEEP_2;
  else if (sleep_mode_1)
    this->sleep_mode_ = KELVINATOR_SLEEP_1;
  else
    this->sleep_mode_ = KELVINATOR_SLEEP_OFF;
  if (this->sleep_mode_select_ != nullptr)
    this->sleep_mode_select_->publish_state(static_cast<size_t>(this->sleep_mode_));

  // climate 一次只能显示一个预设，因此按 E享 > 静音 > 强劲的优先级发布。
  // 睡眠不参与该优先级，它通过独立 select 发布，从而可与静音同时显示。
  if (eshare) {
    this->set_custom_preset_(ESHARE_PRESET);
  } else if (command.quiet) {
    this->set_custom_preset_(QUIET_PRESET);
  } else {
    this->clear_custom_preset_();
    this->preset = command.turbo ? climate::CLIMATE_PRESET_BOOST : climate::CLIMATE_PRESET_NONE;
  }

  this->light_ = command.light;
  if (this->display_switch_ != nullptr)
    this->display_switch_->publish_state(this->light_);

  this->cool_mode_ = command.power && command.mode == MODE_COOL && ((command.raw8[15] & COOL_MODE_MARKER) != 0);
  if (this->cool_mode_switch_ != nullptr)
    this->cool_mode_switch_->publish_state(this->cool_mode_);

  if (command.power && command.mode == MODE_HEAT) {
    // 电辅热只在开机且制热时具有明确语义；其他模式不覆盖 HA 中最后已知档位。
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
