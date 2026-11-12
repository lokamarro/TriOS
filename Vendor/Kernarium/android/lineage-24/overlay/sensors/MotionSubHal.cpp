// SPDX-License-Identifier: Apache-2.0

#include "motion_protocol.hpp"
#include <V2_1/SubHal.h>
#include <android-base/unique_fd.h>
#include <log/log.h>
#include <linux/vm_sockets.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <stop_token>
#include <thread>

namespace {
using namespace android::hardware;
using namespace android::hardware::sensors;
using V1_0::Result;
using V1_0::OperationMode;
using V1_0::RateLevel;
using V1_0::SharedMemInfo;
using V2_1::Event;
using V2_1::SensorType;
using V2_1::implementation::IHalProxyCallback;
using android::base::unique_fd;

int64_t nowNs() {
    timespec time{};
    clock_gettime(CLOCK_BOOTTIME, &time);
    return int64_t(time.tv_sec) * 1'000'000'000 + time.tv_nsec;
}
struct SensorState {
    bool active = false;
    int64_t period = 10'000'000;
    int64_t next = 0;
};

class MotionSubHal final : public V2_1::implementation::ISensorsSubHal {
public:
    MotionSubHal()
        : wake_(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)),
          worker_([this](std::stop_token stop) { serve(stop); }) {}

    Return<void> getSensorsList_2_1(getSensorsList_2_1_cb callback) override {
        hidl_vec<V2_1::SensorInfo> sensors;
        sensors.resize(2);
        for (size_t i = 0; i < sensors.size(); ++i) {
            auto& sensor = sensors[i];
            sensor.sensorHandle = i + 1;
            sensor.name = i == 0 ? "Host Accelerometer" : "Host Gyroscope";
            sensor.vendor = "Andarium";
            sensor.version = 1;
            sensor.type = i == 0 ? SensorType::ACCELEROMETER : SensorType::GYROSCOPE;
            sensor.maxRange = i == 0 ? 156.9064f : 34.9066f;
            sensor.resolution = 0.001f;
            sensor.power = 0.5f;
            sensor.minDelay = 10'000;
            sensor.maxDelay = 1'000'000;
            // Continuous, non-wake-up, no FIFO, injection or direct channel.
            sensor.flags = 0;
        }
        callback(sensors);
        return Void();
    }
    Return<Result> initialize(const android::sp<IHalProxyCallback>& callback) override {
        std::lock_guard lock(mutex_);
        callback_ = callback;
        states_ = {};
        // Every subscription ended with the framework's restart.
        sendControl();
        return Result::OK;
    }
    const std::string getName() override { return "Andarium Motion"; }
    Return<Result> setOperationMode(OperationMode mode) override {
        return mode == OperationMode::NORMAL ? Result::OK : Result::BAD_VALUE;
    }
    Return<Result> activate(int32_t handle, bool enabled) override {
        if (handle < 1 || handle > 2) return Result::BAD_VALUE;
        std::lock_guard lock(mutex_);
        auto& state = states_[handle - 1];
        const bool changed = state.active != enabled;
        state.active = enabled;
        state.next = 0;
        if (changed) sendControl();
        return Result::OK;
    }
    Return<Result> batch(int32_t handle, int64_t period, int64_t latency) override {
        if (handle < 1 || handle > 2 || period < 0 || latency < 0) return Result::BAD_VALUE;
        std::lock_guard lock(mutex_);
        auto& state = states_[handle - 1];
        const auto previous = state.period;
        state.period = std::clamp<int64_t>(period, andarium::motion::minimum_period_ns,
                                           andarium::motion::maximum_period_ns);
        // The host paces its readings by the period of a sensor in use; one
        // that is not in use is told its period when it is activated.
        if (state.active && state.period != previous) sendControl();
        return Result::OK;
    }
    Return<Result> flush(int32_t handle) override {
        if (handle < 1 || handle > 2) return Result::BAD_VALUE;
        std::lock_guard lock(mutex_);
        if (!states_[handle - 1].active || !callback_) return Result::BAD_VALUE;
        Event event{};
        event.sensorHandle = handle;
        event.sensorType = SensorType::META_DATA;
        event.timestamp = nowNs();
        event.u.meta.what = V1_0::MetaDataEventType::META_DATA_FLUSH_COMPLETE;
        post(event);
        return Result::OK;
    }
    Return<Result> injectSensorData_2_1(const Event&) override { return Result::INVALID_OPERATION; }
    Return<void> registerDirectChannel(const SharedMemInfo&, registerDirectChannel_cb callback) override {
        callback(Result::INVALID_OPERATION, -1);
        return Void();
    }
    Return<Result> unregisterDirectChannel(int32_t) override { return Result::INVALID_OPERATION; }
    Return<void> configDirectReport(int32_t, int32_t, RateLevel, configDirectReport_cb callback) override {
        callback(Result::INVALID_OPERATION, 0);
        return Void();
    }
    Return<void> debug(const hidl_handle& fd, const hidl_vec<hidl_string>&) override {
        if (fd.getNativeHandle() && fd->numFds > 0) {
            std::lock_guard lock(mutex_);
            dprintf(fd->data[0], "Andarium motion: accelerometer=%d gyroscope=%d\n",
                    states_[0].active, states_[1].active);
        }
        return Void();
    }

private:
    // Tells the host which sensors applications use and how often, on the
    // connection it made. The frame is the whole state, so a later frame
    // replaces an earlier one. Called with mutex_ held. A frame that cannot
    // be sent whole would leave the host with a stale picture, so the
    // connection is shut down: the receiving loop ends, the host connects
    // again and the state is sent on that connection.
    void sendControl() {
        if (peer_ < 0) return;
        const auto frame = andarium::motion::encode_control(
            {states_[0].active, states_[1].active}, {states_[0].period, states_[1].period});
        const auto sent = send(peer_, frame.data(), frame.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
        if (sent != static_cast<ssize_t>(frame.size())) {
            ALOGW("Andarium motion: control frame not sent (%zd): %s", sent, strerror(errno));
            shutdown(peer_, SHUT_RDWR);
        }
    }
    // The connection the host made, for sendControl. It is set after the
    // host is accepted and cleared before the descriptor closes.
    void attach(int fd) {
        std::lock_guard lock(mutex_);
        peer_ = fd;
        sendControl();
    }
    void detach() {
        std::lock_guard lock(mutex_);
        peer_ = -1;
    }
    // Serialize events with activate, initialize and flush.
    void post(const Event& event) {
        callback_->postEvents({event}, callback_->createScopedWakelock(false));
    }
    void consume(const andarium::motion::Sample& sample) {
        const auto index = sample.type == 1 ? 0 : 1;
        std::lock_guard lock(mutex_);
        auto& state = states_[index];
        if (!callback_ || !state.active) return;
        const auto timestamp = nowNs();
        // Keep a sampling deadline instead of rounding every jittered host
        // interval up to two periods. Late samples never cause a catch-up burst.
        if (timestamp + state.period / 10 < state.next) return;
        state.next = state.next == 0 || timestamp > state.next + state.period
            ? timestamp + state.period : state.next + state.period;
        Event event{};
        event.sensorHandle = index + 1;
        event.sensorType = static_cast<SensorType>(sample.type);
        event.timestamp = timestamp;
        event.u.vec3.x = sample.values[0];
        event.u.vec3.y = sample.values[1];
        event.u.vec3.z = sample.values[2];
        event.u.vec3.status = V1_0::SensorStatus::ACCURACY_HIGH;
        post(event);
    }
    // Blocks until fd is readable, with no timeout: the service is started at
    // boot whether or not a host ever connects, so it must not wake to poll.
    // Returns false when the descriptor failed or the wake descriptor, written
    // when a stop is requested, became readable. Nothing drains the wake
    // descriptor, so every later call returns false at once.
    static bool readable(int fd, int wake) {
        for (;;) {
            pollfd descriptors[] = {{fd, POLLIN, 0}, {wake, POLLIN, 0}};
            const auto result = poll(descriptors, 2, -1);
            if (result < 0 && errno == EINTR) continue;
            if (result < 0 || descriptors[1].revents != 0) return false;
            return (descriptors[0].revents & POLLIN) != 0;
        }
    }
    void receive(int fd) {
        std::array<uint8_t, andarium::motion::record_size> record{};
        size_t used = 0;
        while (readable(fd, wake_.get())) {
            const auto count = recv(fd, record.data() + used, record.size() - used, MSG_DONTWAIT);
            if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            if (count <= 0) return;
            used += count;
            if (used != record.size()) continue;
            const auto sample = andarium::motion::decode(record);
            if (!sample) return;
            consume(*sample);
            used = 0;
        }
    }
    void serve(std::stop_token stop) {
        if (wake_ < 0) {
            ALOGE("Andarium motion: cannot create the wake descriptor: %s", strerror(errno));
            return;
        }
        // A stop request wakes whichever poll the thread is blocked in.
        const std::stop_callback wake_on_stop(stop, [this] { eventfd_write(wake_.get(), 1); });
        unique_fd listener(socket(AF_VSOCK, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
        sockaddr_vm address{};
        address.svm_family = AF_VSOCK;
        address.svm_cid = VMADDR_CID_ANY;
        address.svm_port = andarium::motion::port;
        if (listener < 0 || bind(listener.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) ||
            listen(listener.get(), 1)) {
            ALOGE("Andarium motion: cannot listen on vsock: %s", strerror(errno));
            return;
        }
        while (readable(listener.get(), wake_.get())) {
            sockaddr_vm peer{};
            socklen_t length = sizeof(peer);
            unique_fd stream(accept4(listener.get(), reinterpret_cast<sockaddr*>(&peer), &length,
                                     SOCK_CLOEXEC | SOCK_NONBLOCK));
            if (stream < 0) continue;
            // Only the private host channel may supply hardware samples.
            if (peer.svm_cid != VMADDR_CID_HOST) continue;
            attach(stream.get());
            receive(stream.get());
            detach();
        }
    }
    std::mutex mutex_;
    android::sp<IHalProxyCallback> callback_;
    std::array<SensorState, 2> states_{};
    int peer_ = -1;
    // Declared before the worker, so it outlives the join in the worker's
    // destructor, which requests a stop and so writes to it.
    unique_fd wake_;
    // Destroyed first; its stop request wakes the polling loop, which is then
    // joined before state teardown.
    std::jthread worker_;
};
} // namespace

V2_1::implementation::ISensorsSubHal* sensorsHalGetSubHal_2_1(uint32_t* version) {
    static MotionSubHal hal;
    *version = SUB_HAL_2_1_VERSION;
    return &hal;
}
