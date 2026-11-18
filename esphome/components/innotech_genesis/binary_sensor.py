import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor
from esphome.const import CONF_TYPE
from . import ADDRESS, BLOCK, CONF_ADDRESS, CONF_BLOCK, CONF_GENESIS_ID, CONF_OUTPUT, GenesisBus

DEPENDENCIES = ["innotech_genesis"]


def _check(config):
    if config[CONF_TYPE] == "point" and CONF_BLOCK not in config:
        raise cv.Invalid("block is required for type: point")
    return config


# type: point  -> a digital block output (fan status, after-hours switch...), from the regular poll
# type: online -> the controller answered its last status check with the "running" bit set
CONFIG_SCHEMA = cv.All(
    binary_sensor.binary_sensor_schema().extend(
        {
            cv.GenerateID(CONF_GENESIS_ID): cv.use_id(GenesisBus),
            cv.Required(CONF_ADDRESS): ADDRESS,
            cv.Optional(CONF_TYPE, default="point"): cv.one_of("point", "online", lower=True),
            cv.Optional(CONF_BLOCK): BLOCK,
            cv.Optional(CONF_OUTPUT, default=1): cv.int_range(min=1, max=16),
        }
    ),
    _check,
)


async def to_code(config):
    bus = await cg.get_variable(config[CONF_GENESIS_ID])
    s = await binary_sensor.new_binary_sensor(config)
    if config[CONF_TYPE] == "online":
        cg.add(bus.add_device(config[CONF_ADDRESS]))
        cg.add(bus.set_online_sensor(config[CONF_ADDRESS], s))
    else:
        cg.add(bus.add_binary_point(config[CONF_ADDRESS], config[CONF_BLOCK], config[CONF_OUTPUT], s))
