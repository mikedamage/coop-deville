"""Text sensor platform for LoRa Gateway metrics."""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor
from esphome.const import CONF_ID

from . import lora_gateway_ns, LoraGateway, CONF_ID as GATEWAY_ID

DEPENDENCIES = ["lora_gateway"]

CONF_LORA_GATEWAY_ID = "lora_gateway_id"
CONF_TIMEOUT_LIST = "timeout_list"
CONF_LAST_HEARD_LIST = "last_heard_list"
CONF_SIGNAL_QUALITY_LIST = "signal_quality_list"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_LORA_GATEWAY_ID): cv.use_id(LoraGateway),
        cv.Optional(CONF_TIMEOUT_LIST): text_sensor.text_sensor_schema(),
        cv.Optional(CONF_LAST_HEARD_LIST): text_sensor.text_sensor_schema(),
        cv.Optional(CONF_SIGNAL_QUALITY_LIST): text_sensor.text_sensor_schema(),
    }
)


async def to_code(config):
    gateway = await cg.get_variable(config[CONF_LORA_GATEWAY_ID])

    if CONF_TIMEOUT_LIST in config:
        sens = await text_sensor.new_text_sensor(config[CONF_TIMEOUT_LIST])
        cg.add(gateway.set_timeout_list_sensor(sens))

    if CONF_LAST_HEARD_LIST in config:
        sens = await text_sensor.new_text_sensor(config[CONF_LAST_HEARD_LIST])
        cg.add(gateway.set_last_heard_list_sensor(sens))

    if CONF_SIGNAL_QUALITY_LIST in config:
        sens = await text_sensor.new_text_sensor(config[CONF_SIGNAL_QUALITY_LIST])
        cg.add(gateway.set_signal_quality_list_sensor(sens))
