// SPDX-License-Identifier: GPL-2.0-or-later
/* The limbo both gadget engines' shared address spaces keep their unpublished
 * bodies in, on its own: a body is freed only once every executor that was
 * inside translated code when it was unpublished has left, and running out of
 * memory part way only delays that. The engines' use of it is tested with each
 * engine. */
#include "cpu/gadget_shared_space.hpp"

#include "allocation_failure.hpp"
#include "require.hpp"

#include <cstddef>
#include <new>
#include <vector>

namespace cache = uml::cpu::gadget_cache;
using uml::testing::require;

namespace {

struct Body final {
	unsigned frees{};
};

/* What an engine's free_bodies does, without allocating: the injector fails
 * the allocations of the limbo alone. */
void free_bodies(std::vector<Body *> &bodies) noexcept
{
	for (auto *body : bodies)
		++body->frees;
	bodies.clear();
}

/* Executors attached to one address space: the reclaimer, idle ones and one
 * inside translated code, attached last so that a snapshot that stops part
 * way misses it. */
struct Executors final {
	static constexpr std::size_t idle_count = 4;

	Executors()
	{
		cache::announce_entry(self);
		attached.attach(&self);
		for (auto &announcement : idle)
			attached.attach(&announcement);
		cache::announce_entry(reader);
		attached.attach(&reader);
	}

	cache::ExecutorAnnouncement self;
	cache::ExecutorAnnouncement idle[idle_count];
	cache::ExecutorAnnouncement reader;
	cache::AttachedExecutors attached;
};

/* A reclaim that cannot take its snapshot of the executors for want of memory
 * frees nothing early and throws nothing, whichever of its allocations fails:
 * the body still waits for the executor that was inside when it was
 * unpublished, and goes once that executor has left. */
void a_failed_snapshot_frees_nothing_early()
{
	for (std::size_t allowed = 0;; ++allowed) {
		require(allowed < 64, "the failure point never moved past the "
			"end of a reclaim");
		Executors executors;
		cache::Limbo<Body> limbo;
		Body body;
		limbo.push(&body);
		uml::testing::fail_after_allocations(allowed);
		bool threw = false;
		try {
			limbo.reclaim(&executors.self, executors.attached,
				free_bodies);
		} catch (const std::bad_alloc &) {
			threw = true;
		}
		/* True once the reclaim made fewer allocations than allowed. */
		const bool swept =
			uml::testing::cancel_pending_allocation_failure();
		limbo.reclaim(&executors.self, executors.attached, free_bodies);
		require(body.frees == 0, "a body was freed while an executor "
			"that was inside when it was unpublished still is");
		require(!threw, "a reclaim threw for want of memory");
		require(limbo.size() == 1, "a body left limbo unfreed");
		cache::announce_exit(executors.reader);
		limbo.reclaim(&executors.self, executors.attached, free_bodies);
		require(body.frees == 1 && limbo.size() == 0,
			"a body was not freed once every executor had left");
		if (swept)
			return;
	}
}

/* A body that has waited its turn is freed even when the snapshot for the
 * bodies unpublished after it cannot be taken, and those wait for the
 * executors inside when they were unpublished. */
void a_failed_snapshot_keeps_the_next_bodies_waiting()
{
	Executors executors;
	cache::Limbo<Body> limbo;
	Body first;
	limbo.push(&first);
	limbo.reclaim(&executors.self, executors.attached, free_bodies);
	require(first.frees == 0);
	cache::announce_exit(executors.reader);
	/* More executors attach than the last snapshot had room for, so that
	 * taking the next one allocates, and the last of them enters. */
	cache::ExecutorAnnouncement late[8];
	for (auto &announcement : late)
		executors.attached.attach(&announcement);
	cache::announce_entry(late[7]);
	Body second;
	limbo.push(&second);
	uml::testing::fail_next_allocation();
	bool threw = false;
	try {
		limbo.reclaim(&executors.self, executors.attached, free_bodies);
	} catch (const std::bad_alloc &) {
		threw = true;
	}
	require(!uml::testing::cancel_pending_allocation_failure(),
		"the snapshot made no allocation");
	require(first.frees == 1, "a body whose executors had left was kept");
	limbo.reclaim(&executors.self, executors.attached, free_bodies);
	require(second.frees == 0, "a body was freed while an executor that "
		"was inside when it was unpublished still is");
	require(!threw, "a reclaim threw for want of memory");
	cache::announce_exit(late[7]);
	limbo.reclaim(&executors.self, executors.attached, free_bodies);
	require(second.frees == 1 && limbo.size() == 0,
		"a body was not freed once every executor had left");
}

/* A body the limbo has no memory to queue stays where it is, never freed by
 * the limbo, and the push throws nothing. */
void a_body_that_cannot_be_queued_is_never_freed()
{
	Executors executors;
	cache::announce_exit(executors.reader);
	cache::Limbo<Body> limbo;
	Body body;
	uml::testing::fail_next_allocation();
	bool threw = false;
	try {
		limbo.push(&body);
	} catch (const std::bad_alloc &) {
		threw = true;
	}
	require(!uml::testing::cancel_pending_allocation_failure(),
		"the push made no allocation");
	require(!threw, "a push threw for want of memory");
	limbo.reclaim(&executors.self, executors.attached, free_bodies);
	require(body.frees == 0 && limbo.size() == 0);
}

} // namespace

int main()
{
	a_failed_snapshot_frees_nothing_early();
	a_failed_snapshot_keeps_the_next_bodies_waiting();
	a_body_that_cannot_be_queued_is_never_freed();
	return 0;
}
