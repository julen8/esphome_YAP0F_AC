#pragma once

#include "esphome/components/remote_base/remote_base.h"
#include "esphome/core/helpers.h"

namespace esphome {
namespace remote_base {

class KelvinatorData {
 public:
  // 每个 uint64_t 表示一个独立的 8 字节协议块。常规命令有两块，睡眠 3 有三块。
  std::vector<uint64_t> data;
  bool is_valid_checksum();
  void apply_checksum();
  uint8_t calculate_block_checksum(uint64_t block);
  void log() const;
};

class KelvinatorProtocol : public RemoteProtocol<KelvinatorData> {
 public:
  // RemoteProtocol 接口：负责协议时序编码、单块解码和调试日志输出。
  void encode(RemoteTransmitData *dst, const KelvinatorData &data) override;
  optional<KelvinatorData> decode(RemoteReceiveData data) override;
  void dump(const KelvinatorData &data) override;

 private:
  // 一个协议块由两个 32 位区段组成，中间插入固定 footer。
  void encode_data_(RemoteTransmitData *dst, uint32_t data);
  void encode_footer_(RemoteTransmitData *dst);
  bool decode_data_(RemoteReceiveData &src, uint64_t *data);
  bool decode_data_(RemoteReceiveData &src, uint32_t *data);
  bool decode_footer_(RemoteReceiveData &src);
};

}  // namespace remote_base
}  // namespace esphome
