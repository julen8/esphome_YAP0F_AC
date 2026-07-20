#pragma once

#include "esphome/components/remote_base/remote_base.h"
#include "esphome/core/helpers.h"

namespace esphome {
namespace remote_base {

class KelvinatorData {
 public:
  std::vector<uint64_t> data;
  bool is_valid_checksum();
  void apply_checksum();
  uint8_t calculate_block_checksum(uint64_t block);
  void log() const;
};

class KelvinatorProtocol : public RemoteProtocol<KelvinatorData> {
 public:
  void encode(RemoteTransmitData *dst, const KelvinatorData &data) override;
  optional<KelvinatorData> decode(RemoteReceiveData data) override;
  void dump(const KelvinatorData &data) override;

 private:
  void encode_data_(RemoteTransmitData *dst, uint32_t data);
  void encode_footer_(RemoteTransmitData *dst);
  bool decode_data_(RemoteReceiveData &src, uint64_t *data);
  bool decode_data_(RemoteReceiveData &src, uint32_t *data);
  bool decode_footer_(RemoteReceiveData &src);
};

}  // namespace remote_base
}  // namespace esphome
