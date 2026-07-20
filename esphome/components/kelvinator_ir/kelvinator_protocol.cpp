#include "kelvinator_protocol.h"
#include "esphome/core/log.h"

namespace esphome {
namespace remote_base {

static const char *const TAG = "remote.kelvinator";

static const int32_t TICK_US = 85;
static const int32_t HEADER_MARK_US = 106 * TICK_US;
static const int32_t HEADER_SPACE_US = 53 * TICK_US;
static const int32_t BIT_MARK_US = 8 * TICK_US;
static const int32_t BIT_ONE_SPACE_US = 18 * TICK_US;
static const int32_t BIT_ZERO_SPACE_US = 6 * TICK_US;
static const int32_t GAP_SPACE_US = 235 * TICK_US;
static const int32_t DOUBLE_GAP_SPACE_US = 2 * GAP_SPACE_US;

void KelvinatorProtocol::encode_data_(RemoteTransmitData *dst, const uint32_t data) {
  const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&data);
  for (unsigned byte_index = 0; byte_index < 4; byte_index++) {
    for (uint8_t bit = 0; bit < 8; bit++) {
      dst->mark(BIT_MARK_US);
      dst->space(bytes[byte_index] & (1 << bit) ? BIT_ONE_SPACE_US : BIT_ZERO_SPACE_US);
    }
  }
}

void KelvinatorProtocol::encode_footer_(RemoteTransmitData *dst) {
  dst->item(BIT_MARK_US, BIT_ZERO_SPACE_US);
  dst->item(BIT_MARK_US, BIT_ONE_SPACE_US);
  dst->item(BIT_MARK_US, BIT_ZERO_SPACE_US);
  dst->item(BIT_MARK_US, GAP_SPACE_US);
}

void KelvinatorProtocol::encode(RemoteTransmitData *dst, const KelvinatorData &data) {
  dst->set_carrier_frequency(38000);
  dst->reserve(276 * data.data.size());
  for (size_t block_index = 0; block_index < data.data.size(); block_index++) {
    const uint32_t *words = reinterpret_cast<const uint32_t *>(&data.data[block_index]);
    dst->item(HEADER_MARK_US, HEADER_SPACE_US);
    this->encode_data_(dst, words[0]);
    this->encode_footer_(dst);
    this->encode_data_(dst, words[1]);
    dst->item(BIT_MARK_US, DOUBLE_GAP_SPACE_US);
  }
}

bool KelvinatorProtocol::decode_footer_(RemoteReceiveData &src) {
  return src.expect_item(BIT_MARK_US, BIT_ZERO_SPACE_US) && src.expect_item(BIT_MARK_US, BIT_ONE_SPACE_US) &&
         src.expect_item(BIT_MARK_US, BIT_ZERO_SPACE_US) && src.expect_item(BIT_MARK_US, GAP_SPACE_US);
}

bool KelvinatorProtocol::decode_data_(RemoteReceiveData &src, uint32_t *data) {
  uint8_t *bytes = reinterpret_cast<uint8_t *>(data);
  for (unsigned byte_index = 0; byte_index < 4; byte_index++) {
    uint8_t value = 0;
    for (uint8_t bit = 0; bit < 8; bit++) {
      if (!src.expect_mark(BIT_MARK_US))
        return false;
      if (src.expect_space(BIT_ONE_SPACE_US))
        value |= 1 << bit;
      else if (!src.expect_space(BIT_ZERO_SPACE_US))
        return false;
    }
    bytes[byte_index] = value;
  }
  return true;
}

bool KelvinatorProtocol::decode_data_(RemoteReceiveData &src, uint64_t *data) {
  uint32_t *words = reinterpret_cast<uint32_t *>(data);
  return src.expect_item(HEADER_MARK_US, HEADER_SPACE_US) && this->decode_data_(src, &words[0]) &&
         this->decode_footer_(src) && this->decode_data_(src, &words[1]);
}

optional<KelvinatorData> KelvinatorProtocol::decode(RemoteReceiveData data) {
  KelvinatorData result;
  uint64_t block;
  if (!this->decode_data_(data, &block))
    return {};
  result.data.push_back(block);

  if (!result.is_valid_checksum())
    return {};
  result.log();
  return result;
}

uint8_t KelvinatorData::calculate_block_checksum(const uint64_t block) {
  const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&block);
  uint8_t sum = 10;
  for (uint8_t index = 0; index < 4; index++)
    sum += bytes[index] & 0x0F;
  for (uint8_t index = 4; index < 7; index++)
    sum += bytes[index] >> 4;
  return sum & 0x0F;
}

void KelvinatorData::apply_checksum() {
  for (auto &block : this->data) {
    uint8_t *bytes = reinterpret_cast<uint8_t *>(&block);
    bytes[7] = (bytes[7] & 0x0F) | (this->calculate_block_checksum(block) << 4);
  }
}

bool KelvinatorData::is_valid_checksum() {
  for (auto block : this->data) {
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&block);
    if ((bytes[7] >> 4) != this->calculate_block_checksum(block))
      return false;
  }
  return true;
}

void KelvinatorProtocol::dump(const KelvinatorData &data) { data.log(); }

void KelvinatorData::log() const {
  for (size_t index = 0; index < this->data.size(); index++) {
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&this->data[index]);
    ESP_LOGD(TAG, "Received Kelvinator block %u: %s", index, format_hex_pretty(bytes, sizeof(uint64_t)).c_str());
  }
}

}  // namespace remote_base
}  // namespace esphome
