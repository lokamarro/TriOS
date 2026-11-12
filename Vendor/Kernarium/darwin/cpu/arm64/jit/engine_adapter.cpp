/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cpu/arm64/engine_adapters.hpp"

#include "cpu/arm64/engine_execution.hpp"
#include "cpu/arm64/jit/a32/a32_loop.hpp"
#include "cpu/arm64/jit/jit_cache_geometry.hpp"
#include "cpu/arm64/jit/jit_executor.hpp"
#include "cpu/arm64/jit/jit_executor_pool.hpp"

#include <memory>

namespace uml::cpu::arm64::detail {
namespace {

class JitEngine final : public CpuEngine {
public:
	explicit JitEngine(EngineConfiguration configuration)
		: options_{configuration}, session_{active_jit_executor_pool_session()}
	{
		if (session_ != 0) {
			pool_configuration_admitted_ =
				bind_jit_executor_pool_configuration(session_, options_);
		} else {
			standalone_ = std::make_unique<JitExecutor>(options_);
		}
	}

	[[nodiscard]] EngineKind kind() const noexcept override
	{
		return EngineKind::jit;
	}

	[[nodiscard]] ExecutionResult run(ExecutionRequest &request) noexcept override
	{
		return with_executor(request, [](JitExecutor &executor,
						 ExecutionRequest &entry) {
			return run_translated(executor, entry);
		});
	}

	[[nodiscard]] ExecutionResult run_aarch32(
		ExecutionRequest &request) noexcept override
	{
		return with_executor(request, [](JitExecutor &executor,
						 ExecutionRequest &entry) {
			return jit::a32::run_a32_loop(executor, entry);
		});
	}

private:
	/* Runs `loop` over the request on this engine's executor: the standalone
	 * one, or one borrowed from the session's pool for the entry. */
	template <typename Loop>
	[[nodiscard]] ExecutionResult with_executor(ExecutionRequest &request,
						    Loop loop) noexcept
	{
		if (!native_jit_available())
			return {.reason = ExitReason::fatal};
		try {
			if (session_ == 0)
				return standalone_ == nullptr ?
					ExecutionResult{.reason = ExitReason::fatal} :
					loop(*standalone_, request);
			if (!pool_configuration_admitted_)
				return {.reason = ExitReason::fatal};
			auto borrowed = borrow_jit_executor(session_, preferred_slot_);
			if (!borrowed)
				return {.reason = ExitReason::fatal};
			preferred_slot_ = borrowed.slot();
			return loop(borrowed.executor(), request);
		} catch (...) {
			return {.reason = ExitReason::fatal};
		}
	}

	JitExecutorOptions options_;
	JitExecutorPoolSessionToken session_{};
	std::unique_ptr<JitExecutor> standalone_;
	unsigned preferred_slot_{maximum_jit_cache_executors};
	bool pool_configuration_admitted_{};
};

} // namespace

std::unique_ptr<CpuEngine> make_jit_engine(EngineConfiguration configuration)
{
	return std::make_unique<JitEngine>(configuration);
}

bool native_jit_engine_available() noexcept
{
	return native_jit_available();
}

} // namespace uml::cpu::arm64::detail
