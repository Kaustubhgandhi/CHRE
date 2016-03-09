/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "contexthub.h"

#include <cstring>
#include <errno.h>
#include <vector>

#include "apptohostevent.h"
#include "log.h"
#include "sensorevent.h"
#include "util.h"

namespace android {

#define UNUSED_PARAM(param) (void) (param)

constexpr int kCalibrationTimeoutMs(10000);

struct SensorTypeNames {
    SensorType sensor_type;
    const char *name_abbrev;
};

static const SensorTypeNames sensor_names_[] = {
    { SensorType::Accel,                "accel" },
    { SensorType::AnyMotion,            "anymo" },
    { SensorType::NoMotion,             "nomo" },
    { SensorType::SignificantMotion,    "sigmo" },
    { SensorType::Flat,                 "flat" },
    { SensorType::Gyro,                 "gyro" },
    //{ SensorType::GyroUncal,            "gyro_uncal" },
    { SensorType::Magnetometer,         "mag" },
    //{ SensorType::MagnetometerUncal,    "mag_uncal" },
    { SensorType::Barometer,            "baro" },
    { SensorType::Temperature,          "temp" },
    { SensorType::AmbientLightSensor,   "als" },
    { SensorType::Proximity,            "prox" },
    { SensorType::Orientation,          "orien" },
    //{ SensorType::HeartRateECG,         "ecg" },
    //{ SensorType::HeartRatePPG,         "ppg" },
    { SensorType::Gravity,              "gravity" },
    { SensorType::LinearAccel,          "linear_acc" },
    { SensorType::RotationVector,       "rotation" },
    { SensorType::GeomagneticRotationVector, "geomag" },
    { SensorType::GameRotationVector,   "game" },
    { SensorType::StepCount,            "step_cnt" },
    { SensorType::StepDetect,           "step_det" },
    { SensorType::Gesture,              "gesture" },
    { SensorType::Tilt,                 "tilt" },
    { SensorType::DoubleTwist,          "twist" },
    { SensorType::DoubleTap,            "doubletap" },
    { SensorType::WindowOrientation,    "win_orien" },
    { SensorType::Hall,                 "hall" },
    { SensorType::Activity,             "activity" },
    { SensorType::Vsync,                "vsync" },
};

SensorType ContextHub::SensorAbbrevNameToType(const char *sensor_name_abbrev) {
    for (unsigned int i = 0; i < ARRAY_LEN(sensor_names_); i++) {
        if (strcmp(sensor_names_[i].name_abbrev, sensor_name_abbrev) == 0) {
            return sensor_names_[i].sensor_type;
        }
    }

    return SensorType::Invalid_;
}

SensorType ContextHub::SensorAbbrevNameToType(const std::string& abbrev_name) {
    return ContextHub::SensorAbbrevNameToType(abbrev_name.c_str());
}

std::string ContextHub::SensorTypeToAbbrevName(SensorType sensor_type) {
    for (unsigned int i = 0; i < ARRAY_LEN(sensor_names_); i++) {
        if (sensor_names_[i].sensor_type == sensor_type) {
            return std::string(sensor_names_[i].name_abbrev);
        }
    }

    char buffer[24];
    snprintf(buffer, sizeof(buffer), "unknown (%d)",
             static_cast<int>(sensor_type));
    return std::string(buffer);
}

std::string ContextHub::ListAllSensorAbbrevNames() {
    std::string sensor_list;
    for (unsigned int i = 0; i < ARRAY_LEN(sensor_names_); i++) {
        sensor_list += sensor_names_[i].name_abbrev;
        if (i < ARRAY_LEN(sensor_names_) - 1) {
            sensor_list += ", ";
        }
    }

    return sensor_list;
}

bool ContextHub::Flash(const std::string& filename) {
    FILE *firmware_file = fopen(filename.c_str(), "r");
    if (!firmware_file) {
        LOGE("Failed to open firmware image: %d (%s)", errno, strerror(errno));
        return false;
    }

    fseek(firmware_file, 0, SEEK_END);
    long file_size = ftell(firmware_file);
    fseek(firmware_file, 0, SEEK_SET);

    auto firmware_data = std::vector<uint8_t>(file_size);
    size_t bytes_read = fread(firmware_data.data(), sizeof(uint8_t),
        file_size, firmware_file);
    fclose(firmware_file);

    if (bytes_read != static_cast<size_t>(file_size)) {
        LOGE("Read of firmware file returned %zu, expected %ld",
            bytes_read, file_size);
        return false;
    }
    return FlashSensorHub(firmware_data);
}

bool ContextHub::CalibrateSensors(const std::vector<SensorSpec>& sensors) {
    bool success = ForEachSensor(sensors, [this](const SensorSpec &spec) -> bool {
        return CalibrateSingleSensor(spec);
    });

    if (success) {
        success = SaveCalibration();
    }
    return success;
}

bool ContextHub::EnableSensor(const SensorSpec& spec) {
    ConfigureSensorRequest req;

    req.config.event_type = static_cast<uint32_t>(EventType::ConfigureSensor);
    req.config.sensor_type = static_cast<uint8_t>(spec.sensor_type);
    req.config.command = static_cast<uint8_t>(
        ConfigureSensorRequest::CommandType::Enable);
    if (spec.special_rate != SensorSpecialRate::None) {
        req.config.rate = static_cast<uint32_t>(spec.special_rate);
    } else {
        req.config.rate = ConfigureSensorRequest::FloatRateToFixedPoint(
            spec.rate_hz);
    }
    req.config.latency = spec.latency_ns;

    LOGI("Enabling sensor %d at rate %.0f Hz (special 0x%x) and latency %.2f ms",
         spec.sensor_type, spec.rate_hz, spec.special_rate,
         spec.latency_ns / 1000000.0f);
    auto result = WriteEvent(req);
    if (result == TransportResult::Success) {
        sensor_is_active_[static_cast<int>(spec.sensor_type)] = true;
        return true;
    }

    LOGE("Could not enable sensor %d", spec.sensor_type);
    return false;
}

bool ContextHub::EnableSensors(const std::vector<SensorSpec>& sensors) {
    return ForEachSensor(sensors, [this](const SensorSpec &spec) -> bool {
        return EnableSensor(spec);
    });
}

bool ContextHub::DisableSensor(SensorType sensor_type) {
    ConfigureSensorRequest req;

    req.config.event_type = static_cast<uint32_t>(EventType::ConfigureSensor);
    req.config.sensor_type = static_cast<uint8_t>(sensor_type);
    req.config.command = static_cast<uint8_t>(
        ConfigureSensorRequest::CommandType::Disable);

    // Note that nanohub treats us as a single client, so if we call enable
    // twice then disable once, the sensor will be disabled
    LOGI("Disabling sensor %d", sensor_type);
    auto result = WriteEvent(req);
    if (result == TransportResult::Success) {
        sensor_is_active_[static_cast<int>(sensor_type)] = false;
        return true;
    }

    LOGE("Could not disable sensor %d", sensor_type);
    return false;
}

bool ContextHub::DisableSensors(const std::vector<SensorSpec>& sensors) {
    return ForEachSensor(sensors, [this](const SensorSpec &spec) -> bool {
        return DisableSensor(spec.sensor_type);
    });
}

bool ContextHub::DisableAllSensors() {
    bool success = true;

    for (int sensor_type = static_cast<int>(SensorType::Invalid_) + 1;
            sensor_type < static_cast<int>(SensorType::Max_);
            ++sensor_type) {
        success &= DisableSensor(static_cast<SensorType>(sensor_type));
    }

    return success;
}

bool ContextHub::DisableActiveSensors() {
    bool success = true;

    LOGD("Disabling all active sensors");
    for (int sensor_type = static_cast<int>(SensorType::Invalid_) + 1;
            sensor_type < static_cast<int>(SensorType::Max_);
            ++sensor_type) {
        if (sensor_is_active_[sensor_type]) {
            success &= DisableSensor(static_cast<SensorType>(sensor_type));
        }
    }

    return success;
}

void ContextHub::PrintAllEvents(unsigned int limit) {
    bool continuous = (limit == 0);
    auto event_printer = [&limit, continuous](const SensorEvent& event) -> bool {
        printf("%s", event.ToString().c_str());
        return (continuous || --limit > 0);
    };
    ReadSensorEvents(event_printer);
}

void ContextHub::PrintSensorEvents(SensorType type, int limit) {
    bool continuous = (limit == 0);
    auto event_printer = [type, &limit, continuous](const SensorEvent& event) -> bool {
        if (event.GetSensorType() == type) {
            printf("%s", event.ToString().c_str());
            limit -= event.GetNumSamples();
        }
        return (continuous || limit > 0);
    };
    ReadSensorEvents(event_printer);
}

void ContextHub::PrintSensorEvents(const std::vector<SensorSpec>& sensors, int limit) {
    bool continuous = (limit == 0);
    auto event_printer = [&sensors, &limit, continuous](const SensorEvent& event) -> bool {
        SensorType event_source = event.GetSensorType();
        for (unsigned int i = 0; i < sensors.size(); i++) {
            if (sensors[i].sensor_type == event_source) {
                printf("%s", event.ToString().c_str());
                limit -= event.GetNumSamples();
                break;
            }
        }
        return (continuous || limit > 0);
    };
    ReadSensorEvents(event_printer);
}

// Protected methods -----------------------------------------------------------

bool ContextHub::CalibrateSingleSensor(const SensorSpec& sensor) {
    ConfigureSensorRequest req;

    req.config.event_type = static_cast<uint32_t>(EventType::ConfigureSensor);
    req.config.sensor_type = static_cast<uint8_t>(sensor.sensor_type);
    req.config.command = static_cast<uint8_t>(
        ConfigureSensorRequest::CommandType::Calibrate);

    LOGI("Issuing calibration request to sensor %d (%s)", sensor.sensor_type,
         ContextHub::SensorTypeToAbbrevName(sensor.sensor_type).c_str());
    auto result = WriteEvent(req);
    if (result != TransportResult::Success) {
        LOGE("Failed to calibrate sensor %d", sensor.sensor_type);
        return false;
    }

    bool success = false;
    auto calEventHandler = [this, &sensor, &success](const AppToHostEvent &event) -> bool {
        if (event.IsCalibrationEventForSensor(sensor.sensor_type)) {
            success = HandleCalibrationResult(sensor, event);
            return false;
        }
        return true;
    };

    result = ReadAppEvents(calEventHandler, kCalibrationTimeoutMs);
    if (result != TransportResult::Success) {
      LOGE("Error reading calibration response %d", static_cast<int>(result));
      return false;
    }

    return true;
}

bool ContextHub::ForEachSensor(const std::vector<SensorSpec>& sensors,
        std::function<bool(const SensorSpec&)> callback) {
    bool success = true;

    for (unsigned int i = 0; success && i < sensors.size(); i++) {
        success &= callback(sensors[i]);
    }

    return success;
}

bool ContextHub::HandleCalibrationResult(const SensorSpec& sensor,
        const AppToHostEvent &event) {
    auto hdr = reinterpret_cast<const SensorAppEventHeader *>(event.GetDataPtr());
    if (hdr->status) {
        LOGE("Calibration of sensor %d (%s) failed with status %u",
             sensor.sensor_type,
             ContextHub::SensorTypeToAbbrevName(sensor.sensor_type).c_str(),
             hdr->status);
        return false;
    }

    bool success = false;
    switch (sensor.sensor_type) {
      case SensorType::Accel:
      case SensorType::Gyro: {
        auto result = reinterpret_cast<const TripleAxisCalibrationResult *>(
            event.GetDataPtr());
        success = SetCalibration(sensor.sensor_type, result->xBias,
                                 result->yBias, result->zBias);
        break;
      }

      case SensorType::Barometer:
      case SensorType::AmbientLightSensor: {
        auto result = reinterpret_cast<const FloatCalibrationResult *>(
            event.GetDataPtr());
        if (sensor.have_cal_ref) {
            success = SetCalibration(sensor.sensor_type,
                                     (sensor.cal_ref - result->value));
        }
        break;
      }

      case SensorType::Proximity: {
        auto result = reinterpret_cast<const FourAxisCalibrationResult *>(
            event.GetDataPtr());
        success = SetCalibration(sensor.sensor_type, result->xBias,
                                 result->yBias, result->zBias, result->wBias);
        break;
      }

      default:
        LOGE("Calibration not supported for sensor type %d",
             static_cast<int>(sensor.sensor_type));
    }

    return success;
}

ContextHub::TransportResult ContextHub::ReadAppEvents(
        std::function<bool(const AppToHostEvent&)> callback, int timeout_ms) {
    using Milliseconds = std::chrono::milliseconds;

    TransportResult result;
    bool timeout_required = timeout_ms > 0;
    bool keep_going = true;

    while (keep_going) {
        if (timeout_required && timeout_ms <= 0) {
            return TransportResult::Timeout;
        }

        std::unique_ptr<ReadEventResponse> event;

        SteadyClock start_time = std::chrono::steady_clock::now();
        result = ReadEvent(&event, timeout_ms);
        SteadyClock end_time = std::chrono::steady_clock::now();

        auto delta = end_time - start_time;
        timeout_ms -= std::chrono::duration_cast<Milliseconds>(delta).count();

        if (result != TransportResult::Success) {
            return result;
        }

        if (event->IsAppToHostEvent()) {
            AppToHostEvent *app_event =
                reinterpret_cast<AppToHostEvent*>(event.get());
            keep_going = callback(*app_event);
        } else {
            LOGD("Ignoring non-app-to-host event");
        }
    }

    return TransportResult::Success;
}

void ContextHub::ReadSensorEvents(std::function<bool(const SensorEvent&)> callback) {
    TransportResult result;
    bool keep_going = true;

    while (keep_going) {
        std::unique_ptr<ReadEventResponse> event;
        result = ReadEvent(&event);
        if (result == TransportResult::Success && event->IsSensorEvent()) {
            SensorEvent *sensor_event = reinterpret_cast<SensorEvent*>(
                event.get());
            keep_going = callback(*sensor_event);
        } else {
            if (result != TransportResult::Success) {
                LOGE("Error %d while reading", static_cast<int>(result));
                if (result != TransportResult::ParseFailure) {
                    break;
                }
            } else {
                LOGD("Ignoring non-sensor event");
            }
        }
    }
}

bool ContextHub::SendCalibrationData(SensorType sensor_type,
        const std::vector<uint8_t>& cal_data) {
    ConfigureSensorRequest req;

    req.config.event_type = static_cast<uint32_t>(EventType::ConfigureSensor);
    req.config.sensor_type = static_cast<uint8_t>(sensor_type);
    req.config.command = static_cast<uint8_t>(
        ConfigureSensorRequest::CommandType::ConfigData);
    req.SetAdditionalData(cal_data);

    auto result = WriteEvent(req);
    return (result == TransportResult::Success);
}

ContextHub::TransportResult ContextHub::WriteEvent(
        const WriteEventRequest& request) {
    return WriteEvent(request.GetBytes());
}

ContextHub::TransportResult ContextHub::ReadEvent(
        std::unique_ptr<ReadEventResponse>* response, int timeout_ms) {
    std::vector<uint8_t> responseBuf(256);
    ContextHub::TransportResult result = ReadEvent(responseBuf, timeout_ms);
    if (result == TransportResult::Success) {
        *response = ReadEventResponse::FromBytes(responseBuf);
        if (*response == nullptr) {
            result = TransportResult::ParseFailure;
        }
    }
    return result;
}

// Stubs for subclasses that don't implement calibration support
bool ContextHub::LoadCalibration() {
    LOGE("Loading calibration data not implemented");
    return false;
}

bool ContextHub::SetCalibration(SensorType sensor_type, int32_t data) {
    UNUSED_PARAM(sensor_type);
    UNUSED_PARAM(data);
    return false;
}

bool ContextHub::SetCalibration(SensorType sensor_type, float data) {
    UNUSED_PARAM(sensor_type);
    UNUSED_PARAM(data);
    return false;
}

bool ContextHub::SetCalibration(SensorType sensor_type, int32_t x,
        int32_t y, int32_t z) {
    UNUSED_PARAM(sensor_type);
    UNUSED_PARAM(x);
    UNUSED_PARAM(y);
    UNUSED_PARAM(z);
    return false;
}

bool ContextHub::SetCalibration(SensorType sensor_type, int32_t x,
        int32_t y, int32_t z, int32_t w) {
    UNUSED_PARAM(sensor_type);
    UNUSED_PARAM(x);
    UNUSED_PARAM(y);
    UNUSED_PARAM(z);
    UNUSED_PARAM(w);
    return false;
}

bool ContextHub::SaveCalibration() {
    LOGE("Saving calibration data not implemented");
    return false;
}

}  // namespace android
