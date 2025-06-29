#include "websocket_client.h"
#include "esphome/core/log.h"

#include "esphome/core/application.h"
#include "esphome/components/network/util.h"
#include "esphome/core/helpers.h"
#ifdef USE_TIME
#include "esphome/components/time/real_time_clock.h"
#endif
#ifdef USE_ESP32
#include <esp_system.h>
#include <esp_netif.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
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

      // Set up sensor callbacks with improved logic
      for (size_t i = 0; i < this->sensors_.size(); i++)
      {
        auto sensor = this->sensors_[i].sensor;
        auto name = this->sensors_[i].name;
        size_t index = i; // Capture index for lambda

        sensor->add_on_state_callback([this, sensor, name, index](float value)
                                      {
      ESP_LOGD(TAG, "Sensor %s callback triggered with value: %.2f", name.c_str(), value);
      
      // Skip invalid values
      if (isnan(value)) {
        ESP_LOGD(TAG, "Skipping NaN value for sensor %s", name.c_str());
        return;
      }
      
      bool should_send = false;
      
      if (this->send_all_updates_) {
        // Send every valid update
        should_send = true;
        ESP_LOGD(TAG, "Sending all updates mode - sending value");
      } else {
        // Check if value changed significantly
        float last_value = this->last_sensor_values_[index];
        if (isnan(last_value)) {
          // First reading
          should_send = true;
          ESP_LOGD(TAG, "First reading for sensor %s", name.c_str());
        } else if (abs(value - last_value) >= this->min_change_threshold_) {
          // Value changed enough
          should_send = true;
          ESP_LOGD(TAG, "Value changed from %.2f to %.2f (threshold: %.2f)", 
                   last_value, value, this->min_change_threshold_);
        }
      }
      
      if (should_send) {
        this->last_sensor_values_[index] = value;
        
        if (this->handshake_complete_) {
          // Send immediately if connected
          this->send_sensor_data(sensor, name, value);
        } else {
          // Queue for later if not connected
          ESP_LOGD(TAG, "Not connected, queuing sensor data");
          this->queue_sensor_data(sensor, name, value);
        }
      } });
      }

#ifdef USE_LIGHT
      // Set up light callbacks
      for (size_t i = 0; i < this->lights_.size(); i++)
      {
        auto light = this->lights_[i].light;
        auto name = this->lights_[i].name;

        light->add_new_remote_values_callback([this, light, name]()
                                               {
        ESP_LOGD(TAG, "Light %s state changed", name.c_str());
        
        if (this->handshake_complete_) {
          // Send immediately if connected
          this->send_light_data(light, name);
        } else {
          // Queue for later if not connected
          ESP_LOGD(TAG, "Not connected, queuing light data");
          this->queue_light_data(light, name);
        }
      });
      }
#endif

#ifdef USE_BUTTON
      // Set up button callbacks
      for (size_t i = 0; i < this->buttons_.size(); i++)
      {
        auto button = this->buttons_[i].button;
        auto name = this->buttons_[i].name;

        button->add_on_press_callback([this, button, name]()
                                       {
        ESP_LOGD(TAG, "Button %s pressed", name.c_str());
        
        if (this->handshake_complete_) {
          // Send immediately if connected
          this->send_button_data(button, name);
        } else {
          // Queue for later if not connected
          ESP_LOGD(TAG, "Not connected, queuing button data");
          this->queue_button_data(button, name);
        }
      });
      }
#endif
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

          // Process queued sensor data when connection is established
          if (this->handshake_complete_)
          {
            this->process_queued_data();
#ifdef USE_LIGHT
            this->process_queued_light_data();
#endif
#ifdef USE_BUTTON
            this->process_queued_button_data();
#endif
          }

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
#ifdef USE_LIGHT
      ESP_LOGCONFIG(TAG, "  Lights:");
      for (const auto &config : this->lights_)
      {
        ESP_LOGCONFIG(TAG, "    - %s (%s)", config.name.c_str(), config.light->get_name().c_str());
      }
#endif
#ifdef USE_BUTTON
      ESP_LOGCONFIG(TAG, "  Buttons:");
      for (const auto &config : this->buttons_)
      {
        ESP_LOGCONFIG(TAG, "    - %s (%s)", config.name.c_str(), config.button->get_name().c_str());
      }
#endif
    }

    void WebSocketClientComponent::add_sensor(sensor::Sensor *sensor, const std::string &name)
    {
      this->sensors_.push_back({sensor, name});
    }

#ifdef USE_LIGHT
    void WebSocketClientComponent::add_light(light::LightState *light, const std::string &name)
    {
      this->lights_.push_back({light, name});
    }
#endif

#ifdef USE_BUTTON
    void WebSocketClientComponent::add_button(button::Button *button, const std::string &name)
    {
      this->buttons_.push_back({button, name});
    }
#endif

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
      this->socket_fd_ = socket(AF_INET, SOCK_STREAM, 0);
      if (this->socket_fd_ < 0)
      {
        ESP_LOGW(TAG, "Failed to create socket");
        return;
      }

      // Set socket to non-blocking for timeout control
      int flags = fcntl(this->socket_fd_, F_GETFL, 0);
      fcntl(this->socket_fd_, F_SETFL, flags | O_NONBLOCK);

      // Connect to server
      struct sockaddr_in server_addr;
      memset(&server_addr, 0, sizeof(server_addr));
      server_addr.sin_family = AF_INET;
      server_addr.sin_port = htons(this->port_);

      // Resolve hostname using ESP-IDF functions
#ifdef USE_ESP32
      struct hostent *he = gethostbyname(this->host_.c_str());
      if (he == nullptr)
      {
        ESP_LOGW(TAG, "Failed to resolve hostname: %s", this->host_.c_str());
        close(this->socket_fd_);
        this->socket_fd_ = -1;
        return;
      }
      server_addr.sin_addr.s_addr = *((uint32_t*)he->h_addr);
#else
      IPAddress server_ip;
      if (!WiFi.hostByName(this->host_.c_str(), server_ip))
      {
        ESP_LOGW(TAG, "Failed to resolve hostname: %s", this->host_.c_str());
        close(this->socket_fd_);
        this->socket_fd_ = -1;
        return;
      }
      server_addr.sin_addr.s_addr = server_ip;
#endif

      int result = connect(this->socket_fd_, (struct sockaddr *)&server_addr, sizeof(server_addr));
      if (result != 0 && errno != EINPROGRESS)
      {
        ESP_LOGW(TAG, "Failed to connect to server: %d", errno);
        close(this->socket_fd_);
        this->socket_fd_ = -1;
        return;
      }

      // Wait for connection to complete (simplified - you might want to add proper timeout handling)
#ifdef USE_ESP32
      vTaskDelay(pdMS_TO_TICKS(100));
#else
      delay(100);
#endif

      // Send WebSocket handshake
      std::string handshake = this->create_websocket_handshake();
      if (write(this->socket_fd_, handshake.c_str(), handshake.length()) != handshake.length())
      {
        ESP_LOGW(TAG, "Failed to send handshake");
        close(this->socket_fd_);
        this->socket_fd_ = -1;
        return;
      }

      ESP_LOGI(TAG, "WebSocket handshake sent, waiting for response...");
      this->connected_ = true;
      this->handshake_complete_ = false;
      this->last_heartbeat_ = millis();
    }

    void WebSocketClientComponent::disconnect_websocket()
    {
      if (this->socket_fd_ >= 0)
      {
        close(this->socket_fd_);
        this->socket_fd_ = -1;
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
#ifdef USE_ESP32
        key[i] = esp_random() & 0xFF;
#else
        key[i] = random(0, 256);
#endif
      }

      return base64_encode(key, 16);
    }

    void WebSocketClientComponent::handle_websocket_data()
    {
      if (this->socket_fd_ < 0)
      {
        return;
      }

      uint8_t buffer[1024];
      int bytes_read = read(this->socket_fd_, buffer, sizeof(buffer));

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

            // Send connection message using native JSON builder
            SimpleJsonBuilder json;
            json.add_string("type", "connection");
            json.add_string("device_id", App.get_name());
            json.add_uint64("timestamp", this->get_timestamp());
            std::string message = json.build();
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
          // Handle incoming WebSocket frames
          this->frame_buffer_ += data;
          std::string message;
          while (!(message = this->parse_websocket_frame(this->frame_buffer_)).empty()) {
            ESP_LOGD(TAG, "Received WebSocket message: %s", message.c_str());
            this->process_incoming_message(message);
          }
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
      if (this->socket_fd_ < 0 || !this->handshake_complete_)
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
#ifdef USE_ESP32
        mask[i] = esp_random() & 0xFF;
#else
        mask[i] = random(0, 256);
#endif
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

      int result = write(this->socket_fd_, frame.data(), frame.size());
      bool success = result == (int)frame.size();

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
        this->queue_sensor_data(sensor, name, value);
        return;
      }

      SimpleJsonBuilder json;
      json.add_string("type", "sensor_data");
      json.add_string("device_id", App.get_name());
      json.add_string("sensor_name", name);
      json.add_string("sensor_id", sensor->get_object_id());
      json.add_number("value", value);
      json.add_string("unit", sensor->get_unit_of_measurement());
      json.add_uint64("timestamp", this->get_timestamp());
      json.add_bool("queued", false); // Mark as real-time data
      std::string message = json.build();

      ESP_LOGI(TAG, "Sending sensor data: %s", message.c_str());
      bool sent = this->send_websocket_frame(message);

      if (!sent)
      {
        ESP_LOGW(TAG, "Failed to send sensor data, queuing for retry");
        this->queue_sensor_data(sensor, name, value);
        this->disconnect_websocket();
      }
    }

    void WebSocketClientComponent::send_heartbeat()
    {
      if (!this->handshake_complete_)
      {
        return;
      }

      SimpleJsonBuilder json;
      json.add_string("type", "heartbeat");
      json.add_string("device_id", App.get_name());
      json.add_uint64("timestamp", this->get_timestamp());
      json.add_uint32("uptime", millis());
#ifdef USE_ESP32
      json.add_uint32("free_heap", esp_get_free_heap_size());
#else
      json.add_uint32("free_heap", ESP.getFreeHeap());
#endif
      std::string message = json.build();

      ESP_LOGD(TAG, "Sending heartbeat: %s", message.c_str());
      bool sent = this->send_websocket_frame(message);

      if (!sent)
      {
        ESP_LOGW(TAG, "Failed to send heartbeat, disconnecting");
        this->disconnect_websocket();
      }
    }

    void WebSocketClientComponent::queue_sensor_data(sensor::Sensor *sensor, const std::string &name, float value)
    {
      // Limit queue size to prevent memory issues
      if (this->queued_data_.size() >= 50)
      {
        ESP_LOGW(TAG, "Queue full, dropping oldest sensor data");
        this->queued_data_.pop();
      }

      QueuedSensorData data;
      data.sensor = sensor;
      data.name = name;
      data.value = value;
      data.timestamp = this->get_timestamp();

      this->queued_data_.push(data);
      ESP_LOGD(TAG, "Queued sensor data: %s = %.2f (queue size: %d)",
               name.c_str(), value, this->queued_data_.size());
    }

    void WebSocketClientComponent::process_queued_data()
    {
      if (this->queued_data_.empty())
      {
        return;
      }

      ESP_LOGI(TAG, "Processing %d queued sensor readings", this->queued_data_.size());

      // Process up to 5 queued items per loop to avoid blocking
      int processed = 0;
      while (!this->queued_data_.empty() && processed < 5)
      {
        QueuedSensorData data = this->queued_data_.front();
        this->queued_data_.pop();

        // Send the queued data with original timestamp
        SimpleJsonBuilder json;
        json.add_string("type", "sensor_data");
        json.add_string("device_id", App.get_name());
        json.add_string("sensor_name", data.name);
        json.add_string("sensor_id", data.sensor->get_object_id());
        json.add_number("value", data.value);
        json.add_string("unit", data.sensor->get_unit_of_measurement());
        json.add_uint64("timestamp", data.timestamp);
        json.add_bool("queued", true); // Mark as queued data
        std::string message = json.build();

        ESP_LOGD(TAG, "Sending queued sensor data: %s", message.c_str());
        bool sent = this->send_websocket_frame(message);

        if (!sent)
        {
          ESP_LOGW(TAG, "Failed to send queued data, will retry later");
          // Put it back at the front of the queue
          this->queued_data_ = std::queue<QueuedSensorData>(); // Clear queue to avoid issues
          break;
        }

        processed++;
      }

      if (processed > 0)
      {
        ESP_LOGI(TAG, "Processed %d queued sensor readings, %d remaining",
                 processed, this->queued_data_.size());
      }
    }

#ifdef USE_LIGHT
    void WebSocketClientComponent::send_light_data(light::LightState *light, const std::string &name)
    {
      if (!this->handshake_complete_)
      {
        ESP_LOGD(TAG, "Handshake not complete, queuing light data");
        this->queue_light_data(light, name);
        return;
      }

      // Get current light values
      auto current_values = light->current_values;
      bool is_on = current_values.is_on();

      SimpleJsonBuilder json;
      json.add_string("type", "light_data");
      json.add_string("device_id", App.get_name());
      json.add_string("light_name", name);
      json.add_string("light_id", light->get_object_id());
      json.add_bool("state", is_on);
      json.add_number("brightness", current_values.get_brightness());
      
      // Add color information if available
      if (light->get_traits().supports_color_mode(light::ColorMode::RGB))
      {
        json.add_number("red", current_values.get_red());
        json.add_number("green", current_values.get_green());
        json.add_number("blue", current_values.get_blue());
      }
      
      if (light->get_traits().supports_color_mode(light::ColorMode::WHITE))
      {
        json.add_number("white", current_values.get_white());
      }
      
      json.add_uint64("timestamp", this->get_timestamp());
      json.add_bool("queued", false);
      std::string message = json.build();

      ESP_LOGI(TAG, "Sending light data: %s", message.c_str());
      bool sent = this->send_websocket_frame(message);

      if (!sent)
      {
        ESP_LOGW(TAG, "Failed to send light data, queuing for retry");
        this->queue_light_data(light, name);
        this->disconnect_websocket();
      }
    }

    void WebSocketClientComponent::queue_light_data(light::LightState *light, const std::string &name)
    {
      // Limit queue size to prevent memory issues
      if (this->queued_light_data_.size() >= 50)
      {
        ESP_LOGW(TAG, "Light queue full, dropping oldest light data");
        this->queued_light_data_.pop();
      }

      QueuedLightData data;
      data.light = light;
      data.name = name;
      
      auto current_values = light->current_values;
      data.state = current_values.is_on();
      data.brightness = current_values.get_brightness();
      data.red = current_values.get_red();
      data.green = current_values.get_green();
      data.blue = current_values.get_blue();
      data.white = current_values.get_white();
      data.timestamp = this->get_timestamp();

      this->queued_light_data_.push(data);
      ESP_LOGD(TAG, "Queued light data: %s state=%s (queue size: %d)",
               name.c_str(), data.state ? "ON" : "OFF", this->queued_light_data_.size());
    }

    void WebSocketClientComponent::process_queued_light_data()
    {
      if (this->queued_light_data_.empty())
      {
        return;
      }

      ESP_LOGI(TAG, "Processing %d queued light readings", this->queued_light_data_.size());

      // Process up to 5 queued items per loop to avoid blocking
      int processed = 0;
      while (!this->queued_light_data_.empty() && processed < 5)
      {
        QueuedLightData data = this->queued_light_data_.front();
        this->queued_light_data_.pop();

        // Send the queued data with original timestamp
        SimpleJsonBuilder json;
        json.add_string("type", "light_data");
        json.add_string("device_id", App.get_name());
        json.add_string("light_name", data.name);
        json.add_string("light_id", data.light->get_object_id());
        json.add_bool("state", data.state);
        json.add_number("brightness", data.brightness);
        json.add_number("red", data.red);
        json.add_number("green", data.green);
        json.add_number("blue", data.blue);
        json.add_number("white", data.white);
        json.add_uint64("timestamp", data.timestamp);
        json.add_bool("queued", true);
        std::string message = json.build();

        ESP_LOGD(TAG, "Sending queued light data: %s", message.c_str());
        bool sent = this->send_websocket_frame(message);

        if (!sent)
        {
          ESP_LOGW(TAG, "Failed to send queued light data, will retry later");
          // Clear queue to avoid issues
          this->queued_light_data_ = std::queue<QueuedLightData>();
          break;
        }

        processed++;
      }

      if (processed > 0)
      {
        ESP_LOGI(TAG, "Processed %d queued light readings, %d remaining",
                 processed, this->queued_light_data_.size());
      }
    }
#endif

#ifdef USE_BUTTON
    void WebSocketClientComponent::send_button_data(button::Button *button, const std::string &name)
    {
      if (!this->handshake_complete_)
      {
        ESP_LOGD(TAG, "Handshake not complete, queuing button data");
        this->queue_button_data(button, name);
        return;
      }

      SimpleJsonBuilder json;
      json.add_string("type", "button_data");
      json.add_string("device_id", App.get_name());
      json.add_string("button_name", name);
      json.add_string("button_id", button->get_object_id());
      json.add_string("action", "press");
      json.add_uint64("timestamp", this->get_timestamp());
      json.add_bool("queued", false); // Mark as real-time data
      std::string message = json.build();

      ESP_LOGI(TAG, "Sending button data: %s", message.c_str());
      bool sent = this->send_websocket_frame(message);

      if (!sent)
      {
        ESP_LOGW(TAG, "Failed to send button data, queuing for retry");
        this->queue_button_data(button, name);
        this->disconnect_websocket();
      }
    }

    void WebSocketClientComponent::queue_button_data(button::Button *button, const std::string &name)
    {
      // Limit queue size to prevent memory issues
      if (this->queued_button_data_.size() >= 50)
      {
        ESP_LOGW(TAG, "Button queue full, dropping oldest button data");
        this->queued_button_data_.pop();
      }

      QueuedButtonData data;
      data.button = button;
      data.name = name;
      data.timestamp = this->get_timestamp();

      this->queued_button_data_.push(data);
      ESP_LOGD(TAG, "Queued button data: %s (queue size: %d)",
               name.c_str(), this->queued_button_data_.size());
    }

    void WebSocketClientComponent::process_queued_button_data()
    {
      if (this->queued_button_data_.empty())
      {
        return;
      }

      ESP_LOGI(TAG, "Processing %d queued button events", this->queued_button_data_.size());

      // Process up to 5 queued items per loop to avoid blocking
      int processed = 0;
      while (!this->queued_button_data_.empty() && processed < 5)
      {
        QueuedButtonData data = this->queued_button_data_.front();
        this->queued_button_data_.pop();

        // Send the queued data with original timestamp
        SimpleJsonBuilder json;
        json.add_string("type", "button_data");
        json.add_string("device_id", App.get_name());
        json.add_string("button_name", data.name);
        json.add_string("button_id", data.button->get_object_id());
        json.add_string("action", "press");
        json.add_uint64("timestamp", data.timestamp);
        json.add_bool("queued", true); // Mark as queued data
        std::string message = json.build();

        ESP_LOGD(TAG, "Sending queued button data: %s", message.c_str());
        bool sent = this->send_websocket_frame(message);

        if (!sent)
        {
          ESP_LOGW(TAG, "Failed to send queued button data, will retry later");
          // Clear queue to avoid issues
          this->queued_button_data_ = std::queue<QueuedButtonData>();
          break;
        }

        processed++;
      }

      if (processed > 0)
      {
        ESP_LOGI(TAG, "Processed %d queued button events, %d remaining",
                 processed, this->queued_button_data_.size());
      }
    }
#endif

    std::string WebSocketClientComponent::parse_websocket_frame(const std::string &data)
    {
      if (data.length() < 2) return "";
      
      uint8_t first_byte = data[0];
      uint8_t second_byte = data[1];
      
      // Check if this is a text frame and FIN bit is set
      bool fin = (first_byte & 0x80) != 0;
      uint8_t opcode = first_byte & 0x0F;
      bool masked = (second_byte & 0x80) != 0;
      
      if (opcode != 0x01) return ""; // Only handle text frames
      if (!fin) return ""; // Only handle complete frames for now
      
      uint64_t payload_length = second_byte & 0x7F;
      size_t header_size = 2;
      
      if (payload_length == 126) {
        if (data.length() < 4) return "";
        payload_length = (data[2] << 8) | data[3];
        header_size = 4;
      } else if (payload_length == 127) {
        if (data.length() < 10) return "";
        payload_length = 0;
        for (int i = 0; i < 8; i++) {
          payload_length = (payload_length << 8) | data[2 + i];
        }
        header_size = 10;
      }
      
      if (masked) header_size += 4; // Account for mask
      
      if (data.length() < header_size + payload_length) return "";
      
      // Extract payload (servers don't mask, so we ignore mask for incoming)
      std::string payload = data.substr(header_size, payload_length);
      
      // Remove processed frame from buffer
      this->frame_buffer_ = data.substr(header_size + payload_length);
      
      return payload;
    }

    void WebSocketClientComponent::process_incoming_message(const std::string &message)
    {
      SimpleJsonParser parser(message);
      std::string type = parser.get_string("type");
      std::string device_id = parser.get_string("device_id");
      
      // Check if message is for this device or if it's our own message being echoed back
      if (!device_id.empty() && device_id != App.get_name()) {
        ESP_LOGD(TAG, "Message not for this device: %s", device_id.c_str());
        return;
      }
      
      // If device_id matches this device and it's an outgoing message type, it's probably an echo
      if (!device_id.empty() && device_id == App.get_name() && 
          (type == "sensor_data" || type == "heartbeat" || type == "connection" || type == "light_data" || type == "button_data")) {
        ESP_LOGD(TAG, "Ignoring echoed message from this device: %s", type.c_str());
        return;
      }
      
      ESP_LOGI(TAG, "Processing incoming message type: %s", type.c_str());
      
      if (type == "light_control") {
        this->handle_light_control(parser);
      }
#ifdef USE_BUTTON
      else if (type == "button_trigger") {
        this->handle_button_trigger(parser);
      }
#endif
      else if (type == "sensor_data" || type == "heartbeat" || type == "connection") {
        // Ignore our own outgoing message types that might be echoed back
        ESP_LOGD(TAG, "Ignoring echoed message type: %s", type.c_str());
      }
      else {
        ESP_LOGW(TAG, "Unknown message type: %s", type.c_str());
      }
    }

    void WebSocketClientComponent::handle_light_control(const SimpleJsonParser &parser)
    {
#ifdef USE_LIGHT
      std::string light_id = parser.get_string("light_id");
      std::string action = parser.get_string("action");
      
      ESP_LOGI(TAG, "Light control - ID: %s, Action: %s", light_id.c_str(), action.c_str());
      
      // Find the light by ID
      ESP_LOGI(TAG, "Looking for light with ID: %s", light_id.c_str());
      for (const auto &config : this->lights_) {
        std::string component_name = config.light->get_name();
        std::string object_id = config.light->get_object_id();
        ESP_LOGI(TAG, "Checking light: %s (name: %s, object_id: %s)", config.name.c_str(), component_name.c_str(), object_id.c_str());
        
        // Check both object_id and the component name for flexibility
        if (object_id == light_id || component_name == light_id || config.name == light_id) {
          auto light = config.light;
          ESP_LOGI(TAG, "Found matching light: %s", config.name.c_str());
          
          if (action == "turn_on") {
            auto call = light->turn_on();
            
            // Check for brightness
            double brightness = parser.get_number("brightness");
            if (brightness > 0.0) {
              call.set_brightness(brightness);
            }
            
            // Check for RGB values
            double red = parser.get_number("red");
            double green = parser.get_number("green");
            double blue = parser.get_number("blue");
            if (red >= 0.0 || green >= 0.0 || blue >= 0.0) {
              call.set_rgb(red, green, blue);
            }
            
            // Check for white value
            double white = parser.get_number("white");
            if (white >= 0.0) {
              call.set_white(white);
            }
            
            // Check for effect
            std::string effect = parser.get_string("effect");
            if (!effect.empty()) {
              call.set_effect(effect);
              ESP_LOGI(TAG, "Setting effect: %s", effect.c_str());
            }
            
            call.perform();
            ESP_LOGI(TAG, "Turned on light: %s", light_id.c_str());
            
          } else if (action == "turn_off") {
            ESP_LOGI(TAG, "Executing turn_off for light: %s", light_id.c_str());
            auto call = light->turn_off();
            call.set_state(false);
            call.perform();
            ESP_LOGI(TAG, "Turned off light: %s", light_id.c_str());
            
          } else if (action == "set_state") {
            bool state = parser.get_bool("state");
            if (state) {
              auto call = light->turn_on();
              double brightness = parser.get_number("brightness");
              if (brightness > 0.0) {
                call.set_brightness(brightness);
              }
              
              // Check for RGB values
              double red = parser.get_number("red");
              double green = parser.get_number("green");
              double blue = parser.get_number("blue");
              if (red >= 0.0 || green >= 0.0 || blue >= 0.0) {
                call.set_rgb(red, green, blue);
              }
              
              // Check for effect
              std::string effect = parser.get_string("effect");
              if (!effect.empty()) {
                call.set_effect(effect);
                ESP_LOGI(TAG, "Setting effect: %s", effect.c_str());
              }
              
              call.perform();
            } else {
              light->turn_off().perform();
            }
            ESP_LOGI(TAG, "Set light state: %s -> %s", light_id.c_str(), state ? "ON" : "OFF");
          }
          
          return;
        }
      }
      
      ESP_LOGW(TAG, "Light not found: %s", light_id.c_str());
#else
      ESP_LOGW(TAG, "Light control requested but USE_LIGHT not defined");
#endif
    }

#ifdef USE_BUTTON
    void WebSocketClientComponent::handle_button_trigger(const SimpleJsonParser &parser)
    {
      std::string button_id = parser.get_string("button_id");
      
      ESP_LOGI(TAG, "Button trigger - ID: %s", button_id.c_str());
      
      // Find the button by ID
      for (const auto &config : this->buttons_) {
        if (config.button->get_object_id() == button_id) {
          config.button->press();
          ESP_LOGI(TAG, "Triggered button: %s", button_id.c_str());
          return;
        }
      }
      
      ESP_LOGW(TAG, "Button not found: %s", button_id.c_str());
    }
#endif
  }
}