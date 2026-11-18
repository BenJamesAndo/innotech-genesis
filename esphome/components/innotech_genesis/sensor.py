import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from . import ADDRESS, BLOCK, CONF_ADDRESS, CONF_BLOCK, CONF_GENESIS_ID, CONF_OUTPUT, GenesisBus

DEPENDENCIES = ["innotech_genesis"]

CONF_ICOMM_FIELD = "icomm_field"

# Fixed iComm field names for outputs that aren't called OUTPUT/Value/State. The numbers are indices into
# IC_FIELDS in innotech_genesis.cpp. Over serial only `output` matters (PID loop: 1 = Direct, 2 = Reverse).
ICOMM_FIELDS = {"Direct": 5, "Reverse": 6}

CONFIG_SCHEMA = sensor.sensor_schema(accuracy_decimals=1).extend(
    {
        cv.GenerateID(CONF_GENESIS_ID): cv.use_id(GenesisBus),
        cv.Required(CONF_ADDRESS): ADDRESS,
        cv.Required(CONF_BLOCK): BLOCK,
        cv.Optional(CONF_OUTPUT, default=1): cv.int_range(min=1, max=16),
        cv.Optional(CONF_ICOMM_FIELD): cv.one_of(*ICOMM_FIELDS),
    }
)


async def to_code(config):
    bus = await cg.get_variable(config[CONF_GENESIS_ID])
    s = await sensor.new_sensor(config)
    field = ICOMM_FIELDS.get(config.get(CONF_ICOMM_FIELD), 0)
    cg.add(bus.add_sensor(config[CONF_ADDRESS], config[CONF_BLOCK], config[CONF_OUTPUT], s, field))
