#pragma once

#ifdef ARCH_ESP32

#include "ProtobufModule.h"
#include "concurrency/OSThread.h"
#include <esp_adc/adc_oneshot.h>
#include <driver/adc.h>
#include <OLEDDisplay.h>
#include <OLEDDisplayUi.h>

/**
 * Sharp GP2Y1010AU0F PM2.5 Dust Sensor Module
 * 
 * Reads analog dust sensor and reports PM2.5 via telemetry.
 * - Warms up sensor every 15 minutes
 * - Takes 60 seconds of readings
 * - Reports average via air quality telemetry
 */
class SharpPM25Sensor : public ProtobufModule<meshtastic_Telemetry>, private concurrency::OSThread
{
  public:
    SharpPM25Sensor();

  protected:
    virtual bool handleReceivedProtobuf(const meshtastic_MeshPacket &mp, meshtastic_Telemetry *p) override;
    virtual meshtastic_MeshPacket *allocReply() override;
    virtual int32_t runOnce() override;
    virtual bool wantUIFrame() override { return true; }
    virtual void drawFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y) override;

    /**
     * Send PM2.5 telemetry into the mesh
     */
    bool sendTelemetry(NodeNum dest = NODENUM_BROADCAST, bool phoneOnly = false);

  private:
    static const uint32_t POLL_INTERVAL_MS = 15 * 60 * 1000; // 15 minutes
    static const uint32_t WARMUP_TIME_MS = 30000;            // 30 second warmup
    static const uint32_t SAMPLE_DURATION_MS = 60000;        // 60 second sampling
    static const uint16_t NUM_SAMPLES = 100;                 // ~1 sample per 600ms

    static const gpio_num_t SENSOR_LED_PIN = GPIO_NUM_33;    // Active low
    static const adc_channel_t SENSOR_OUT_PIN = static_cast<adc_channel_t>(4); // GPIO 4 / ADC1_CH4

    void turnOnSensor();
    void turnOffSensor();
    float readPM25();
    uint16_t readVoltage();
    float voltageToAQI(uint16_t voltage_mv);

    uint32_t lastReadTime = 0;
    float lastPM25Value = 0.0f;
    meshtastic_MeshPacket *lastMeasurementPacket = nullptr;
    uint32_t lastSentToPhone = 0;
};

extern SharpPM25Sensor *sharpPM25Sensor;

#endif // ARCH_ESP32
