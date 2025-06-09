import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import CONF_ID, CONF_URL
from esphome.core import CORE

DEPENDENCIES = ["wifi"]
AUTO_LOAD = ["json"]

CONF_SENSORS = "sensors"
CONF_SENSOR_ID = "sensor_id"
CONF_NAME = "name"
CONF_RECONNECT_INTERVAL = "reconnect_interval"
CONF_HEARTBEAT_INTERVAL = "heartbeat_interval"

websocket_client_ns = cg.esphome_ns.namespace("websocket_client")
WebSocketClientComponent = websocket_client_ns.class_(
    "WebSocketClientComponent", cg.Component
)

SENSOR_SCHEMA = cv.Schema({
    cv.Required(CONF_SENSOR_ID): cv.use_id(sensor.Sensor),
    cv.Required(CONF_NAME): cv.string,
})

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(WebSocketClientComponent),
    cv.Required(CONF_URL): cv.url,
    cv.Optional(CONF_SENSORS, default=[]): cv.ensure_list(SENSOR_SCHEMA),
    cv.Optional(CONF_RECONNECT_INTERVAL, default="30s"): cv.positive_time_period_milliseconds,
    cv.Optional(CONF_HEARTBEAT_INTERVAL, default="60s"): cv.positive_time_period_milliseconds,
}).extend(cv.COMPONENT_SCHEMA)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    
    cg.add(var.set_url(config[CONF_URL]))
    cg.add(var.set_reconnect_interval(config[CONF_RECONNECT_INTERVAL]))
    cg.add(var.set_heartbeat_interval(config[CONF_HEARTBEAT_INTERVAL]))
    
    for sensor_config in config[CONF_SENSORS]:
        sens = await cg.get_variable(sensor_config[CONF_SENSOR_ID])
        cg.add(var.add_sensor(sens, sensor_config[CONF_NAME]))
    
    # No external libraries needed - using ESPHome's built-in socket implementation