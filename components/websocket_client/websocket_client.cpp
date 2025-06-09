#include "websocket_client.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include "esphome/components/network/util.h"
#ifdef USE_TIME
#include "esphome/components/time/real_time_clock.h"
#endif
#include <mbedtls/base64.h>
#ifdef USE_ESP32
#include <WiFi.h>
#else
#include <ESP8266WiFi.h>
#endif

namespace esphome
{
  namespace websocket_client
  {

    // TAG is defined in header file

    void WebSocketClientComponent::setup()
    {
      ESP_LOGCONFIG(TAG, "Setting up WebSocket Client...");

      // Parse URL
      this->parse_url();

      // Initialize last sensor values
      this->last_sensor_values_.resize(this->sensors_.size(), NAN);

      // Set up sensor callbacks
      for (size_t i = 0; i < this->sensors_.size(); i++)
      {
        auto sensor = this->sensors_[i].sensor;
        auto name = this->sensors_[i].name;
        size_t index = i; // Capture index for lambda

        sensor->add_on_state_callback([this, sensor, name, index](float value)
                                      {
      ESP_LOGD(TAG, "Sensor %s callback triggered with value: %.2f", name.c_str(), value);
      if (!isnan(value) && (isnan(this->last_sensor_values_[index]) 
          || abs(value - this->last_sensor_values_[index]) > 0.01)) {
        this->last_sensor_values_[index] = value;
        this->send_sensor_data(sensor, name, value);
      } });
      }
    }

    void WebSocketClientComponent::loop()
    {
      uint32_t now = millis();

      // Check WiFi connection
      bool wifi_now_connected = network::is_connected();
      if (wifi_now_connected != this->wifi_connected_)
      {
        this->wifi_connected_ = wifi_now_connected;
        if (!wifi_now_connected && this->connected_)
        {
          ESP_LOGW(TAG, "WiFi disconnected, closing WebSocket");
          this->disconnect_websocket();
        }
      }

      // Handle WebSocket connection
      if (this->wifi_connected_)
      {
        if (!this->connected_)
        {
          if (now - this->last_reconnect_attempt_ > this->reconnect_interval_)
          {
            this->connect_websocket();
            this->last_reconnect_attempt_ = now;
          }
        }
        else
        {
          // Handle incoming data
          this->handle_websocket_data();

          // Send heartbeat
          if (now - this->last_heartbeat_ > this->heartbeat_interval_)
          {
            this->send_heartbeat();
            this->last_heartbeat_ = now;
          }
        }
      }
    }

    void WebSocketClientComponent::dump_config()
    {
      ESP_LOGCONFIG(TAG, "WebSocket Client:");
      ESP_LOGCONFIG(TAG, "  URL: %s", this->url_.c_str());
      ESP_LOGCONFIG(TAG, "  Host: %s", this->host_.c_str());
      ESP_LOGCONFIG(TAG, "  Port: %u", this->port_);
      ESP_LOGCONFIG(TAG, "  Path: %s", this->path_.c_str());
      ESP_LOGCONFIG(TAG, "  SSL: %s", this->use_ssl_ ? "yes" : "no");
      ESP_LOGCONFIG(TAG, "  Reconnect Interval: %u ms", this->reconnect_interval_);
      ESP_LOGCONFIG(TAG, "  Heartbeat Interval: %u ms", this->heartbeat_interval_);
      ESP_LOGCONFIG(TAG, "  Sensors:");
      for (const auto &config : this->sensors_)
      {
        ESP_LOGCONFIG(TAG, "    - %s (%s)", config.name.c_str(), config.sensor->get_name().c_str());
      }
    }

    void WebSocketClientComponent::add_sensor(sensor::Sensor *sensor, const std::string &name)
    {
      this->sensors_.push_back({sensor, name});
    }

#ifdef USE_TIME
    void WebSocketClientComponent::set_time_component(time::RealTimeClock *time_component)
    {
      this->time_component_ = time_component;
    }
#endif

    uint64_t WebSocketClientComponent::get_timestamp()
    {
#ifdef USE_TIME
      if (this->time_component_->now().is_valid())
      {
        // Return Unix timestamp
        return this->time_component_->timestamp_now();
      }

      // Gotta return something if time is not synced
      ESP_LOGW(TAG, "Time component not synced, using millis() as fallback");
      return millis();
#else
      ESP_LOGW(TAG, "Time component not available, using millis()");
      return millis();
#endif
    }

    void WebSocketClientComponent::parse_url()
    {
      std::string url = this->url_;

      // Determine protocol
      if (url.substr(0, 6) == "wss://")
      {
        this->use_ssl_ = true;
        url = url.substr(6);
        this->port_ = 443;
      }
      else if (url.substr(0, 5) == "ws://")
      {
        this->use_ssl_ = false;
        url = url.substr(5);
        this->port_ = 80;
      }
      else
      {
        ESP_LOGE(TAG, "Invalid WebSocket URL: %s", this->url_.c_str());
        return;
      }

      // Find host and path
      size_t slash_pos = url.find('/');
      if (slash_pos != std::string::npos)
      {
        this->host_ = url.substr(0, slash_pos);
        this->path_ = url.substr(slash_pos);
      }
      else
      {
        this->host_ = url;
        this->path_ = "/";
      }

      // Extract port if specified
      size_t port_pos = this->host_.find(':');
      if (port_pos != std::string::npos)
      {
        this->port_ = std::stoi(this->host_.substr(port_pos + 1));
        this->host_ = this->host_.substr(0, port_pos);
      }
    }

    void WebSocketClientComponent::connect_websocket()
    {
      if (this->connected_)
      {
        return;
      }

      ESP_LOGI(TAG, "Connecting to WebSocket: %s:%u%s", this->host_.c_str(), this->port_, this->path_.c_str());

      // Create socket
      this->socket_ = socket::socket_ip(SOCK_STREAM, 0);
      if (!this->socket_)
      {
        ESP_LOGW(TAG, "Failed to create socket");
        return;
      }

      // Set socket to non-blocking for timeout control
      this->socket_->setblocking(false);

      // Connect to server
      struct sockaddr_in server_addr;
      memset(&server_addr, 0, sizeof(server_addr));
      server_addr.sin_family = AF_INET;
      server_addr.sin_port = htons(this->port_);

      // Resolve hostname using ESP32/ESP8266 WiFi library
      IPAddress server_ip;
      if (!WiFi.hostByName(this->host_.c_str(), server_ip))
      {
        ESP_LOGW(TAG, "Failed to resolve hostname: %s", this->host_.c_str());
        this->socket_.reset();
        return;
      }

      server_addr.sin_addr.s_addr = server_ip;

      int result = this->socket_->connect((struct sockaddr *)&server_addr, sizeof(server_addr));
      if (result != 0 && errno != EINPROGRESS)
      {
        ESP_LOGW(TAG, "Failed to connect to server: %d", errno);
        this->socket_.reset();
        return;
      }

      // Wait for connection to complete (simplified - you might want to add proper timeout handling)
      delay(100);

      // Send WebSocket handshake
      std::string handshake = this->create_websocket_handshake();
      if (this->socket_->write(handshake.c_str(), handshake.length()) != handshake.length())
      {
        ESP_LOGW(TAG, "Failed to send handshake");
        this->socket_.reset();
        return;
      }

      ESP_LOGI(TAG, "WebSocket handshake sent, waiting for response...");
      this->connected_ = true;
      this->handshake_complete_ = false;
      this->last_heartbeat_ = millis();
    }

    void WebSocketClientComponent::disconnect_websocket()
    {
      if (this->socket_)
      {
        this->socket_.reset();
      }
      this->connected_ = false;
      this->handshake_complete_ = false;
      ESP_LOGI(TAG, "WebSocket disconnected");
    }

    std::string WebSocketClientComponent::create_websocket_handshake()
    {
      std::string key = this->generate_websocket_key();

      std::string handshake = "GET " + this->path_ + " HTTP/1.1\r\n";
      handshake += "Host: " + this->host_ + "\r\n";
      handshake += "Upgrade: websocket\r\n";
      handshake += "Connection: Upgrade\r\n";
      handshake += "Sec-WebSocket-Key: " + key + "\r\n";
      handshake += "Sec-WebSocket-Version: 13\r\n";
      handshake += "\r\n";

      ESP_LOGD(TAG, "WebSocket handshake:\n%s", handshake.c_str());
      return handshake;
    }

    std::string WebSocketClientComponent::generate_websocket_key()
    {
      uint8_t key[16];
      for (int i = 0; i < 16; i++)
      {
        key[i] = random(0, 256);
      }

      size_t output_len;
      mbedtls_base64_encode(nullptr, 0, &output_len, key, 16);

      std::string encoded_key(output_len - 1, 0);
      mbedtls_base64_encode((unsigned char *)encoded_key.data(), output_len, &output_len, key, 16);

      return encoded_key;
    }

    void WebSocketClientComponent::handle_websocket_data()
    {
      if (!this->socket_)
      {
        return;
      }

      uint8_t buffer[1024];
      int bytes_read = this->socket_->read(buffer, sizeof(buffer));

      if (bytes_read > 0)
      {
        std::string data(reinterpret_cast<char *>(buffer), bytes_read);
        ESP_LOGD(TAG, "Received data: %s", data.c_str());

        if (!this->handshake_complete_)
        {
          // Check for successful handshake response
          if (data.find("HTTP/1.1 101") != std::string::npos &&
              data.find("Upgrade: websocket") != std::string::npos)
          {
            ESP_LOGI(TAG, "WebSocket handshake successful");
            this->handshake_complete_ = true;

            // Send connection message using ArduinoJson
            DynamicJsonDocument doc(256);
            doc["type"] = "connection";
            doc["device_id"] = App.get_name();
            doc["timestamp"] = this->get_timestamp();

            std::string message;
            serializeJson(doc, message);
            this->send_websocket_frame(message);
          }
          else
          {
            ESP_LOGW(TAG, "WebSocket handshake failed: %s", data.c_str());
            this->disconnect_websocket();
          }
        }
        else
        {
          // Handle WebSocket frames here if needed
          ESP_LOGD(TAG, "Received WebSocket frame data");
        }
      }
      else if (bytes_read < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
      {
        ESP_LOGW(TAG, "WebSocket connection lost: %d", errno);
        this->disconnect_websocket();
      }
    }

    bool WebSocketClientComponent::send_websocket_frame(const std::string &data)
    {
      if (!this->socket_ || !this->handshake_complete_)
      {
        ESP_LOGW(TAG, "Cannot send frame - socket not ready or handshake not complete");
        return false;
      }

      ESP_LOGD(TAG, "Sending WebSocket frame: %s", data.c_str());

      // Create WebSocket frame with masking (required for client)
      std::vector<uint8_t> frame;
      frame.push_back(0x81); // FIN=1, opcode=1 (text)

      // Generate masking key
      uint8_t mask[4];
      for (int i = 0; i < 4; i++)
      {
        mask[i] = random(0, 256);
      }

      if (data.length() < 126)
      {
        frame.push_back(0x80 | data.length()); // MASK=1, length
      }
      else if (data.length() < 65536)
      {
        frame.push_back(0x80 | 126); // MASK=1, extended length
        frame.push_back((data.length() >> 8) & 0xFF);
        frame.push_back(data.length() & 0xFF);
      }
      else
      {
        ESP_LOGW(TAG, "Message too large for WebSocket frame");
        return false;
      }

      // Add masking key
      frame.insert(frame.end(), mask, mask + 4);

      // Add masked payload
      for (size_t i = 0; i < data.length(); i++)
      {
        frame.push_back(data[i] ^ mask[i % 4]);
      }

      int result = this->socket_->write(frame.data(), frame.size());
      bool success = result == frame.size();

      if (!success)
      {
        ESP_LOGW(TAG, "Failed to send WebSocket frame: wrote %d of %d bytes", result, frame.size());
      }

      return success;
    }

    void WebSocketClientComponent::send_sensor_data(sensor::Sensor *sensor, const std::string &name, float value)
    {
      if (!this->handshake_complete_)
      {
        ESP_LOGD(TAG, "Handshake not complete, queuing sensor data");
        return;
      }

      DynamicJsonDocument doc(384);
      doc["type"] = "sensor_data";
      doc["device_id"] = App.get_name();
      doc["sensor_name"] = name;
      doc["sensor_id"] = sensor->get_object_id();
      doc["value"] = value;
      doc["unit"] = sensor->get_unit_of_measurement();
      doc["timestamp"] = this->get_timestamp();

      std::string message;
      serializeJson(doc, message);

      ESP_LOGI(TAG, "Sending sensor data: %s", message.c_str());
      bool sent = this->send_websocket_frame(message);

      if (!sent)
      {
        ESP_LOGW(TAG, "Failed to send sensor data, disconnecting");
        this->disconnect_websocket();
      }
    }

    void WebSocketClientComponent::send_heartbeat()
    {
      if (!this->handshake_complete_)
      {
        return;
      }

      DynamicJsonDocument doc(256);
      doc["type"] = "heartbeat";
      doc["device_id"] = App.get_name();
      doc["timestamp"] = this->get_timestamp();
      doc["uptime"] = millis();
      doc["free_heap"] = ESP.getFreeHeap();

      std::string message;
      serializeJson(doc, message);

      ESP_LOGD(TAG, "Sending heartbeat: %s", message.c_str());
      bool sent = this->send_websocket_frame(message);

      if (!sent)
      {
        ESP_LOGW(TAG, "Failed to send heartbeat, disconnecting");
        this->disconnect_websocket();
      }
    }

  }
}