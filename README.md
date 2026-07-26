# 适配格力空调(遥控器YAP0F)的esphome组件

## 支持功能：
- 发送红外控制空调
- 接收遥控器的红外信号，并同步状态
- 支持灯光控制
- 支持 "强劲" 模式
- 支持 "静音" 模式
- 支持 "E享" 模式
- 支持 "睡眠1" / "睡眠2" / "睡眠3" / "睡眠4" 模式
- 支持上下5个风向控制
- 支持左右7个风向控制
- 支持制冷模式下凉爽功能开关
- 支持制热模式下控制电辅热

## 适用设备

esp32-c3实测

## 示例配置

```yaml
substitutions:
  device_name: shufang
  friendly_name: 书房

esphome:
  name: ${device_name}
  friendly_name: ${friendly_name}空调
  devices:
    - id: ${device_name}_ac_device
      name: ${friendly_name}空调

esp32:
  board: esp32-c3-devkitm-1
  framework:
    type: arduino
    version: latest

logger:
  level: DEBUG

# Enable Home Assistant API
api:
  encryption:
    key: !secret api_shufang_key

ota:
  - platform: esphome
    password: !secret ota_key

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password

debug:
  update_interval: 5s

web_server:
  port: 80
  version: 3
  include_internal: true

external_components:
  - source: github://julen8/esphome_YAP0F_AC
    components: [ kelvinator_ir ]

climate:
  - platform: kelvinator_ir
    id: ${device_name}_ac
    name: ${friendly_name}空调
    device_id: ${device_name}_ac_device
    receiver_id: ${device_name}_ir_receiver
    supports_heat: true
    light: true
    display:
      name: ${friendly_name}空调显示灯
      device_id: ${device_name}_ac_device
    vertical_direction:
      id: ${device_name}_vertical_direction
      name: ${friendly_name}空调上下风向 UD
      device_id: ${device_name}_ac_device
    horizontal_direction:
      id: ${device_name}_horizontal_direction
      name: ${friendly_name}空调左右风向 LR
      device_id: ${device_name}_ac_device
    auxiliary_heat:
      name: ${friendly_name}空调电辅热
      device_id: ${device_name}_ac_device
    cool_mode:
      name: ${friendly_name}空调凉爽模式
      device_id: ${device_name}_ac_device
    sleep_mode:
      id: ${device_name}_sleep_mode
      name: ${friendly_name}空调睡眠模式 SLP
      device_id: ${device_name}_ac_device

remote_transmitter:
  pin: 1
  carrier_duty_percent: 25%
  non_blocking: true

remote_receiver:
  id: ${device_name}_ir_receiver
  pin:
    number: 0
    inverted: true
    mode:
      input: true
      pullup: true
  tolerance: 55%
  idle: 30ms
```
