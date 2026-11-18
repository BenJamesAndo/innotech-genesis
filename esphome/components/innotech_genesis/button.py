import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import button
from esphome.const import CONF_TYPE
from . import ADDRESS, BLOCK, CONF_ADDRESS, CONF_GENESIS_ID, GenesisBus, innotech_ns

DEPENDENCIES = ["innotech_genesis"]

CONF_OUTPUT_BLOCK = "output_block"

GenesisScanButton = innotech_ns.class_("GenesisScanButton", button.Button, cg.Component)
GenesisForceOnButton = innotech_ns.class_("GenesisForceOnButton", button.Button, cg.Component)

_BASE = {
    cv.GenerateID(CONF_GENESIS_ID): cv.use_id(GenesisBus),
    cv.Required(CONF_ADDRESS): ADDRESS,
}

CONFIG_SCHEMA = cv.typed_schema(
    {
        # Logs every block (number, type, name, User Variable values) of one controller.
        "scan": button.button_schema(GenesisScanButton, icon="mdi:format-list-numbered")
        .extend(_BASE)
        .extend(cv.COMPONENT_SCHEMA),
        # Forces a Digital Output (e.g. an AHU supply fan) ON. This overrides the controller's schedule,
        # after-hours switch and fire-alarm logic; release it by setting one of that AHU's zones to Auto.
        "force_on": button.button_schema(GenesisForceOnButton, icon="mdi:fan-alert")
        .extend(_BASE)
        .extend({cv.Required(CONF_OUTPUT_BLOCK): BLOCK})
        .extend(cv.COMPONENT_SCHEMA),
    },
    default_type="scan",
)


async def to_code(config):
    bus = await cg.get_variable(config[CONF_GENESIS_ID])
    b = await button.new_button(config)
    await cg.register_component(b, config)
    cg.add(b.set_bus(bus))
    if config[CONF_TYPE] == "force_on":
        cg.add(b.set_target(config[CONF_ADDRESS], config[CONF_OUTPUT_BLOCK]))
    else:
        cg.add(b.set_address(config[CONF_ADDRESS]))
