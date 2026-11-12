// SPDX-License-Identifier: GPL-2.0-or-later

#include "host_battery.hpp"
#include "protocol.hpp"
#include "require.hpp"
#include "virtio_battery_device.hpp"
#include "wire_bytes.hpp"

#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <type_traits>
#include <vector>

namespace {

namespace protocol = uml::battery::protocol;

using uml::testing::bytes;
using uml::testing::loadExact;
using uml::testing::require;

class TestProvider final : public uml::platform::HostBatteryProvider {
public:
    uml::platform::HostBatteryState state() noexcept override
    {
        ++queries;
        return current;
    }

    uml::platform::HostBatteryState current;
    unsigned queries {};
};

struct Fixture final {
    Fixture()
    {
        auto owned = std::make_unique<TestProvider>();
        provider = owned.get();
        device = std::make_unique<uml::battery::VirtioBatteryDevice>(
            std::move(owned));
    }

    TestProvider* provider {};
    std::unique_ptr<uml::battery::VirtioBatteryDevice> device;
};

void exposesOneStatusQueueWithoutFeatures()
{
    Fixture fixture;
    require(fixture.device->features() == 0);
    require(fixture.device->queueCount() == 1);
    require(fixture.device->configuration().empty());
}

void reportsHostPercentageAndChargingState()
{
    Fixture fixture;
    fixture.provider->current = {
        .percentage = 64,
        .status = uml::platform::HostBatteryStatus::charging,
        .present = true,
        .externalPower = true,
    };
    const protocol::Request request {.version = protocol::version};
    const auto result = fixture.device->processQueue(
        protocol::statusQueue, 1, bytes(request), sizeof(protocol::State));
    require(result.action == uml::virtio::VhostUserQueueAction::complete);
    const auto state = loadExact<protocol::State>(result.response);
    require(state.version == protocol::version);
    require(state.percentage == 64);
    require(state.status == protocol::ChargeStatus::charging);
    require(state.present == 1);
    require(state.externalPower == 1);
    require(fixture.provider->queries == 1);
}

void preservesTheBatterylessHostFallback()
{
    Fixture fixture;
    const protocol::Request request {.version = protocol::version};
    const auto result = fixture.device->processQueue(
        protocol::statusQueue, 2, bytes(request), sizeof(protocol::State));
    const auto state = loadExact<protocol::State>(result.response);
    require(state.percentage == 100);
    require(state.status == protocol::ChargeStatus::charging);
    require(state.present == 1);
    require(state.externalPower == 1);
}

void refusesMalformedRequestsWithoutQueryingTheHost()
{
    Fixture fixture;
    const protocol::Request wrongVersion {.version = protocol::version + 1};
    const auto wrongQueue = fixture.device->processQueue(
        1, 3, bytes(wrongVersion), sizeof(protocol::State));
    const auto shortResponse = fixture.device->processQueue(
        protocol::statusQueue, 4, bytes(wrongVersion), 1);
    require(wrongQueue.action == uml::virtio::VhostUserQueueAction::complete);
    require(shortResponse.action == uml::virtio::VhostUserQueueAction::complete);
    require(wrongQueue.response.empty());
    require(shortResponse.response.empty());
    require(fixture.provider->queries == 0);
}

} // namespace

int main()
{
    exposesOneStatusQueueWithoutFeatures();
    reportsHostPercentageAndChargingState();
    preservesTheBatterylessHostFallback();
    refusesMalformedRequestsWithoutQueryingTheHost();
}
