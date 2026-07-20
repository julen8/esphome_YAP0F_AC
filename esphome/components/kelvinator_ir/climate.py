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

CONF_AUXILIARY_HEAT = "auxiliary_heat"
AUXILIARY_HEAT_OPTIONS = ["关闭", "自动", "开启"]

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
    }
)


async def to_code(config):
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
