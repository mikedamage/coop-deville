import base64

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sx126x, time as time_component
from esphome.const import CONF_ID, CONF_NAME, CONF_TIME_ID, CONF_ADDRESS

CODEOWNERS = ["@mikedamage"]
DEPENDENCIES = ["sx126x"]
AUTO_LOAD = ["sensor", "binary_sensor", "text_sensor", "time"]

CONF_SX126X_ID = "sx126x_id"
CONF_AUTH_KEY = "auth_key"
CONF_REMOTE_NODES = "remote_nodes"
CONF_RESPONSE_TIMEOUT = "response_timeout"
CONF_POLL_INTERVAL = "poll_interval"
CONF_TIME_SYNC_INTERVAL = "time_sync_interval"
CONF_SEND_ACK = "send_ack"
CONF_STALE_SENSOR_BEHAVIOR = "stale_sensor_behavior"

lora_gateway_ns = cg.esphome_ns.namespace("lora_gateway")
LoraGateway = lora_gateway_ns.class_("LoraGateway", cg.Component, sx126x.SX126xListener)
RemoteNode = lora_gateway_ns.class_("RemoteNode")

StaleSensorBehavior = lora_gateway_ns.enum("StaleSensorBehavior", is_class=True)
STALE_SENSOR_BEHAVIOR_OPTIONS = {
    "keep": StaleSensorBehavior.KEEP_LAST_VALUE,
    "invalidate": StaleSensorBehavior.INVALIDATE,
}

REMOTE_NODE_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(RemoteNode),
        cv.Required(CONF_ADDRESS): cv.hex_uint8_t,
        cv.Required(CONF_NAME): cv.string,
    }
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


def validate_unique_addresses(config):
    """Validate that all remote node addresses are unique."""
    addresses = [node[CONF_ADDRESS] for node in config[CONF_REMOTE_NODES]]
    if len(addresses) != len(set(addresses)):
        raise cv.Invalid("Remote node addresses must be unique")
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(LoraGateway),
            cv.Required(CONF_SX126X_ID): cv.use_id(sx126x.SX126x),
            cv.Required(CONF_ADDRESS): cv.All(cv.hex_uint8_t, validate_address_range),
            cv.Required(CONF_AUTH_KEY): validate_auth_key,
            cv.Required(CONF_REMOTE_NODES): cv.ensure_list(REMOTE_NODE_SCHEMA),
            cv.Required(CONF_RESPONSE_TIMEOUT): cv.positive_time_period_milliseconds,
            cv.Required(CONF_POLL_INTERVAL): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_TIME_ID): cv.use_id(time_component.RealTimeClock),
            cv.Optional(CONF_TIME_SYNC_INTERVAL): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_SEND_ACK, default=False): cv.boolean,
            cv.Optional(CONF_STALE_SENSOR_BEHAVIOR, default="keep"): cv.enum(
                STALE_SENSOR_BEHAVIOR_OPTIONS, lower=True
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    validate_unique_addresses,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    radio = await cg.get_variable(config[CONF_SX126X_ID])
    cg.add(var.set_sx126x(radio))
    cg.add(radio.register_listener(var))
    cg.add(var.set_address(config[CONF_ADDRESS]))
    cg.add(var.set_auth_key(config[CONF_AUTH_KEY]))
    cg.add(var.set_response_timeout(config[CONF_RESPONSE_TIMEOUT]))
    cg.add(var.set_poll_interval(config[CONF_POLL_INTERVAL]))
    cg.add(var.set_send_ack(config[CONF_SEND_ACK]))
    cg.add(var.set_stale_sensor_behavior(config[CONF_STALE_SENSOR_BEHAVIOR]))

    if CONF_TIME_ID in config:
        time_source = await cg.get_variable(config[CONF_TIME_ID])
        cg.add(var.set_time_source(time_source))

    if CONF_TIME_SYNC_INTERVAL in config:
        cg.add(var.set_time_sync_interval(config[CONF_TIME_SYNC_INTERVAL]))

    for node_config in config[CONF_REMOTE_NODES]:
        node_var = cg.new_Pvariable(node_config[CONF_ID])
        cg.add(node_var.set_address(node_config[CONF_ADDRESS]))
        cg.add(node_var.set_name(node_config[CONF_NAME]))
        cg.add(var.add_remote_node(node_var))
