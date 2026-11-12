// SPDX-License-Identifier: GPL-2.0-or-later

#include "forwarder.hpp"
#include "named_tests.hpp"
#include "vsock_test_guest.hpp"
#include "watchdog.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>

namespace {

using namespace uml::vsock;
using namespace uml::vsock::testing;

std::optional<ForwardSpecification> parse(std::string_view text)
{
    std::string error;
    auto result = parseForward(text, error);
    require(result.has_value() == error.empty());
    return result;
}

/* The guest's endpoint, borrowed for a forwarder that must not own it. */
std::shared_ptr<Endpoint> borrowed(Guest& guest)
{
    return std::shared_ptr<Endpoint>(&guest.endpoint(), [](Endpoint*) {});
}

ForwardSpecification forward(std::uint16_t hostPort, std::uint32_t guestPort)
{
    return {
        .hostAddress = "127.0.0.1",
        .hostPort = hostPort,
        .guestPort = guestPort,
    };
}

void parsesTheForwardGrammar()
{
    const auto plain = parse("5555-5000");
    require(plain.has_value());
    require(plain->hostAddress == "127.0.0.1" && plain->hostPort == 5555 &&
        plain->guestPort == 5000);
    const auto chosen = parse("127.0.0.1:0-5555");
    require(chosen.has_value() && chosen->hostPort == 0 && chosen->guestPort == 5555);
    const auto other = parse("127.0.0.2:8000-1");
    require(other.has_value() && other->hostAddress == "127.0.0.2");
    const auto high = parse("65535-4294967294");
    require(high.has_value() && high->guestPort == 4294967294U);
}

void refusesWhatIsNotLoopback()
{
    for (const char* text : {
             "0.0.0.0:1-2", "192.0.2.1:1-2", "203.0.113.9:1-2", "localhost:1-2",
             "[::1]:1-2", "::1:1-2", "8.8.8.8:0-5555"}) {
        std::string error;
        require(!parseForward(text, error).has_value(), text);
        require(error.find("loopback") != std::string::npos, text);
    }
}

void refusesMalformedForwards()
{
    for (const char* text : {
             "", "abc", "5555", "5555-", "-5555", "70000-1", "1-0", "1-4294967295",
             "1-4294967296", "x-1", "1-x", "1:2-3-4", "127.0.0.1:-5", "-1-5"}) {
        std::string error;
        require(!parseForward(text, error).has_value(), text);
        require(!error.empty());
    }
}

int connectLoopback(std::uint16_t port)
{
    const int descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
    require(descriptor >= 0);
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_len = sizeof(address);
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(::connect(descriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    return descriptor;
}

bool readable(int descriptor, int milliseconds)
{
    pollfd waiting {.fd = descriptor, .events = POLLIN, .revents = 0};
    return ::poll(&waiting, 1, milliseconds) > 0;
}

void forwardsAConnectionToTheGuestPort()
{
    uml::testing::Watchdog watchdog("forwarder", std::chrono::seconds(60));
    Guest guest;
    Forwarder forwarder(borrowed(guest), {forward(0, 5555)});
    require(forwarder.port(0) != 0);
    const int client = connectLoopback(forwarder.port(0));
    const auto request = guest.expect();
    require(request.header.op == protocol::opRequest);
    require(request.header.destinationPort == 5555);
    guest.transmit(guestHeader(
        protocol::opResponse, 5555, request.header.sourcePort));
    const char hello[] = "hello guest";
    require(::send(client, hello, sizeof(hello), 0) == static_cast<ssize_t>(sizeof(hello)));
    const auto data = guest.expect();
    require(data.header.op == protocol::opReadWrite);
    require(data.payload.size() == sizeof(hello));
    const char reply[] = "hello host";
    guest.transmit(
        guestHeader(
            protocol::opReadWrite, 5555, request.header.sourcePort, sizeof(reply)),
        std::span(reinterpret_cast<const std::uint8_t*>(reply), sizeof(reply)));
    char received[64] {};
    require(readable(client, 5000));
    require(::recv(client, received, sizeof(received), 0) == static_cast<ssize_t>(sizeof(reply)));
    require(std::strcmp(received, reply) == 0);
    /* The client closes, and the guest is told the stream ended. */
    ::close(client);
    const auto shutdown = guest.expectNotCredit();
    require(shutdown.header.op == protocol::opShutdown);
}

void closesAClientTheGuestRefuses()
{
    uml::testing::Watchdog watchdog("forwarder refusal", std::chrono::seconds(60));
    Guest guest;
    Forwarder forwarder(borrowed(guest), {forward(0, 7)});
    const int client = connectLoopback(forwarder.port(0));
    const auto request = guest.expect();
    guest.transmit(guestHeader(protocol::opReset, 7, request.header.sourcePort));
    guest.pump();
    require(readable(client, 5000));
    char byte = 0;
    require(::recv(client, &byte, 1, 0) <= 0);
    ::close(client);
}

void stopsPromptlyWhileAConnectionWaitsForTheGuest()
{
    uml::testing::Watchdog watchdog("forwarder stop", std::chrono::seconds(60));
    Guest guest;
    const auto started = std::chrono::steady_clock::now();
    int client = -1;
    {
        Forwarder forwarder(borrowed(guest), {forward(0, 9)});
        client = connectLoopback(forwarder.port(0));
        /* The guest never answers. */
        const auto request = guest.expect();
        require(request.header.op == protocol::opRequest);
    }
    require(std::chrono::steady_clock::now() - started < std::chrono::seconds(5));
    ::close(client);
}

void refusesAPortInUse()
{
    Guest guest;
    Forwarder first(borrowed(guest), {forward(0, 1)});
    bool refused = false;
    try {
        Forwarder second(borrowed(guest), {forward(first.port(0), 2)});
    } catch (const std::runtime_error&) {
        refused = true;
    }
    require(refused);
}

} // namespace

int main()
{
    static constexpr uml::testing::NamedTest tests[] = {
        {"parses-the-forward-grammar", parsesTheForwardGrammar},
        {"refuses-what-is-not-loopback", refusesWhatIsNotLoopback},
        {"refuses-malformed-forwards", refusesMalformedForwards},
        {"forwards-a-connection-to-the-guest-port", forwardsAConnectionToTheGuestPort},
        {"closes-a-client-the-guest-refuses", closesAClientTheGuestRefuses},
        {"stops-promptly-while-a-connection-waits-for-the-guest",
         stopsPromptlyWhileAConnectionWaitsForTheGuest},
        {"refuses-a-port-in-use", refusesAPortInUse},
    };
    return uml::testing::runNamedTests(tests);
}
