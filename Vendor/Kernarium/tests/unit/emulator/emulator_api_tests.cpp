// SPDX-License-Identifier: GPL-2.0-or-later
#include "uml/emulator.h"
#include "allocation_failure.hpp"
#include "cpu/arm64/gadgets/sme_support.hpp"
#include "cpu/arm64/id_registers.hpp"
#include "cpu/arm64/jit/jit_sme_support.hpp"
#include "cpu/arm64/sme_profile.hpp"
#include "device/network/device.hpp"
#include "emulator_test_support.hpp"
#include "native_window_test_support.hpp"
#include "platform/apple/host_features.hpp"
#include "platform/run_routing.hpp"
#include "require.hpp"
#include "run_window_slot.hpp"
#include "runtime/context.hpp"
#include "scratch_backing.hpp"
#include "testing.hpp"
#include "uml_darwin_cpu.h"
#include "uml_darwin_network.h"
#include "uml_darwin_memory.h"
#include "uml_darwin_static_mmu.h"
#if UML_EMULATOR_TEST_HAS_SLIRP
#include "uml_slirp_net.h"
#endif

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

extern "C" int darwin_uml_net_open(unsigned char mac[6], unsigned int *mtu);
extern "C" int darwin_uml_net_irq_fd(void);
extern "C" int darwin_uml_net_read(void *buffer, unsigned long capacity);
extern "C" int darwin_uml_net_write(const void *buffer, unsigned long length);

namespace {

template<typename Provider>
concept HasStopCallback = requires(Provider provider) { provider.stop; };

static_assert(!HasStopCallback<uml::network::Provider>);

bool kernel_arguments_valid;
bool execution_option_was_stripped;
bool kernel_inputs_were_mutable;
unsigned long kernel_top_byte_ignore;
#if UML_EMULATOR_TEST_HAS_SLIRP
bool network_kernel_ran;
bool ip_argument_present;
#endif

struct NetworkState final {
	int writes{};
	unsigned int mtu{1500};
};


struct NativeShadowKernelState final {
	std::uintptr_t address{};
	int fd{-1};
	std::uint8_t expected{};
	std::uint8_t replacement{};
	int status{};
	bool valid{};
};

NativeShadowKernelState native_shadow_a;
NativeShadowKernelState native_shadow_b;

using uml::testing::require;

void test_c_api_rejects_inconsistent_creation_inputs()
{
	auto *stale = reinterpret_cast<uml_emulator *>(std::uintptr_t{1});
	require(uml_emulator_create(nullptr, &stale) ==
			-UML_EMULATOR_INVALID_ARGUMENT && stale == nullptr,
		"failed creation did not clear the caller's output handle");

	const uml_emulator_config inconsistent_image{
		.abi_version = UML_EMULATOR_ABI_VERSION,
		.struct_size = sizeof(uml_emulator_config),
		.rootfs_path = "/tmp/rootfs.erofs",
		.kernel_image = nullptr,
		.kernel_image_size = 1,
	};
	stale = reinterpret_cast<uml_emulator *>(std::uintptr_t{1});
	require(uml_emulator_create(&inconsistent_image, &stale) ==
			-UML_EMULATOR_INVALID_ARGUMENT && stale == nullptr,
		"null kernel image with a positive size was accepted");
}

void test_memory_bridge_rejects_split_address_space_ranges()
{
	constexpr std::uintptr_t address_limit =
		DARWIN_UML_NATIVE_ADDRESS_START;
	constexpr std::uintptr_t low_page =
		address_limit - DARWIN_UML_GUEST_PAGE_SIZE;
	constexpr unsigned long split_length = 2 * DARWIN_UML_GUEST_PAGE_SIZE;
	const auto *address = reinterpret_cast<void *>(low_page);
	const auto rejected = [](int result) {
		return result == -1 && errno == EINVAL;
	};

	errno = 0;
	require(rejected(darwin_uml_vm_map(const_cast<void *>(address), -1, 0,
		split_length, PROT_READ, 0, 0)),
		"split static/native map was routed to one address-space implementation");
	errno = 0;
	require(rejected(darwin_uml_vm_map_generation(17,
		const_cast<void *>(address), -1, 0, split_length,
		PROT_READ, 0, 0)),
		"split generation map was routed to one address-space implementation");
	errno = 0;
	require(rejected(darwin_uml_vm_protect(
		const_cast<void *>(address), split_length, PROT_READ)),
		"split protect was routed to one address-space implementation");
	errno = 0;
	require(rejected(darwin_uml_vm_unmap(
		const_cast<void *>(address), split_length)),
		"split unmap was routed to one address-space implementation");
	errno = 0;
	require(rejected(darwin_uml_vm_unmap_generation(
		17, const_cast<void *>(address), split_length)),
		"split generation unmap was routed to one address-space implementation");
	errno = 0;
	require(rejected(darwin_uml_vm_drop(
		const_cast<void *>(address), split_length)),
		"split drop was silently accepted");
	errno = 0;
	require(rejected(darwin_uml_vm_sync_from_backing(address_limit - 1)),
		"split backing refresh was silently accepted");
	errno = 0;
	require(rejected(darwin_uml_vm_sync_from_buffer(
		low_page, nullptr, split_length)),
		"split copy range was not rejected before address-space access");
}

[[nodiscard]] uml::runtime::ExecutionEngine current_execution_engine() noexcept
{
	const auto *context = uml::runtime::current_context();
	return context == nullptr ? uml::runtime::ExecutionEngine::automatic :
		context->execution_engine();
}


using uml::testing::ScratchBacking;

constexpr std::string_view backing_name = "uml-emulator-native-shadow-XXXXXX";

using uml::testing::RunWindowSlot;

int native_shadow_kernel(NativeShadowKernelState &state)
{
	state.valid = darwin_uml_vm_map(
		reinterpret_cast<void *>(state.address), state.fd, 0,
		DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE, 0, 1) == 0;
	if (state.valid)
		state.valid = *reinterpret_cast<volatile std::uint8_t *>(
			state.address) == state.expected;
	if (state.valid)
		*reinterpret_cast<volatile std::uint8_t *>(state.address) =
			state.replacement;
	return state.status;
}

int native_shadow_kernel_a(int, char *const[], char *const[])
{
	return native_shadow_kernel(native_shadow_a);
}

int native_shadow_kernel_b(int, char *const[], char *const[])
{
	return native_shadow_kernel(native_shadow_b);
}


int run_kernel(int argc, char *const argv[], char *const envp[])
{
	const auto arguments = std::span{argv, static_cast<std::size_t>(argc)};
	const bool rootfs = std::ranges::any_of(arguments, [](const char *argument) {
		return std::strcmp(argument, "ubd0c=/tmp/rootfs.erofs") == 0;
	});
	const bool root = std::ranges::any_of(arguments, [](const char *argument) {
		return std::strcmp(argument, "root=/dev/ubda") == 0;
	});
	const bool root_type = std::ranges::any_of(
		arguments, [](const char *argument) {
			return std::strcmp(argument, "rootfstype=erofs") == 0;
		});
	const bool environment = envp != nullptr && envp[0] != nullptr &&
		std::strcmp(envp[0], "UML_TEST=1") == 0;
	execution_option_was_stripped = std::ranges::none_of(
		arguments, [](const char *argument) {
			return std::string_view{argument}.starts_with(
				"--execution-engine");
		});
	kernel_arguments_valid = rootfs && root && root_type && environment;
	unsigned char mac[6]{};
	unsigned int mtu{};
	const unsigned char frame[4]{1, 2, 3, 4};
	kernel_arguments_valid = kernel_arguments_valid &&
		darwin_uml_net_open(mac, &mtu) == 0 && mtu == 1500 &&
		darwin_uml_net_irq_fd() == 42 &&
		darwin_uml_net_read(nullptr, 0) == 0 &&
		darwin_uml_net_write(frame, sizeof(frame)) == 0;
	kernel_top_byte_ignore = darwin_uml_guest_top_byte_ignore();
	argv[0][0] = 'U';
	envp[0][0] = 'u';
	kernel_inputs_were_mutable = argv[0][0] == 'U' && envp[0][0] == 'u';
	return 73;
}

bool current_stop_requested()
{
	const auto *context = ::uml::runtime::current_context();
	return context != nullptr && context->stop_requested();
}

int network_open(void *, uint8_t mac[6], unsigned int *mtu)
{
	std::fill_n(mac, 6, uint8_t{0x52});
	*mtu = 1500;
	return 0;
}

int instance_network_open(void *context, uint8_t mac[6], unsigned int *mtu)
{
	const auto &state = *static_cast<NetworkState *>(context);
	std::fill_n(mac, 6, uint8_t{0x52});
	*mtu = state.mtu;
	return 0;
}

int network_irq_fd(void *) { return 42; }
int network_read(void *, void *, size_t) { return 0; }

int network_write(void *context, const void *, size_t)
{
	static_cast<NetworkState *>(context)->writes++;
	return 0;
}


#if UML_EMULATOR_TEST_HAS_SLIRP
int network_kernel(int argc, char *const argv[], char *const[])
{
	const auto arguments = std::span{argv, static_cast<std::size_t>(argc)};
	network_kernel_ran = true;
	ip_argument_present = std::ranges::any_of(
		arguments, [](const char *argument) {
			return std::string_view{argument}.starts_with("ip=");
		});
	return 0;
}
#endif

using uml::testing::create_emulator;
using uml::testing::make_emulator;

void test_capabilities_describe_the_kernel()
{
	const auto empty = create_emulator({
		.abi_version = UML_EMULATOR_ABI_VERSION,
		.struct_size = sizeof(uml_emulator_config),
	});
	uml_emulator_capabilities capabilities{
		.struct_size = sizeof(uml_emulator_capabilities),
	};
	require(uml_emulator_get_capabilities(empty.get(), &capabilities) ==
			UML_EMULATOR_OK &&
		capabilities.abi_version == UML_EMULATOR_ABI_VERSION &&
		capabilities.can_run == 0 && capabilities.sandbox_safe == 1 &&
		capabilities.platform != nullptr,
		"a handle without a kernel misreported its capabilities");
	const char *arguments[]{"uml-test"};
	require(uml_emulator_run(empty.get(), 1, arguments, nullptr) ==
			-UML_EMULATOR_UNSUPPORTED,
		"a handle without a kernel did not refuse to run");

	const auto embedded = make_emulator(run_kernel);
	require(uml_emulator_get_capabilities(embedded.get(), &capabilities) ==
			UML_EMULATOR_OK &&
		capabilities.can_run == UML_EMULATOR_TEST_HAS_KERNEL_RUNNER &&
		capabilities.sandbox_safe == !UML_EMULATOR_TEST_HAS_KERNEL_RUNNER,
		"an embedded kernel image misreported its capabilities");
}

bool stop_seen;

int stop_sampling_kernel(int, char *const[], char *const[])
{
	stop_seen = current_stop_requested();
	return 0;
}

uml_emulator *self_stopping_handle;
unsigned int self_stopping_entries;

/* The first run stops itself; later runs only sample. */
int self_stopping_kernel(int, char *const[], char *const[])
{
	const bool before = current_stop_requested();
	if (self_stopping_entries++ != 0) {
		stop_seen = before;
		return 0;
	}
	uml_emulator_request_stop(self_stopping_handle);
	stop_seen = !before && current_stop_requested();
	return 0;
}

/* A stop request reaches the run it overlaps and no later one. */
void test_stop_requests_belong_to_one_run()
{
	const auto sampling = make_emulator(stop_sampling_kernel);
	const char *arguments[]{"uml-test", "--execution-engine=gadgets"};
	uml_emulator_request_stop(sampling.get());
	stop_seen = true;
	require(uml_emulator_run(sampling.get(), 2, arguments, nullptr) == 0 &&
			!stop_seen,
		"a run inherited a stop requested while its handle was idle");

	const auto stopping = make_emulator(self_stopping_kernel);
	self_stopping_handle = stopping.get();
	stop_seen = false;
	require(uml_emulator_run(stopping.get(), 2, arguments, nullptr) == 0 &&
			stop_seen,
		"a stop requested during a run did not reach that run");
	stop_seen = true;
	require(uml_emulator_run(stopping.get(), 2, arguments, nullptr) == 0 &&
			!stop_seen,
		"a later run inherited an earlier run's stop");
	self_stopping_handle = nullptr;
}

void test_run_boundary()
{
	/* The guest kernel reads guest_hwcaps() of the probed host, the words
	 * arm64-advertised-features pins, before and after a run selects an
	 * engine: outside a run, as both engines present it
	 * (sme_host_for_support). */
	namespace arm64 = ::uml::cpu::arm64;
	const auto expected = arm64::detail::guest_hwcaps(
		arm64::sme_host_for_support(
			::uml::platform::apple::probed_host_capabilities(),
			std::min(arm64::gadgets::sme_support,
				arm64::jit::sme_support)));
	require(darwin_uml_guest_top_byte_ignore() == 0,
		"top-byte-ignore was reported outside a run");
	require(darwin_uml_guest_hwcap() == expected[0],
		"guest HWCAP does not match the host-safe virtual CPU");
	require(darwin_uml_guest_hwcap2() == expected[1],
		"guest HWCAP2 does not match the host-safe virtual CPU");

	NetworkState network_state;
	uml_emulator_network_callbacks network{
		.struct_size = sizeof(uml_emulator_network_callbacks),
		.context = &network_state,
		.open = network_open,
		.irq_fd = network_irq_fd,
		.read = network_read,
		.write = network_write,
	};
	char rootfs_path[]{"/tmp/rootfs.erofs"};
	auto image = uml::emulator::testing::kernel_image(run_kernel);
	uml_emulator_config config{
		.abi_version = UML_EMULATOR_ABI_VERSION,
		.struct_size = sizeof(uml_emulator_config),
		.rootfs_path = rootfs_path,
		.kernel_image = image.data(),
		.kernel_image_size = image.size(),
		.network = &network,
	};
	const auto emulator = create_emulator(config);
	rootfs_path[0] = 'X';
	std::ranges::fill(image, std::byte{0});
	network.open = nullptr;
	config.rootfs_path = nullptr;
	config.kernel_image = nullptr;
	config.kernel_image_size = 0;
	config.network = nullptr;
	uml_emulator_capabilities capabilities{
		.struct_size = sizeof(uml_emulator_capabilities),
	};
	require(uml_emulator_get_capabilities(emulator.get(), &capabilities) ==
		UML_EMULATOR_OK, "capability query failed");
	require(capabilities.userspace_execution == UML_EMULATOR_NATIVE_JIT,
		"macOS did not default to the native JIT");
	char argument_zero[]{"uml-test"};
	char argument_one[]{"--execution-engine=gadgets"};
	char argument_two[]{"--jit-cache-budget=1G"};
	char argument_three[]{"ncpus=1"};
	char environment_zero[]{"UML_TEST=1"};
	const char *arguments[]{
		argument_zero, argument_one, argument_two, argument_three};
	const char *environment[]{environment_zero, nullptr};
	require(uml_emulator_run(emulator.get(), 4, arguments, environment) == 73,
		"kernel result was not returned");
	require(kernel_inputs_were_mutable &&
		std::strcmp(argument_zero, "uml-test") == 0 &&
		std::strcmp(argument_one, "--execution-engine=gadgets") == 0 &&
		std::strcmp(argument_two, "--jit-cache-budget=1G") == 0 &&
		std::strcmp(argument_three, "ncpus=1") == 0 &&
		std::strcmp(environment_zero, "UML_TEST=1") == 0,
		"run did not isolate caller-owned argument/environment buffers");
	require(uml_emulator_get_capabilities(emulator.get(), &capabilities) ==
		UML_EMULATOR_OK &&
		capabilities.userspace_execution == UML_EMULATOR_STATIC_GADGETS,
		"handle-local gadget selection was not retained");
	require(darwin_uml_guest_hwcap() == expected[0] &&
			darwin_uml_guest_hwcap2() == expected[1],
		"gadget selection changed the virtual CPU contract");
	require(execution_option_was_stripped,
		"host execution option leaked into kernel arguments");
	require(kernel_arguments_valid,
		"kernel arguments or network callbacks were invalid");
	require(network_state.writes == 1,
		"network callback was not invoked exactly once");
	require(kernel_top_byte_ignore == 1,
		"a run without a top-byte-ignore option did not report it");
	/* The kernel decides at boot whether to offer the tagged-address ABI
	 * from the setting, which is the run's own and is consumed before the
	 * kernel sees its arguments. */
	const char *tbi_arguments[]{
		"uml-test", "--execution-engine=gadgets", "--guest-no-top-byte-ignore"};
	require(uml_emulator_run(
			emulator.get(), 3, tbi_arguments, environment) == 73,
		"a run with the top-byte-ignore off switch did not enter the kernel");
	require(kernel_top_byte_ignore == 0,
		"--guest-no-top-byte-ignore did not reach the kernel's query");
	/* The gadget run's deliberately different JIT budget/CPU pair must not
	 * freeze process-global JIT geometry. A later default JIT run publishes
	 * its own pair and enters normally.
	 */
	const char *jit_arguments[]{
		"uml-test", "--execution-engine=jit"};
	require(uml_emulator_run(
			emulator.get(), 2, jit_arguments, environment) == 73,
		"a gadget-only configuration froze later JIT geometry");
	require(network_state.writes == 3 && kernel_top_byte_ignore == 1,
		"the sequential JIT run did not enter the kernel, or kept "
		"the previous run's top-byte-ignore off switch");

	const char *invalid_arguments[]{
		"uml-test", "--execution-engine=invalid"};
	require(uml_emulator_run(
			emulator.get(), 2, invalid_arguments, environment) ==
		-UML_EMULATOR_INVALID_ARGUMENT,
		"invalid execution engine was accepted");
	const char *invalid_fallback[]{
		"uml-test", "--jit-gadget-fallback=off"};
	require(uml_emulator_run(
			emulator.get(), 2, invalid_fallback, environment) ==
		-UML_EMULATOR_INVALID_ARGUMENT,
		"removed JIT gadget fallback option was accepted");
	const char *below_minimum_budget[]{
		"uml-test", "--execution-engine=jit", "--jit-cache-budget=1K"};
	require(uml_emulator_run(emulator.get(), 3, below_minimum_budget,
			environment) == -UML_EMULATOR_RUNTIME_ERROR,
		"below-minimum JIT budget did not fail launch clearly");
	const char *incompatible_budget[]{
		"uml-test", "--execution-engine=jit", "--jit-cache-budget=1G"};
	require(uml_emulator_run(emulator.get(), 3, incompatible_budget,
			environment) == -UML_EMULATOR_RUNTIME_ERROR,
		"sequentially incompatible JIT budget did not fail launch clearly");
}


void test_native_shadow_domains_retire_on_every_handle_exit(
	RunWindowSlot &slot)
{
	ScratchBacking backing_a{backing_name, DARWIN_UML_GUEST_PAGE_SIZE};
	backing_a.put_byte(0, std::byte{0x31});
	ScratchBacking backing_b{backing_name, DARWIN_UML_GUEST_PAGE_SIZE};
	backing_b.put_byte(0, std::byte{0xa7});
	native_shadow_a = {
		.address = slot.page(),
		.fd = backing_a.descriptor(),
		.expected = 0x31,
		.replacement = 0x42,
		.status = 0,
	};
	native_shadow_b = {
		.address = slot.page(),
		.fd = backing_b.descriptor(),
		.expected = 0xa7,
		.replacement = 0xb8,
		.status = 37,
	};
	const auto handle_a = make_emulator(native_shadow_kernel_a);
	const auto handle_b = make_emulator(native_shadow_kernel_b);
	const char *arguments[]{"native-shadow-domain"};
	require(uml_emulator_run(handle_a.get(), 1, arguments, nullptr) == 0 &&
		native_shadow_a.valid,
		"normal native-shadow run failed");
	require(slot.retired(),
		"normal run retained native-shadow mapping or protection state");
	std::uint8_t backing_value{};
	require(pread(backing_a.descriptor(), &backing_value, sizeof(backing_value), 0) ==
			static_cast<ssize_t>(sizeof(backing_value)) &&
		backing_value == 0x42,
		"normal native-shadow retirement did not flush backing state");

	require(uml_emulator_run(handle_b.get(), 1, arguments, nullptr) == 37 &&
		native_shadow_b.valid,
		"abnormal-return native-shadow run failed");
	require(slot.retired(),
		"abnormal return retained native-shadow state");
	require(pread(backing_b.descriptor(), &backing_value, sizeof(backing_value), 0) ==
			static_cast<ssize_t>(sizeof(backing_value)) &&
		backing_value == 0xb8,
		"abnormal native-shadow retirement did not flush backing state");

	native_shadow_b.expected = 0xb8;
	native_shadow_b.replacement = 0xc9;
	native_shadow_b.status = 0;
	native_shadow_b.valid = false;
	require(uml_emulator_run(handle_b.get(), 1, arguments, nullptr) == 0 &&
		native_shadow_b.valid && slot.retired(),
		"handle could not retry with a fresh native-shadow domain");
}


void test_active_registries_are_allocation_free_and_transactional()
{
	NetworkState state_a{.mtu = 1701};
	NetworkState state_b{.mtu = 1702};
	uml::network::ProviderContext provider_a{{
		.context = &state_a,
		.open = instance_network_open,
		.irq_fd = network_irq_fd,
		.read = network_read,
		.write = network_write,
	}};
	uml::network::ProviderContext provider_b{{
		.context = &state_b,
		.open = instance_network_open,
		.irq_fd = network_irq_fd,
		.read = network_read,
		.write = network_write,
	}};
	uml::runtime::Context context_a;
	uml::runtime::Context context_b;
	require(context_a.set_execution_engine(
			uml::runtime::ExecutionEngine::gadgets) == 0,
		"could not select registry test engine");
	uml::runtime::RunScope run_a{context_a};
	require(run_a.entered(), "registry test run was not admitted");
	uml::routing::ActiveRouteScope active_a{&context_a, &provider_a};

	const auto a_is_still_unbound_fallback = [&] {
		bool valid = false;
		std::thread sample{[&] {
			unsigned char mac[6]{};
			unsigned int mtu{};
			valid = current_execution_engine() ==
					uml::runtime::ExecutionEngine::gadgets &&
				darwin_uml_net_open(mac, &mtu) == 0 && mtu == 1701;
		}};
		sample.join();
		return valid;
	};
	require(a_is_still_unbound_fallback(),
		"handle A was not the initial unbound fallback");

	/* The route reservation used by Emulator::run() is allocation-free. A
	 * pending pair stays invisible and rolls back without mutating the
	 * established one-run fallback. */
	uml::testing::fail_next_allocation();
	{
		uml::routing::ActiveRouteScope pending_b{
			&context_b, &provider_b,
			uml::routing::ActiveRouteScope::DeferredPublication{}};
		require(uml::testing::cancel_pending_allocation_failure(),
			"route reservation unexpectedly allocated");
	}
	require(a_is_still_unbound_fallback(),
		"pending route did not roll back exactly");

	require(context_b.set_execution_engine(
			uml::runtime::ExecutionEngine::gadgets) == 0,
		"could not select retry engine for context B");
	{
		uml::routing::ActiveRouteScope retry_b{
			&context_b, &provider_b,
			uml::routing::ActiveRouteScope::DeferredPublication{}};
		retry_b.commit();
		unsigned char mac[6]{};
		unsigned int mtu{};
		require(current_execution_engine() ==
				uml::runtime::ExecutionEngine::gadgets &&
			darwin_uml_net_open(mac, &mtu) == 0 && mtu == 1702,
			"context/provider B could not retry after rollback");

		bool unbound_failed_closed = false;
		std::thread sample{[&] {
			unsigned char unbound_mac[6]{};
			unsigned int unbound_mtu{};
			unbound_failed_closed =
				current_execution_engine() ==
					uml::runtime::ExecutionEngine::automatic &&
				darwin_uml_net_open(unbound_mac, &unbound_mtu) ==
					-ENETDOWN;
		}};
		sample.join();
		require(unbound_failed_closed,
			"committed overlap did not fail closed");
	}
	require(!a_is_still_unbound_fallback(),
		"committed overlap incorrectly restored A's fallback");
}

#if UML_EMULATOR_TEST_HAS_SLIRP
/* No maintained kernel builds IP_PNP, so a user network leaves addressing to
 * guest userspace and adds nothing to the kernel command line. */
void test_injected_user_network_adds_no_kernel_ip_configuration()
{
	void *network_context = uml_slirp_net_create();
	require(network_context != nullptr,
		"injected user-network creation failed");
	const std::unique_ptr<void, decltype(&uml_slirp_net_destroy)> network{
		network_context, uml_slirp_net_destroy};
	const uml_emulator_network_callbacks callbacks{
		.struct_size = sizeof(uml_emulator_network_callbacks),
		.context = network.get(),
		.open = uml_slirp_net_open,
		.irq_fd = uml_slirp_net_irq_fd,
		.read = uml_slirp_net_read,
		.write = uml_slirp_net_write,
	};
	const auto emulator = make_emulator(network_kernel, &callbacks);
	const char *arguments[]{"uml-test"};
	require(uml_emulator_run(emulator.get(), 1, arguments, nullptr) == 0,
		"injected-network emulator run failed");
	uml_slirp_net_stop(network.get());
	require(network_kernel_ran,
		"injected-network emulator never entered its kernel");
	require(!ip_argument_present,
		"injected user network added kernel IP autoconfiguration");
}
#endif

} // namespace

int main()
{
	try {
		/* Runs take the test's native window: the product's on an
		 * ordinary build, one the sanitizer leaves free on a sanitized
		 * build. */
		uml::emulator::testing::place_native_kernel_window(
			uml::testing::native_test_window_start);
		test_c_api_rejects_inconsistent_creation_inputs();
		test_memory_bridge_rejects_split_address_space_ranges();
		test_capabilities_describe_the_kernel();
		test_stop_requests_belong_to_one_run();
		test_run_boundary();
		{
			RunWindowSlot native_shadow_slot;
			test_native_shadow_domains_retire_on_every_handle_exit(
				native_shadow_slot);
		}
		test_active_registries_are_allocation_free_and_transactional();
#if UML_EMULATOR_TEST_HAS_SLIRP
		test_injected_user_network_adds_no_kernel_ip_configuration();
#endif
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::fprintf(stderr, "emulator API test failed: %s\n", error.what());
		return EXIT_FAILURE;
	} catch (...) {
		std::fprintf(stderr, "emulator API test failed: unknown exception\n");
		return EXIT_FAILURE;
	}
}
