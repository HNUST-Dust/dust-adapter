/* SPDX-License-Identifier: Apache-2.0 */

#include "adapters/ethercat_ssc/feedback.hpp"

#include <algorithm>

namespace ecat {

FeedbackAggregator::FeedbackAggregator(CanService& can, UartService& uart,
                                       ImuSource& imu)
    : can_(can), uart_(uart), imu_(imu) {}

int FeedbackAggregator::Initialize() {
    return imu_.Init();
}

bool FeedbackAggregator::CanCapacityValid(const Configuration& value) const {
    return can_.CapacityValid(value);
}

void FeedbackAggregator::Configure(const Configuration& value) {
    imu_enabled_ = value.imu;
}

bool FeedbackAggregator::DrainCan(uint64_t now) {
    return can_.DrainReceive(now);
}

void FeedbackAggregator::CaptureCan(Feedback& feedback) {
    can_.CaptureFeedback(feedback);
}

void FeedbackAggregator::PollSensors() {
    ImuSample sample;
    if (imu_.Poll(sample)) {
        latest_ = sample;
        have_sample_ = true;
    }
}

void FeedbackAggregator::Capture(Session& session, Feedback& feedback,
                                 uint64_t now) {
    can_.CaptureFeedback(feedback);
    uart_.Capture(session, now);

    if (!have_sample_ || !imu_enabled_) {
        return;
    }
    feedback.imu_valid = true;
    feedback.imu_sequence = ++imu_sequence_;
    feedback.imu_capture_us = latest_.capture_us;
    feedback.temperature = latest_.temperature_c;
    std::copy(latest_.raw.begin(), latest_.raw.end(),
              feedback.imu_raw.begin());
}

bool FeedbackAggregator::LatestTemperature(float& temperature_c,
                                           uint64_t& capture_us) const {
    if (!have_sample_) {
        return false;
    }
    temperature_c = latest_.temperature_c;
    capture_us = latest_.capture_us;
    return true;
}

}  // namespace ecat
