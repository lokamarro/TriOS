/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <uml/emulator.h>

#include <string.h>

/* Packages are built without the embedded-kernel runner, so the consumer
 * checks the package's API on a handle without a kernel: it reports that it
 * cannot run and refuses a run. */
int main(void)
{
	struct uml_emulator *emulator = 0;
	const struct uml_emulator_config configuration = {
		.abi_version = UML_EMULATOR_ABI_VERSION,
		.struct_size = sizeof(struct uml_emulator_config),
	};
	if (uml_emulator_create(&configuration, &emulator) != UML_EMULATOR_OK)
		return 1;
	struct uml_emulator_capabilities capabilities = {
		.struct_size = sizeof(struct uml_emulator_capabilities),
	};
	const int status = uml_emulator_get_capabilities(
		emulator, &capabilities);
	const char *const arguments[] = {"uml-package-consumer", "--help"};
	const int run_status = uml_emulator_run(emulator, 2, arguments, 0);
	/* A run that never entered a kernel did not end a guest. */
	struct uml_emulator_run_end run_end = {
		.struct_size = sizeof(struct uml_emulator_run_end),
		.kind = UML_EMULATOR_RUN_END_FAILED,
		.restart_command = "stale",
	};
	const int run_end_status = uml_emulator_get_run_end(emulator, &run_end);
	/* A guest that does not run has no statistics and no console to ask. */
	struct uml_emulator_statistics statistics = {
		.struct_size = sizeof(struct uml_emulator_statistics),
	};
	const int statistics_status =
		uml_emulator_get_statistics(emulator, &statistics);
	struct uml_emulator_guest_memory guest_memory = {
		.struct_size = sizeof(struct uml_emulator_guest_memory),
	};
	const int guest_memory_status =
		uml_emulator_get_guest_memory(emulator, &guest_memory);
	const int release_status = uml_emulator_release_guest(emulator);
	uml_emulator_request_stop(emulator);
	uml_emulator_destroy(emulator);
	return status == UML_EMULATOR_OK &&
		run_status == -UML_EMULATOR_UNSUPPORTED &&
		run_end_status == UML_EMULATOR_OK &&
		release_status == -UML_EMULATOR_UNSUPPORTED &&
		statistics_status == -UML_EMULATOR_UNSUPPORTED &&
		guest_memory_status == -UML_EMULATOR_UNSUPPORTED &&
		run_end.kind == UML_EMULATOR_RUN_END_NONE &&
		run_end.restart_command[0] == '\0' &&
		capabilities.abi_version == UML_EMULATOR_ABI_VERSION &&
		!capabilities.can_run && capabilities.sandbox_safe &&
		capabilities.platform != 0 &&
		strcmp(capabilities.platform, UML_EXPECTED_PLATFORM) == 0 ? 0 : 1;
}
