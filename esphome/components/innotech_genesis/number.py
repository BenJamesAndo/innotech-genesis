import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import number
from esphome.const import CONF_MAX_VALUE, CONF_MIN_VALUE, CONF_STEP
from . import ADDRESS, BLOCK, CONF_ADDRESS, CONF_BLOCK, CONF_GENESIS_ID, GenesisBus, innotech_ns

DEPENDENCIES = ["innotech_genesis"]

GenesisNumber = innotech_ns.class_("GenesisNumber", number.Number, cg.Component)

# A Genesis "User Variable" block (setpoints, after-hours timers). Writes do read-modify-write of the block.
CONFIG_SCHEMA = (
    number.number_schema(GenesisNumber)
    .extend(
        {
            cv.GenerateID(CONF_GENESIS_ID): cv.use_id(GenesisBus),
            cv.Required(CONF_ADDRESS): ADDRESS,
            cv.Required(CONF_BLOCK): BLOCK,
            cv.Optional(CONF_MIN_VALUE, default=15.0): cv.float_,
            cv.Optional(CONF_MAX_VALUE, default=30.0): cv.float_,
            cv.Optional(CONF_STEP, default=0.5): cv.positive_float,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    bus = await cg.get_variable(config[CONF_GENESIS_ID])
    n = await number.new_number(
        config, min_value=config[CONF_MIN_VALUE], max_value=config[CONF_MAX_VALUE], step=config[CONF_STEP]
    )
    await cg.register_component(n, config)
    cg.add(n.set_bus(bus))
    cg.add(n.set_target(config[CONF_ADDRESS], config[CONF_BLOCK]))
    cg.add(bus.add_number(config[CONF_ADDRESS], config[CONF_BLOCK], n))
