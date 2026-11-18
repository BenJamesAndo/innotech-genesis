"""Innotech Genesis II DDC (Net protocol) bus master + transparent TCP bridge for iComm."""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.components import socket, uart
from esphome.const import CONF_ID, CONF_FLOW_CONTROL_PIN, CONF_UPDATE_INTERVAL

CODEOWNERS = ["@BenJamesAndo"]
DEPENDENCIES = ["uart"]
AUTO_LOAD = ["socket", "network"]
MULTI_CONF = True

CONF_GENESIS_ID = "genesis_id"
CONF_ADDRESS = "address"
CONF_BLOCK = "block"
CONF_OUTPUT = "output"
CONF_TCP_PORT = "tcp_port"
CONF_STATUS_INTERVAL = "status_interval"
CONF_REPLY_TIMEOUT = "reply_timeout"
CONF_MAX_POINTS = "max_points_per_message"
CONF_SAVE_TO_FLASH = "save_to_flash"
CONF_POLLING = "polling"
CONF_DEVICES = "devices"
CONF_TRANSPORT = "transport"
CONF_ICOMM_HOSTS = "icomm_hosts"
CONF_ICOMM_PORT = "icomm_port"
CONF_ICOMM_CONNECTION = "icomm_connection"
CONF_ICOMM_NAME = "icomm_name"
ICOMM_NAME_MAX = 32  # the hello's application field; iComm shows it in Current Applications

TRANSPORTS = {"serial": 0, "icomm": 1, "auto": 2}

innotech_ns = cg.esphome_ns.namespace("innotech_genesis")
GenesisBus = innotech_ns.class_("GenesisBus", cg.Component, uart.UARTDevice)

ADDRESS = cv.int_range(min=1, max=254)
BLOCK = cv.int_range(min=1, max=255)

def _consume_sockets(config):
    # One socket for the iComm client, always: the server can also be set at runtime (set_icomm_server).
    socket.consume_sockets(1, "innotech_genesis_icomm")(config)
    # The TCP bridge needs one listening socket and one client (plus one spare for a replacing client).
    if config[CONF_TCP_PORT]:
        socket.consume_sockets(2, "innotech_genesis")(config)
        socket.consume_sockets(1, "innotech_genesis", socket.SocketType.TCP_LISTEN)(config)
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(GenesisBus),
            cv.Optional(CONF_FLOW_CONTROL_PIN): pins.gpio_output_pin_schema,
            cv.Optional(CONF_TCP_PORT, default=0): cv.int_range(min=0, max=65535),
            cv.Optional(CONF_UPDATE_INTERVAL, default="5s"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_STATUS_INTERVAL, default="60s"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_REPLY_TIMEOUT, default="1000ms"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_MAX_POINTS, default=20): cv.int_range(min=1, max=30),
            cv.Optional(CONF_SAVE_TO_FLASH, default=True): cv.boolean,
            cv.Optional(CONF_POLLING, default=True): cv.boolean,
            cv.Optional(CONF_DEVICES, default=[]): cv.ensure_list(ADDRESS),
            # serial: talk to the controllers on the UART. icomm: subscribe through an iComm server like Supervisor.
            # auto: serial, falling back to iComm when no controller answers (re-checks serial every 10 min).
            cv.Optional(CONF_TRANSPORT, default="auto"): cv.one_of(*TRANSPORTS, lower=True),
            cv.Optional(CONF_ICOMM_HOSTS, default=[]): cv.ensure_list(cv.ipv4address),
            cv.Optional(CONF_ICOMM_PORT, default=20000): cv.port,
            cv.Optional(CONF_ICOMM_CONNECTION, default=1): cv.int_range(min=1, max=99),
            # Name shown in iComm's Current Applications. Default: "ESPHome <friendly name> <last 4 MAC hex>",
            # so several bridges never look alike.
            cv.Optional(CONF_ICOMM_NAME): cv.All(cv.string_strict, cv.Length(min=1, max=ICOMM_NAME_MAX)),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(uart.UART_DEVICE_SCHEMA),
    _consume_sockets,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
    if CONF_FLOW_CONTROL_PIN in config:
        pin = await cg.gpio_pin_expression(config[CONF_FLOW_CONTROL_PIN])
        cg.add(var.set_flow_control_pin(pin))
    cg.add(var.set_tcp_port(config[CONF_TCP_PORT]))
    cg.add(var.set_update_interval(config[CONF_UPDATE_INTERVAL]))
    cg.add(var.set_status_interval(config[CONF_STATUS_INTERVAL]))
    cg.add(var.set_reply_timeout(config[CONF_REPLY_TIMEOUT]))
    cg.add(var.set_max_points(config[CONF_MAX_POINTS]))
    cg.add(var.set_save_to_flash(config[CONF_SAVE_TO_FLASH]))
    cg.add(var.set_polling(config[CONF_POLLING]))
    cg.add(var.set_transport(TRANSPORTS[config[CONF_TRANSPORT]]))
    for host in config[CONF_ICOMM_HOSTS]:
        cg.add(var.add_icomm_host(str(host)))
    cg.add(var.set_icomm_port(config[CONF_ICOMM_PORT]))
    cg.add(var.set_icomm_connection(config[CONF_ICOMM_CONNECTION]))
    if CONF_ICOMM_NAME in config:
        cg.add(var.set_icomm_name(config[CONF_ICOMM_NAME]))
    for addr in config[CONF_DEVICES]:
        cg.add(var.add_device(addr))
