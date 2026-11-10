/* SPDX-License-Identifier: Apache-2.0 */

#include "common/device_sensor_page.hpp"
#include <cstring>
#include "adapters/ethercat_ssc/feedback.hpp"

#include <algorithm>
#include <zephyr/irq.h>

namespace ecat {

FeedbackAggregator::FeedbackAggregator(CanService& can, UartService& uart,
                                       ImuSource& imu)
    : can_(can), uart_(uart), imu_(imu) {}

int FeedbackAggregator::Initialize() {
    imu_.SetSampleReady([](void* context, const ImuSample& sample) {
        static_cast<FeedbackAggregator*>(context)->PublishImu(sample);
    }, this);
    const int result = imu_.Init();
    imu_ready_ = result == 0;
    return result;
}

void FeedbackAggregator::ApplyLimits(Limits& limits) const {
    limits.imu = imu_ready_;
}

bool FeedbackAggregator::CanCapacityValid(const Configuration& value) const {
    return can_.CapacityValid(value);
}

void FeedbackAggregator::Configure(const Configuration& value) {
    imu_enabled_ = value.imu && imu_ready_;
}

bool FeedbackAggregator::DrainCan(uint64_t now) {
    return can_.DrainReceive(now);
}

void FeedbackAggregator::CaptureCan(Feedback& feedback) {
    can_.CaptureFeedback(feedback);
}

void FeedbackAggregator::PollSensors() {
    ImuSample sample;
    // Normal mode publishes from DMA completion; Poll only starts pending work.
    if (imu_.Poll(sample)) {
        PublishImu(sample);
    }
}

void FeedbackAggregator::PublishImu(const ImuSample& sample) {
    namespace sensor = dust::device_sensor_page;
    std::array<uint8_t, sensor::kImuCompactBytes> fields{};
    std::copy_n(sample.raw.begin() + 1, 12, fields.begin());
    std::memcpy(fields.data() + 12, &sample.temperature_c, 4);
    sensor::Page page;
    (void)sensor::Encode(sensor::Kind::ImuCompact, sample.sequence,
                         sample.capture_us, false, fields.data(), fields.size(), 0, page);
    static_assert(sensor::kHeaderBytes + sensor::kImuCompactBytes == 28);
    const auto page_key = irq_lock();
    std::copy_n(page.begin(), imu_page_.size(), imu_page_.begin());
    irq_unlock(page_key);
    const auto key = irq_lock();
    latest_ = sample;
    have_sample_ = true;
    irq_unlock(key);
}

bool FeedbackAggregator::SnapshotImu(ImuSample& sample) const {
    const auto key = irq_lock();
    const bool available = have_sample_;
    sample = latest_;
    irq_unlock(key);
    return available;
}

void FeedbackAggregator::Capture(Session& session, Feedback& feedback,
                                 uint64_t now) {
    can_.CaptureFeedback(feedback);
    uart_.Capture(session, now);

    ImuSample sample;
    if (!imu_enabled_ || !SnapshotImu(sample)) {
        return;
    }
    feedback.imu_valid = true;
    feedback.imu_sequence = ++imu_sequence_;
    feedback.imu_capture_us = sample.capture_us;
    feedback.temperature = sample.temperature_c;
    std::copy(sample.raw.begin(), sample.raw.end(),
              feedback.imu_raw.begin());
}
bool FeedbackAggregator::CopyImuPage(uint8_t* destination) const {
    if (!imu_enabled_ || !imu_page_[0]) {
        return false;
    }
    std::copy(imu_page_.begin(), imu_page_.end(), destination);
    return true;
}




bool FeedbackAggregator::LatestTemperature(float& temperature_c,
                                           uint64_t& capture_us) const {
    ImuSample sample;
    if (!SnapshotImu(sample)) {
        return false;
    }
    temperature_c = sample.temperature_c;
    capture_us = sample.capture_us;
    return true;
}

}  // namespace ecat
