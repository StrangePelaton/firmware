#ifdef ARCH_ESP32

#include "SharpPM25Sensor.h"
#include "MeshService.h"
#include "esp_adc_cal.h"
#include "graphics/Screen.h"

SharpPM25Sensor *sharpPM25Sensor = nullptr;

SharpPM25Sensor::SharpPM25Sensor()
    : ProtobufModule("SharpPM25", meshtastic_PortNum_TELEMETRY_APP, &meshtastic_Telemetry_msg),
      concurrency::OSThread("SharpPM25")
{
    // Initialize GPIO for sensor LED (active low)
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << SENSOR_LED_PIN);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);

    // Initialize sensor in off state (HIGH = off)
    gpio_set_level(SENSOR_LED_PIN, 1);

    // Initialize ADC
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(SENSOR_OUT_PIN, ADC_ATTEN_DB_6);

    lastMeasurementPacket = nullptr;
    setIntervalFromNow(10 * 1000);

    LOG_DEBUG("SharpPM25Sensor initialized (LED: GPIO%d, OUT: GPIO4)\n", SENSOR_LED_PIN);
}

void SharpPM25Sensor::turnOnSensor()
{
    // LED active low
    gpio_set_level(SENSOR_LED_PIN, 0);
    LOG_DEBUG("PM2.5 Sensor warming up...\n");
}

void SharpPM25Sensor::turnOffSensor()
{
    // LED active low
    gpio_set_level(SENSOR_LED_PIN, 1);
    LOG_DEBUG("PM2.5 Sensor shutdown\n");
}

uint16_t SharpPM25Sensor::readVoltage()
{
    // Read ADC and convert to millivolts
    // ESP32 ADC is 12-bit (0-4095) with 3.3V reference
    // Using Vref=1.1V internally calibrated
    
    static esp_adc_cal_characteristics_t adc_chars;
    static bool adc_chars_initialized = false;

    if (!adc_chars_initialized) {
        esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_6, ADC_WIDTH_BIT_12, 1100, &adc_chars);
        adc_chars_initialized = true;
    }

    uint32_t raw = adc1_get_raw(SENSOR_OUT_PIN);
    uint16_t mv = esp_adc_cal_raw_to_voltage(raw, &adc_chars);

    return mv;
}

float SharpPM25Sensor::voltageToAQI(uint16_t voltage_mv)
{
    // Sharp GP2Y1010AU0F conversion
    // Output voltage: 0V = 0µg/m³, 4.0V = 500µg/m³
    // Linear mapping: PM2.5 = (voltage - 0.5V) / 0.004V * 100 + offset
    //
    // Common calibration (adjust based on your conditions):
    // voltage_mv = 0   → PM2.5 = 0 µg/m³
    // voltage_mv = 500 → PM2.5 ≈ 10 µg/m³ (clean air baseline)
    // voltage_mv = 4000 → PM2.5 ≈ 500 µg/m³

    if (voltage_mv < 500) {
        return 0.0f; // Below minimum detectable
    }

    // Subtract baseline (500mV = 0µg/m³) and scale
    float pm25 = (float)(voltage_mv - 500) / 40.0f; // 40mV per 10µg/m³

    return pm25 < 0 ? 0.0f : pm25;
}

float SharpPM25Sensor::readPM25()
{
    turnOnSensor();

    // Warm up
    vTaskDelay(pdMS_TO_TICKS(WARMUP_TIME_MS));

    // Sample over 60 seconds
    uint32_t sum_voltage = 0;
    uint16_t sample_count = 0;
    uint32_t start_time = millis();

    LOG_DEBUG("Sampling PM2.5 for 60 seconds...\n");

    while (millis() - start_time < SAMPLE_DURATION_MS) {
        uint16_t voltage = readVoltage();
        sum_voltage += voltage;
        sample_count++;

        if (sample_count % 10 == 0) {
            LOG_DEBUG("Sample %d: %dmV\n", sample_count, voltage);
        }

        vTaskDelay(pdMS_TO_TICKS(600)); // ~600ms between samples = 100 samples/60sec
    }

    turnOffSensor();

    // Calculate average
    uint16_t avg_voltage = sample_count > 0 ? (sum_voltage / sample_count) : 0;
    float pm25 = voltageToAQI(avg_voltage);

    LOG_DEBUG("PM2.5 sampling complete: %d samples, avg=%dmV, PM2.5=%.1f µg/m³\n", 
              sample_count, avg_voltage, pm25);

    return pm25;
}

int32_t SharpPM25Sensor::runOnce()
{
    // Check if it's time for a reading
    uint32_t now = millis();

    if (now - lastReadTime < POLL_INTERVAL_MS) {
        // Not time yet, check again in 1 minute
        return 60000;
    }

    lastReadTime = now;

    // Take a reading
    lastPM25Value = readPM25();

    // Send telemetry
    sendTelemetry();

    // Schedule next reading in 15 minutes
    return POLL_INTERVAL_MS;
}

bool SharpPM25Sensor::sendTelemetry(NodeNum dest, bool phoneOnly)
{
    meshtastic_Telemetry t = meshtastic_Telemetry_init_default;
    t.which_variant = meshtastic_Telemetry_air_quality_metrics_tag;
    t.time = getTime();
    t.variant.air_quality_metrics.has_pm25_standard = true;
    t.variant.air_quality_metrics.pm25_standard = (uint32_t)lastPM25Value;

    meshtastic_MeshPacket *p = allocDataProtobuf(t);
    p->to = dest;
    p->decoded.want_response = false;
    p->priority = meshtastic_MeshPacket_Priority_BACKGROUND;

    if (phoneOnly) {
        service->sendToPhone(p);
    } else {
        service->sendToMesh(p, RX_SRC_LOCAL, true);
    }

    LOG_INFO("Sent PM2.5 telemetry: %.1f µg/m³\n", lastPM25Value);
    return true;
}

bool SharpPM25Sensor::handleReceivedProtobuf(const meshtastic_MeshPacket &mp, meshtastic_Telemetry *p)
{
    if (p->which_variant == meshtastic_Telemetry_air_quality_metrics_tag) {
        // Log received air quality metrics
        const char *sender = getSenderShortName(mp);
        LOG_INFO("(Received from %s): PM2.5=%u µg/m³\n", sender, p->variant.air_quality_metrics.pm25_standard);

        // Release previous packet and store this one
        if (lastMeasurementPacket != nullptr) {
            packetPool.release(lastMeasurementPacket);
        }
        lastMeasurementPacket = packetPool.allocCopy(mp);
    }

    return false; // Let others look at this message also
}

meshtastic_MeshPacket *SharpPM25Sensor::allocReply()
{
    if (currentRequest) {
        if (isMultiHopBroadcastRequest()) {
            ignoreRequest = true;
            return NULL;
        }

        auto req = *currentRequest;
        const auto &p = req.decoded;
        meshtastic_Telemetry scratch = meshtastic_Telemetry_init_default;

        if (pb_decode_from_bytes(p.payload.bytes, p.payload.size, &meshtastic_Telemetry_msg, &scratch)) {
            // Check if this is a request for air quality metrics
            if (scratch.which_variant == meshtastic_Telemetry_air_quality_metrics_tag) {
                LOG_INFO("Air quality telemetry reply to request");

                meshtastic_Telemetry t = meshtastic_Telemetry_init_default;
                t.which_variant = meshtastic_Telemetry_air_quality_metrics_tag;
                t.time = getTime();
                t.variant.air_quality_metrics.has_pm25_standard = true;
                t.variant.air_quality_metrics.pm25_standard = (uint32_t)lastPM25Value;

                return allocDataProtobuf(t);
            }
        } else {
            LOG_ERROR("Error decoding SharpPM25Sensor telemetry request!");
        }
    }

    return NULL;
}

#if HAS_SCREEN
void SharpPM25Sensor::drawFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y)
{
    display->clear();
    display->setTextAlignment(TEXT_ALIGN_LEFT);
    display->setFont(FONT_SMALL);

    if (lastMeasurementPacket == nullptr) {
        display->drawString(x, y, "PM2.5: No measurement");
        return;
    }

    // Decode the last measurement packet
    meshtastic_Telemetry lastMeasurement;
    uint32_t agoSecs = service->GetTimeSinceMeshPacket(lastMeasurementPacket);
    const char *lastSender = getSenderShortName(*lastMeasurementPacket);

    const meshtastic_Data &p = lastMeasurementPacket->decoded;
    if (!pb_decode_from_bytes(p.payload.bytes, p.payload.size, &meshtastic_Telemetry_msg, &lastMeasurement)) {
        display->drawString(x, y, "PM2.5: Decode Error");
        LOG_ERROR("Unable to decode PM2.5 packet");
        return;
    }

    char headerStr[64];
    snprintf(headerStr, sizeof(headerStr), "PM2.5 From: %s (%us)", lastSender, (int)agoSecs);
    display->drawString(x, y, headerStr);

    if (lastMeasurement.which_variant == meshtastic_Telemetry_air_quality_metrics_tag) {
        uint32_t pm25 = lastMeasurement.variant.air_quality_metrics.pm25_standard;

        char pm25Str[32];
        snprintf(pm25Str, sizeof(pm25Str), "PM2.5: %u µg/m³", pm25);
        display->drawString(x, y + 16, pm25Str);

        // Add quality indicator
        const char *quality;
        if (pm25 <= 35) {
            quality = "Good";
        } else if (pm25 <= 75) {
            quality = "Moderate";
        } else if (pm25 <= 115) {
            quality = "Unhealthy (SG)";
        } else if (pm25 <= 150) {
            quality = "Unhealthy";
        } else if (pm25 <= 250) {
            quality = "Very Unhealthy";
        } else {
            quality = "Hazardous";
        }

        display->drawString(x, y + 32, quality);
    }
}
#endif

#endif // ARCH_ESP32
