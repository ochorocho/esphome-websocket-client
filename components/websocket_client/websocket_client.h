#pragma once

#include "esphome/core/component.h"
#include "esphome/components/sensor/sensor.h"
#ifdef USE_LIGHT
#include "esphome/components/light/light_state.h"
#endif
#ifdef USE_BUTTON
#include "esphome/components/button/button.h"
#endif
#ifdef USE_TIME
#include "esphome/components/time/real_time_clock.h"
#endif

#include <vector>
#include <string>
#include <queue>
#ifdef USE_ESP32
#include <lwip/sockets.h>
#else
#include <ESP8266WiFi.h>
#endif

namespace esphome
{
  namespace websocket_client
  {

    static const char *const TAG = "websocket_client";

    // Simple JSON builder class to avoid ArduinoJson dependency
    class SimpleJsonBuilder
    {
    public:
      SimpleJsonBuilder() : first_(true) { json_ = "{"; }
      
      void add_string(const std::string &key, const std::string &value) {
        if (!first_) json_ += ",";
        json_ += "\"" + key + "\":\"" + escape_string(value) + "\"";
        first_ = false;
      }
      
      void add_number(const std::string &key, double value) {
        if (!first_) json_ += ",";
        json_ += "\"" + key + "\":" + std::to_string(value);
        first_ = false;
      }
      
      void add_bool(const std::string &key, bool value) {
        if (!first_) json_ += ",";
        json_ += "\"" + key + "\":" + (value ? "true" : "false");
        first_ = false;
      }
      
      void add_uint64(const std::string &key, uint64_t value) {
        if (!first_) json_ += ",";
        json_ += "\"" + key + "\":" + std::to_string(value);
        first_ = false;
      }
      
      void add_uint32(const std::string &key, uint32_t value) {
        if (!first_) json_ += ",";
        json_ += "\"" + key + "\":" + std::to_string(value);
        first_ = false;
      }
      
      std::string build() {
        return json_ + "}";
      }
      
    private:
      std::string json_;
      bool first_;
      
      std::string escape_string(const std::string &str) {
        std::string result;
        for (char c : str) {
          switch (c) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default: result += c; break;
          }
        }
        return result;
      }
    };

    // Simple JSON parser for incoming messages
    class SimpleJsonParser
    {
    public:
      SimpleJsonParser(const std::string &json) : json_(json) {}
      
      std::string get_string(const std::string &key) const {
        size_t key_pos = json_.find("\"" + key + "\":");
        if (key_pos == std::string::npos) return "";
        
        size_t value_start = json_.find("\"", key_pos + key.length() + 3);
        if (value_start == std::string::npos) return "";
        value_start++;
        
        size_t value_end = json_.find("\"", value_start);
        if (value_end == std::string::npos) return "";
        
        return json_.substr(value_start, value_end - value_start);
      }
      
      bool get_bool(const std::string &key) const {
        size_t key_pos = json_.find("\"" + key + "\":");
        if (key_pos == std::string::npos) return false;
        
        size_t value_start = key_pos + key.length() + 3;
        while (value_start < json_.length() && (json_[value_start] == ' ' || json_[value_start] == ':')) {
          value_start++;
        }
        
        return json_.substr(value_start, 4) == "true";
      }
      
      double get_number(const std::string &key) const {
        size_t key_pos = json_.find("\"" + key + "\":");
        if (key_pos == std::string::npos) return 0.0;
        
        size_t value_start = key_pos + key.length() + 3;
        while (value_start < json_.length() && (json_[value_start] == ' ' || json_[value_start] == ':')) {
          value_start++;
        }
        
        size_t value_end = value_start;
        while (value_end < json_.length() && 
               (isdigit(json_[value_end]) || json_[value_end] == '.' || json_[value_end] == '-')) {
          value_end++;
        }
        
        if (value_end > value_start) {
          return std::stod(json_.substr(value_start, value_end - value_start));
        }
        
        return 0.0;
      }
      
    private:
      std::string json_;
    };

    struct SensorConfig
    {
      sensor::Sensor *sensor;
      std::string name;
    };

#ifdef USE_LIGHT
    struct LightConfig
    {
      light::LightState *light;
      std::string name;
    };
#endif

#ifdef USE_BUTTON
    struct ButtonConfig
    {
      button::Button *button;
      std::string name;
    };
#endif

    struct QueuedSensorData
    {
      sensor::Sensor *sensor;
      std::string name;
      float value;
      uint64_t timestamp;
    };

#ifdef USE_LIGHT
    struct QueuedLightData
    {
      light::LightState *light;
      std::string name;
      bool state;
      float brightness;
      float red;
      float green;
      float blue;
      float white;
      uint64_t timestamp;
    };
#endif

#ifdef USE_BUTTON
    struct QueuedButtonData
    {
      button::Button *button;
      std::string name;
      uint64_t timestamp;
    };
#endif

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
      void set_send_all_updates(bool send_all) { this->send_all_updates_ = send_all; }
      void set_min_change_threshold(float threshold) { this->min_change_threshold_ = threshold; }

      void add_sensor(sensor::Sensor *sensor, const std::string &name);
#ifdef USE_LIGHT
      void add_light(light::LightState *light, const std::string &name);
#endif
#ifdef USE_BUTTON
      void add_button(button::Button *button, const std::string &name);
#endif
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
      bool send_all_updates_{false};  // Send every update regardless of change
      float min_change_threshold_{0.01}; // Configurable threshold

      std::vector<SensorConfig> sensors_;
#ifdef USE_LIGHT
      std::vector<LightConfig> lights_;
#endif
#ifdef USE_BUTTON
      std::vector<ButtonConfig> buttons_;
#endif
      std::vector<float> last_sensor_values_;
      std::queue<QueuedSensorData> queued_data_;
#ifdef USE_LIGHT
      std::queue<QueuedLightData> queued_light_data_;
#endif
#ifdef USE_BUTTON
      std::queue<QueuedButtonData> queued_button_data_;
#endif  // Queue for offline data

      int socket_fd_{-1};
      bool connected_{false};
      bool handshake_complete_{false};
      bool wifi_connected_{false};
      uint32_t last_reconnect_attempt_{0};
      uint32_t last_heartbeat_{0};
      std::string frame_buffer_;
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
      void queue_sensor_data(sensor::Sensor *sensor, const std::string &name, float value);
#ifdef USE_LIGHT
      void send_light_data(light::LightState *light, const std::string &name);
      void queue_light_data(light::LightState *light, const std::string &name);
#endif
#ifdef USE_BUTTON
      void send_button_data(button::Button *button, const std::string &name);
      void queue_button_data(button::Button *button, const std::string &name);
#endif
      void process_queued_data();
#ifdef USE_LIGHT
      void process_queued_light_data();
#endif
#ifdef USE_BUTTON
      void process_queued_button_data();
#endif
      void send_heartbeat();
      uint64_t get_timestamp();
      void process_incoming_message(const std::string &message);
      void handle_light_control(const SimpleJsonParser &parser);
#ifdef USE_BUTTON
      void handle_button_trigger(const SimpleJsonParser &parser);
#endif
      std::string parse_websocket_frame(const std::string &data);
    };

  }
}