#pragma once
#include <Arduino.h>

/* Forward-declare MQTT log forwarding hook (defined in mqtt.cpp). */
extern bool mqttLogForwardingEnabled;
void mqttPublishLog(const char *line);

/** Maximum line length (including null terminator) for the MQTT log buffer. */
static const uint16_t MQTT_LOGGER_LINE_BUF = 512;

/**
 * MqttLogger — drop-in Print replacement that tees output to the hardware
 * serial port AND, when mqttLogForwardingEnabled is true, forwards complete
 * log lines to the MQTT log topic.
 *
 * The object is instantiated once in mqtt.cpp as the global LOGGER.
 * All other translation units that include this header use LOGGER instead
 * of Serial for tagged log output so that the forwarding is transparent.
 */
class MqttLogger : public Print {
    HardwareSerial &_hw;
    static const uint16_t BUF_SIZE = MQTT_LOGGER_LINE_BUF;
    char     _buf[BUF_SIZE];
    uint16_t _pos;
    bool     _guard;  // re-entrancy guard: prevents recursive publish

public:
    explicit MqttLogger(HardwareSerial &hw)
        : _hw(hw), _pos(0), _guard(false) {}

    size_t write(uint8_t c) override {
        _hw.write(c);
        if (_guard) return 1;
        if (_pos < BUF_SIZE - 1) _buf[_pos++] = static_cast<char>(c);
        if (c == '\n' || _pos >= BUF_SIZE - 1) {
            _buf[_pos] = '\0';
            if (mqttLogForwardingEnabled) {
                _guard = true;
                mqttPublishLog(_buf);
                _guard = false;
            }
            _pos = 0;
        }
        return 1;
    }

    size_t write(const uint8_t *buf, size_t sz) override {
        for (size_t i = 0; i < sz; i++) write(buf[i]);
        return sz;
    }

    int availableForWrite() override { return _hw.availableForWrite(); }
};

extern MqttLogger LOGGER;
