# ESPHome Websocket Client

Send sensor data via websocket connection

Data message:

```json
{
    "type": "sensor_data",
    "device_id": "distance",
    "sensor_name": "distance",
    "sensor_id": "ultrasonic_sensor",
    "value": 1.963504,
    "unit": "m",
    "timestamp": 2993154
}
```

Heartbeat message:

```json
{
    "type":"heartbeat",
    "device_id":"distance",
    "timestamp": 2910518,
    "uptime": 2910518,
    "free_heap":216800
}
```

# ESPHome

## MicroController: Initial setup

https://web.esphome.io/

## The Device Builder

To manage the fleet of MicroControllers the [Device Builder](https://esphome.io/guides/getting_started_command_line#bonus-esphome-device-builder) is
very useful:

```bash
docker run --rm -p 6052:6052 -e ESPHOME_DASHBOARD_USE_PING=true -v "${PWD}":/config -it ghcr.io/esphome/esphome
```
