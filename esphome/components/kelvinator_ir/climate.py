import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import climate_ir, select, switch
from esphome.const import CONF_DISPLAY, CONF_LIGHT

AUTO_LOAD = ["climate_ir", "select", "switch"]
CODEOWNERS = ["@michael-horne"]

kelvinator_ir_ns = cg.esphome_ns.namespace("kelvinator_ir")
KelvinatorIR = kelvinator_ir_ns.class_("KelvinatorIR", climate_ir.ClimateIR)
KelvinatorDisplaySwitch = kelvinator_ir_ns.class_(
    "KelvinatorDisplaySwitch", switch.Switch
)
KelvinatorAuxiliaryHeatSelect = kelvinator_ir_ns.class_(
    "KelvinatorAuxiliaryHeatSelect", select.Select
)
KelvinatorSleepModeSelect = kelvinator_ir_ns.class_(
    "KelvinatorSleepModeSelect", select.Select
)
KelvinatorVerticalDirectionSelect = kelvinator_ir_ns.class_(
    "KelvinatorVerticalDirectionSelect", select.Select
)
KelvinatorHorizontalDirectionSelect = kelvinator_ir_ns.class_(
    "KelvinatorHorizontalDirectionSelect", select.Select
)
KelvinatorCoolModeSwitch = kelvinator_ir_ns.class_(
    "KelvinatorCoolModeSwitch", switch.Switch
)

CONF_AUXILIARY_HEAT = "auxiliary_heat"
CONF_SLEEP_MODE = "sleep_mode"
CONF_VERTICAL_DIRECTION = "vertical_direction"
CONF_HORIZONTAL_DIRECTION = "horizontal_direction"
CONF_COOL_MODE = "cool_mode"
# select 的选项索引会直接转换成 C++ 枚举值，调整顺序时必须同步修改头文件枚举。
AUXILIARY_HEAT_OPTIONS = ["关闭", "自动", "开启"]
SLEEP_MODE_OPTIONS = ["关闭", "睡眠1", "睡眠2", "睡眠3", "睡眠4"]
VERTICAL_DIRECTION_OPTIONS = ["关闭", "扫风", "位置1", "位置2", "位置3", "位置4", "位置5"]
HORIZONTAL_DIRECTION_OPTIONS = [
    "关闭",
    "扫风",
    "位置1",
    "位置2",
    "位置3",
    "位置4",
    "位置5",
    "定向向两边",
    "交替扫风",
]

CONFIG_SCHEMA = climate_ir.climate_ir_with_receiver_schema(KelvinatorIR).extend(
    {
        cv.Optional(CONF_LIGHT, default=True): cv.boolean,
        cv.Optional(CONF_DISPLAY): switch.switch_schema(
            KelvinatorDisplaySwitch,
            icon="mdi:led-on",
            default_restore_mode="DISABLED",
        ),
        cv.Optional(CONF_AUXILIARY_HEAT): select.select_schema(
            KelvinatorAuxiliaryHeatSelect,
            icon="mdi:radiator",
        ),
        cv.Optional(CONF_SLEEP_MODE): select.select_schema(
            KelvinatorSleepModeSelect,
            icon="mdi:sleep",
        ),
        cv.Optional(CONF_VERTICAL_DIRECTION): select.select_schema(
            KelvinatorVerticalDirectionSelect,
            icon="mdi:arrow-up-down",
        ),
        cv.Optional(CONF_HORIZONTAL_DIRECTION): select.select_schema(
            KelvinatorHorizontalDirectionSelect,
            icon="mdi:arrow-left-right",
        ),
        cv.Optional(CONF_COOL_MODE): switch.switch_schema(
            KelvinatorCoolModeSwitch,
            icon="mdi:snowflake-thermometer",
            default_restore_mode="DISABLED",
        ),
    }
)


async def to_code(config):
    # 创建主 climate 实体，并按 YAML 中是否配置子实体来注册显示灯、电辅热和睡眠模式。
    # 子实体构造时传入 var，C++ 端通过 parent 指针把操作汇总成一条完整空调命令。
    var = await climate_ir.new_climate_ir(config)
    cg.add(var.set_light(config[CONF_LIGHT]))

    if display_config := config.get(CONF_DISPLAY):
        display = await switch.new_switch(display_config, var)
        cg.add(var.set_display_switch(display))

    if auxiliary_heat_config := config.get(CONF_AUXILIARY_HEAT):
        auxiliary_heat = await select.new_select(
            auxiliary_heat_config,
            var,
            options=AUXILIARY_HEAT_OPTIONS,
        )
        cg.add(var.set_auxiliary_heat_select(auxiliary_heat))

    if sleep_mode_config := config.get(CONF_SLEEP_MODE):
        sleep_mode = await select.new_select(
            sleep_mode_config,
            var,
            options=SLEEP_MODE_OPTIONS,
        )
        cg.add(var.set_sleep_mode_select(sleep_mode))

    if vertical_direction_config := config.get(CONF_VERTICAL_DIRECTION):
        vertical_direction = await select.new_select(
            vertical_direction_config,
            var,
            options=VERTICAL_DIRECTION_OPTIONS,
        )
        cg.add(var.set_vertical_direction_select(vertical_direction))

    if horizontal_direction_config := config.get(CONF_HORIZONTAL_DIRECTION):
        horizontal_direction = await select.new_select(
            horizontal_direction_config,
            var,
            options=HORIZONTAL_DIRECTION_OPTIONS,
        )
        cg.add(var.set_horizontal_direction_select(horizontal_direction))

    if cool_mode_config := config.get(CONF_COOL_MODE):
        cool_mode = await switch.new_switch(cool_mode_config, var)
        cg.add(var.set_cool_mode_switch(cool_mode))
