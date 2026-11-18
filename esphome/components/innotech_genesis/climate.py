import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import climate
from . import ADDRESS, BLOCK, CONF_ADDRESS, CONF_GENESIS_ID, GenesisBus, innotech_ns

DEPENDENCIES = ["innotech_genesis"]

CONF_TEMPERATURE_BLOCK = "temperature_block"
CONF_SETPOINT_BLOCK = "setpoint_block"
CONF_VALVE_BLOCK = "valve_block"
CONF_FAN_BLOCK = "fan_block"
CONF_FAN_OUTPUT_BLOCK = "fan_output_block"
CONF_HEATER_BLOCKS = "heater_blocks"
MAX_HEATERS = 2

GenesisClimate = innotech_ns.class_("GenesisClimate", climate.Climate, cg.Component)

# Role numbers match GenesisClimate::Role, parameter ids match ParamField in innotech_genesis.h
ROLE_CURRENT, ROLE_TARGET, ROLE_VALVE, ROLE_FAN, ROLE_FORCED, ROLE_FORCE_VALUE = 1, 2, 3, 4, 5, 6
ROLE_HEATER_1 = 7  # + stage index
PARAM_FORCED, PARAM_FORCE_VALUE = 1, 2

# One zone as a thermostat: current temperature from the zone's analogue input block, target from its
# setpoint (User Variable) block. valve_block (AHU chilled-water valve %) and fan_block (supply-fan status)
# are optional and drive the action shown in HA (cooling / idle / off).
# fan_output_block (the AHU's supply-fan Digital Output) adds an Off mode: Off forces that output off, Auto
# releases the override. It is shared by every zone on the AHU. Force-on is a separate button (button platform,
# type: force_on) and shows here as Fan only.
CONFIG_SCHEMA = (
    climate.climate_schema(GenesisClimate)
    .extend(
        {
            cv.GenerateID(CONF_GENESIS_ID): cv.use_id(GenesisBus),
            cv.Required(CONF_ADDRESS): ADDRESS,
            cv.Required(CONF_TEMPERATURE_BLOCK): BLOCK,
            cv.Required(CONF_SETPOINT_BLOCK): BLOCK,
            cv.Optional(CONF_VALVE_BLOCK): BLOCK,
            cv.Optional(CONF_FAN_BLOCK): BLOCK,
            cv.Optional(CONF_FAN_OUTPUT_BLOCK): BLOCK,
            # The zone's electric duct heater stage outputs (Digital Outputs); any on = action heating.
            cv.Optional(CONF_HEATER_BLOCKS): cv.All(cv.ensure_list(BLOCK), cv.Length(min=1, max=MAX_HEATERS)),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    bus = await cg.get_variable(config[CONF_GENESIS_ID])
    var = await climate.new_climate(config)
    await cg.register_component(var, config)
    address = config[CONF_ADDRESS]
    cg.add(var.set_bus(bus))
    cg.add(var.set_target(address, config[CONF_SETPOINT_BLOCK]))
    cg.add(bus.add_climate_point(address, config[CONF_TEMPERATURE_BLOCK], ROLE_CURRENT, var))
    cg.add(bus.add_climate_point(address, config[CONF_SETPOINT_BLOCK], ROLE_TARGET, var))
    if CONF_VALVE_BLOCK in config:
        cg.add(var.set_has_valve(True))
        cg.add(bus.add_climate_point(address, config[CONF_VALVE_BLOCK], ROLE_VALVE, var))
    if CONF_FAN_BLOCK in config:
        cg.add(var.set_has_fan(True))
        cg.add(bus.add_climate_point(address, config[CONF_FAN_BLOCK], ROLE_FAN, var))
    if CONF_HEATER_BLOCKS in config:
        cg.add(var.set_has_heater(True))
        for i, block in enumerate(config[CONF_HEATER_BLOCKS]):
            cg.add(bus.add_climate_point(address, block, ROLE_HEATER_1 + i, var))
    if CONF_FAN_OUTPUT_BLOCK in config:
        block = config[CONF_FAN_OUTPUT_BLOCK]
        cg.add(var.set_fan_output_block(block))
        cg.add(bus.add_climate_param(address, block, PARAM_FORCED, ROLE_FORCED, var))
        cg.add(bus.add_climate_param(address, block, PARAM_FORCE_VALUE, ROLE_FORCE_VALUE, var))
