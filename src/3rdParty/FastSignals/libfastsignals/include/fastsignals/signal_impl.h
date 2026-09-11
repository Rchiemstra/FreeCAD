#pragma once

#include "function_detail.h"
#include "spin_mutex.h"
#include <algorithm>
#include <exception>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fastsignals::detail
{

struct resilient_slot_key
{
	const void* signal = nullptr;
	uint64_t slotId = 0;

	bool operator==(const resilient_slot_key&) const noexcept = default;
};

struct resilient_slot_key_hash
{
	size_t operator()(const resilient_slot_key& key) const noexcept
	{
		return std::hash<const void*>{}(key.signal)
			^ (std::hash<uint64_t>{}(key.slotId) << 1);
	}
};

struct resilient_slot_authority
{
	std::mutex copyMutex;
	packed_function callable;
};

template <class Signature>
struct resilient_slot_registry
{
	std::mutex mutex;
	std::unordered_map<resilient_slot_key,
		std::weak_ptr<resilient_slot_authority>,
		resilient_slot_key_hash> slots;
	// A signal_impl cannot erase its own entries: it is not templated on the
	// signature, so it cannot reach the per-signature registries, and adding a
	// member or virtual to reach them would break the historical layout that
	// statically linked extension binaries depend on. Entries for destroyed
	// signals therefore linger as expired weak references. Sweep them under
	// the lock we already hold, on a threshold that keeps the amortized cost
	// proportional to live slots.
	size_t pruneThreshold = 64;

	void prune_expired_locked()
	{
		if (slots.size() <= pruneThreshold)
		{
			return;
		}
		std::erase_if(slots, [](const auto& entry) noexcept {
			return entry.second.expired();
		});
		pruneThreshold = (std::max)(static_cast<size_t>(64), slots.size() * 2);
	}

	static resilient_slot_registry& instance()
	{
		// Signals can be emitted during process-static teardown. Keep the weak
		// registry alive for the process lifetime to avoid destruction-order
		// dependencies; it owns no callable strongly.
		static auto* registry = new resilient_slot_registry;
		return *registry;
	}
};

template <class Signature>
struct resilient_slot_wrapper;

template <class Return, class... Arguments>
struct resilient_slot_wrapper<Return(Arguments...)>
{
	static packed_function make(std::shared_ptr<resilient_slot_authority> retained)
	{
		auto forwarder = [retained = std::move(retained)](Arguments... args) -> Return {
			// Historical ordinary emission cloned the connected callable before
			// invocation. Preserve that behavior after resilient traversal has
			// stabilized the vector entry, but perform the potentially throwing
			// user clone outside signal_impl's mutex.
			packed_function invocation;
			{
				std::lock_guard copyLock(retained->copyMutex);
				invocation = retained->callable;
			}
			if constexpr (std::is_void_v<Return>)
			{
				invocation.get<Return(Arguments...)>()(
					std::forward<Arguments>(args)...);
				return;
			}
			else
			{
				return invocation.get<Return(Arguments...)>()(
					std::forward<Arguments>(args)...);
			}
		};
		packed_function wrapper;
		wrapper.init<decltype(forwarder), Return, Arguments...>(std::move(forwarder));
		return wrapper;
	}
};

class signal_impl
{
public:
	uint64_t add(packed_function fn);

	void remove(uint64_t id) noexcept;

	void remove_all() noexcept;

	size_t count() const noexcept;

	template <class Combiner, class Result, class Signature, class... Args>
	Result invoke(Args... args) const
	{
		packed_function slot;
		size_t slotIndex = 0;
		uint64_t slotId = 1;

		if constexpr (std::is_same_v<Result, void>)
		{
			while (get_next_slot(slot, slotIndex, slotId))
			{
				slot.get<Signature>()(std::forward<Args>(args)...);
			}
		}
		else
		{
			Combiner combiner;
			while (get_next_slot(slot, slotIndex, slotId))
			{
				combiner(slot.get<Signature>()(std::forward<Args>(args)...));
			}
			return combiner.get_value();
		}
	}

	template <class Signature, class FailureHandler, class... Args>
	void invoke_resilient(FailureHandler&& failureHandler, Args&&... args) const noexcept
	{
		packed_function slot;
		size_t slotIndex = 0;
		uint64_t slotId = 1;

		while (true)
		{
			const auto previousSlotIndex = slotIndex;
			const auto previousSlotId = slotId;
			try
			{
				if (!get_next_resilient_slot<Signature>(slot, slotIndex, slotId))
				{
					break;
				}
			}
			catch (...)
			{
				try
				{
					failureHandler(std::current_exception());
				}
				catch (...)
				{
					// A diagnostic callback must not interrupt remaining slots.
				}
				if (slotIndex == previousSlotIndex && slotId == previousSlotId)
				{
					// Registry initialization or mutex acquisition failed before
					// traversal advanced. Retrying would spin forever at an
					// irrevocable lifecycle boundary.
					break;
				}
				continue;
			}

			try
			{
				slot.get<Signature>()(std::forward<Args>(args)...);
			}
			catch (...)
			{
				try
				{
					failureHandler(std::current_exception());
				}
				catch (...)
				{
					// A diagnostic callback must not interrupt remaining slots.
				}
			}
		}
	}

private:
	bool get_next_slot(packed_function& slot, size_t& expectedIndex, uint64_t& nextId) const;

	template <class Signature>
	bool get_next_resilient_slot(
		packed_function& slot,
		size_t& expectedIndex,
		uint64_t& nextId) const
	{
		std::shared_ptr<resilient_slot_authority> retained;
		auto& registry = resilient_slot_registry<Signature>::instance();
		{
			// Serialize lazy stabilization separately from the historical signal
			// mutex. Old statically linked add/remove implementations know only
			// m_mutex and the unchanged vector<packed_function> representation.
			std::lock_guard registryLock(registry.mutex);
			registry.prune_expired_locked();
			std::lock_guard signalLock(m_mutex);

			if (expectedIndex >= m_ids.size() || m_ids[expectedIndex] != nextId)
			{
				auto it = (nextId < m_nextId)
					? std::lower_bound(m_ids.cbegin(), m_ids.cend(), nextId)
					: m_ids.end();
				if (it == m_ids.end())
				{
					return false;
				}
				expectedIndex = std::distance(m_ids.cbegin(), it);
			}

			const auto currentIndex = expectedIndex;
			const auto currentId = m_ids[currentIndex];
			nextId = (currentIndex + 1 < m_ids.size())
				? m_ids[currentIndex + 1]
				: currentId + 1;
			++expectedIndex;

			const resilient_slot_key key {this, currentId};
			if (const auto found = registry.slots.find(key);
				found != registry.slots.end())
			{
				retained = found->second.lock();
				if (!retained)
				{
					registry.slots.erase(found);
				}
			}

			if (!retained)
			{
				auto original = std::make_shared<resilient_slot_authority>();
				auto wrapper = resilient_slot_wrapper<Signature>::make(original);
				// Register the weak authority before mutating the historical slot.
				// Allocation failure therefore leaves the slot untouched while the
				// traversal has already advanced to the next stable ID.
				registry.slots.emplace(key, original);
				original->callable = std::move(m_functions[currentIndex]);
				m_functions[currentIndex] = std::move(wrapper);
				retained = std::move(original);
			}
		}

		// User callable cloning happens after both locks are released. The
		// installed wrapper keeps the original alive across old-code
		// disconnect/connect calls, and its shared_ptr-only copy is noexcept for
		// ordinary historical emission.
		//
		// copyMutex is held only across the clone, never across the invocation,
		// so a user callable may re-enter num_slots/connect/disconnect. The one
		// unsupported re-entry is a copy constructor that resiliently re-emits
		// the same slot of the same signal: copyMutex is not recursive.
		slot.reset();
		{
			std::lock_guard copyLock(retained->copyMutex);
			slot = retained->callable;
		}
		return true;
	}

	mutable spin_mutex m_mutex;
	// Keep this exact element representation and member order. FastSignals is
	// statically linked into binary extension modules, whose historical
	// add/remove/emission code can operate on App-owned signal instances.
	mutable std::vector<packed_function> m_functions;
	std::vector<uint64_t> m_ids;
	uint64_t m_nextId = 1;
};

using signal_impl_ptr = std::shared_ptr<signal_impl>;
using signal_impl_weak_ptr = std::weak_ptr<signal_impl>;

} // namespace fastsignals::detail
