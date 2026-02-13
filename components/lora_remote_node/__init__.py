import base64

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sx126x, sensor, binary_sensor, time as time_component
from esphome.const import CONF_ID, CONF_SENSORS, CONF_BINARY_SENSORS, CONF_TIME_ID, CONF_ADDRESS

CODEOWNERS = ["@mikedamage"]
DEPENDENCIES = ["sx126x"]

CONF_SX126X_ID = "sx126x_id"
CONF_AUTH_KEY = "auth_key"
CONF_LISTEN_WINDOW = "listen_window"

lora_remote_node_ns = cg.esphome_ns.namespace("lora_remote_node")
LoraRemoteNode = lora_remote_node_ns.class_(
    "LoraRemoteNode", cg.Component, sx126x.SX126xListener
)


def validate_address_range(value):
    """Validate that address is in valid range (0x01-0xFE)."""
    if value < 0x01 or value > 0xFE:
        raise cv.Invalid(f"Address must be between 0x01 and 0xFE, got 0x{value:02X}")
    return value


def validate_auth_key(value):
    """Validate and parse a 16-byte authentication key.

    Accepts either a 32-character hex string or a base64-encoded 16-byte key.
    """
    value = cv.string(value)
    # Try hex string (32 hex chars = 16 bytes)
    cleaned = value.replace(" ", "").replace(":", "").replace("-", "")
    if len(cleaned) == 32:
        try:
            return list(bytes.fromhex(cleaned))
        except ValueError:
            pass
    # Try base64
    try:
        decoded = base64.b64decode(value)
        if len(decoded) == 16:
            return list(decoded)
    except Exception:
        pass
    raise cv.Invalid(
        "Auth key must be a 32-character hex string or base64-encoded 16-byte key"
    )


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(LoraRemoteNode),
        cv.Required(CONF_SX126X_ID): cv.use_id(sx126x.SX126x),
        cv.Required(CONF_ADDRESS): cv.All(cv.hex_uint8_t, validate_address_range),
        cv.Required(CONF_AUTH_KEY): validate_auth_key,
        cv.Optional(CONF_TIME_ID): cv.use_id(time_component.RealTimeClock),
        cv.Optional(CONF_LISTEN_WINDOW): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_SENSORS, default=[]): cv.ensure_list(cv.use_id(sensor.Sensor)),
        cv.Optional(CONF_BINARY_SENSORS, default=[]): cv.ensure_list(
            cv.use_id(binary_sensor.BinarySensor)
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    radio = await cg.get_variable(config[CONF_SX126X_ID])
    cg.add(var.set_sx126x(radio))
    cg.add(radio.register_listener(var))
    cg.add(var.set_address(config[CONF_ADDRESS]))
    cg.add(var.set_auth_key(config[CONF_AUTH_KEY]))

    if CONF_TIME_ID in config:
        time_source = await cg.get_variable(config[CONF_TIME_ID])
        cg.add(var.set_time_source(time_source))

    if CONF_LISTEN_WINDOW in config:
        cg.add(var.set_listen_window(config[CONF_LISTEN_WINDOW]))

    for sensor_id in config[CONF_SENSORS]:
        sens = await cg.get_variable(sensor_id)
        cg.add(var.add_sensor(sens))

    for binary_sensor_id in config[CONF_BINARY_SENSORS]:
        sens = await cg.get_variable(binary_sensor_id)
        cg.add(var.add_binary_sensor(sens))
