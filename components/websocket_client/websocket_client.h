#pragma once

#include "esphome/core/component.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/socket/socket.h"
#ifdef USE_TIME
#include "esphome/components/time/real_time_clock.h"
#endif

#include <ArduinoJson.h>
#include <vector>
#include <string>

namespace esphome
{
  namespace websocket_client
  {

    static const char *const TAG = "websocket_client";

    struct SensorConfig
    {
      sensor::Sensor *sensor;
      std::string name;
    };

    class WebSocketClientComponent : public Component
    {
    public:
      void setup() override;
      void loop() override;
      void dump_config() override;
      float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

      void set_url(const std::string &url) { this->url_ = url; }
      void set_reconnect_interval(uint32_t interval) { this->reconnect_interval_ = interval; }
      void set_heartbeat_interval(uint32_t interval) { this->heartbeat_interval_ = interval; }

      void add_sensor(sensor::Sensor *sensor, const std::string &name);
#ifdef USE_TIME
      void set_time_component(time::RealTimeClock *time_component);
#endif

    protected:
      std::string url_;
      std::string host_;
      std::string path_;
      uint16_t port_;
      bool use_ssl_{false};

      uint32_t reconnect_interval_{5000};
      uint32_t heartbeat_interval_{30000};

      std::vector<SensorConfig> sensors_;
      std::vector<float> last_sensor_values_;

      std::unique_ptr<socket::Socket> socket_;
      bool connected_{false};
      bool handshake_complete_{false};
      bool wifi_connected_{false};
      uint32_t last_reconnect_attempt_{0};
      uint32_t last_heartbeat_{0};
#ifdef USE_TIME
      time::RealTimeClock *time_component_{nullptr};
#endif
      void parse_url();
      void connect_websocket();
      void disconnect_websocket();
      std::string create_websocket_handshake();
      std::string generate_websocket_key();
      void handle_websocket_data();
      bool send_websocket_frame(const std::string &data);
      void send_sensor_data(sensor::Sensor *sensor, const std::string &name, float value);
      void send_heartbeat();
      uint64_t get_timestamp();
    };

  }
}