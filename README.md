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

Install [esphome cli](https://esphome.io/guides/installing_esphome):

```bash
pip3 install wheel
pip3 install esphome

esphome version # should return something like "Version: 2021.12.3"
```

Download the repository:

```bash
git clone https://github.com/ochorocho/esphome-websocket-client.git
cd esphome-websocket-client
```

Create a [secrets.yaml](secrets.yaml.example)

```bash
esphome run led-temp.yaml
```

https://web.esphome.io/

## The Device Builder

To manage the fleet of MicroControllers the [Device Builder](https://esphome.io/guides/getting_started_command_line#bonus-esphome-device-builder) is
very useful.


To run the Device Builder locally the easiest way is to use Docker:

```bash
docker run --rm -p 6052:6052 -e ESPHOME_DASHBOARD_USE_PING=true -v "${PWD}":/config -it ghcr.io/esphome/esphome
```
