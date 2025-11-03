/**************************************************************************/
/*  hash_set.h                                                            */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "core/os/memory.h"
#include "core/string/print_string.h"
#include "core/templates/hashfuncs.h"

/**
 * Essentially the same thing as AHashMap, but only storing keys.
 * Use RBSet instead of this only if the following conditions are met:
 *
 * - You need to keep an iterator or const pointer to Key and you intend to add/remove elements in the meantime.
 * - Iteration order does matter (via operator<)
 *
 */

template <typename TKey,
		typename Hasher = HashMapHasherDefault,
		typename Comparator = HashMapComparatorDefault<TKey>>
class HashSet {
public:
	// Must be a power of two.
	static constexpr uint32_t INITIAL_CAPACITY = 16;
	static constexpr uint32_t EMPTY_HASH = 0;
	static_assert(EMPTY_HASH == 0, "EMPTY_HASH must always be 0 for the memcpy() optimization.");

private:
	struct Metadata {
		uint32_t hash;
		uint32_t key_idx;
	};

	static_assert(sizeof(Metadata) == 8);

	TKey *_keys = nullptr;
	Metadata *_metadata = nullptr;

	// Due to optimization, this is `capacity - 1`. Use + 1 to get normal capacity.
	uint32_t _capacity_mask = 0;
	uint32_t _size = 0;

	_FORCE_INLINE_ uint32_t _hash(const TKey &p_key) const {
		uint32_t hash = Hasher::hash(p_key);

		if (unlikely(hash == EMPTY_HASH)) {
			hash = EMPTY_HASH + 1;
		}

		return hash;
	}

	static _FORCE_INLINE_ uint32_t _get_resize_count(uint32_t p_capacity_mask) {
		return p_capacity_mask ^ (p_capacity_mask + 1) >> 2; // = get_capacity() * 0.75 - 1; Works only if p_capacity_mask = 2^n - 1.
	}

	static _FORCE_INLINE_ uint32_t _get_probe_length(uint32_t p_meta_idx, uint32_t p_hash, uint32_t p_capacity) {
		const uint32_t original_idx = p_hash & p_capacity;
		return (p_meta_idx - original_idx + p_capacity + 1) & p_capacity;
	}

	bool _lookup_idx(const TKey &p_key, uint32_t &r_key_idx, uint32_t &r_meta_idx) const {
		if (unlikely(_keys == nullptr)) {
			return false; // Failed lookups, no _keys.
		}
		return _lookup_idx_with_hash(p_key, r_key_idx, r_meta_idx, _hash(p_key));
	}

	bool _lookup_idx_with_hash(const TKey &p_key, uint32_t &r_key_idx, uint32_t &r_meta_idx, uint32_t p_hash) const {
		if (unlikely(_keys == nullptr)) {
			return false; // Failed lookups, no _keys.
		}

		uint32_t meta_idx = p_hash & _capacity_mask;
		Metadata metadata = _metadata[meta_idx];
		if (metadata.hash == p_hash && Comparator::compare(_keys[metadata.key_idx], p_key)) {
			r_key_idx = metadata.key_idx;
			r_meta_idx = meta_idx;
			return true;
		}

		if (metadata.hash == EMPTY_HASH) {
			return false;
		}

		// A collision occurred.
		meta_idx = (meta_idx + 1) & _capacity_mask;
		uint32_t distance = 1;
		while (true) {
			metadata = _metadata[meta_idx];
			if (metadata.hash == p_hash && Comparator::compare(_keys[metadata.key_idx], p_key)) {
				r_key_idx = metadata.key_idx;
				r_meta_idx = meta_idx;
				return true;
			}

			if (metadata.hash == EMPTY_HASH) {
				return false;
			}

			if (distance > _get_probe_length(meta_idx, metadata.hash, _capacity_mask)) {
				return false;
			}

			meta_idx = (meta_idx + 1) & _capacity_mask;
			distance++;
		}
	}

	uint32_t _insert_metadata(uint32_t p_hash, uint32_t p_key_idx) {
		uint32_t meta_idx = p_hash & _capacity_mask;

		if (_metadata[meta_idx].hash == EMPTY_HASH) {
			_metadata[meta_idx] = Metadata{ p_hash, p_key_idx };
			return meta_idx;
		}

		uint32_t distance = 1;
		meta_idx = (meta_idx + 1) & _capacity_mask;
		Metadata metadata;
		metadata.hash = p_hash;
		metadata.key_idx = p_key_idx;

		while (true) {
			if (_metadata[meta_idx].hash == EMPTY_HASH) {
#ifdef DEV_ENABLED
				if (unlikely(distance > 12)) {
					WARN_PRINT("Excessive collision count, is the right hash function being used?");
				}
#endif
				_metadata[meta_idx] = metadata;
				return meta_idx;
			}

			// Not an empty slot, let's check the probing length of the existing one.
			uint32_t existing_probe_len = _get_probe_length(meta_idx, _metadata[meta_idx].hash, _capacity_mask);
			if (existing_probe_len < distance) {
				SWAP(metadata, _metadata[meta_idx]);
				distance = existing_probe_len;
			}

			meta_idx = (meta_idx + 1) & _capacity_mask;
			distance++;
		}
	}

	void _resize_and_rehash(uint32_t p_new_capacity) {
		uint32_t real_old_capacity = _capacity_mask + 1;
		// Capacity can't be 0 and must be 2^n - 1.
		_capacity_mask = MAX(4u, p_new_capacity);
		uint32_t real_capacity = next_power_of_2(_capacity_mask);
		_capacity_mask = real_capacity - 1;

		Metadata *old_set_data = _metadata;

		_metadata = reinterpret_cast<Metadata *>(Memory::alloc_static_zeroed(sizeof(Metadata) * real_capacity));
		_keys = reinterpret_cast<TKey *>(Memory::realloc_static(_keys, sizeof(TKey) * (_get_resize_count(_capacity_mask) + 1)));

		if (_size != 0) {
			for (uint32_t i = 0; i < real_old_capacity; i++) {
				Metadata metadata = old_set_data[i];
				if (metadata.hash != EMPTY_HASH) {
					_insert_metadata(metadata.hash, metadata.key_idx);
				}
			}
		}

		Memory::free_static(old_set_data);
	}

	int32_t _insert(const TKey &p_key, uint32_t p_hash) {
		if (unlikely(_keys == nullptr)) {
			// Allocate on demand to save memory.

			uint32_t real_capacity = _capacity_mask + 1;
			_metadata = reinterpret_cast<Metadata *>(Memory::alloc_static_zeroed(sizeof(Metadata) * real_capacity));
			_keys = reinterpret_cast<TKey *>(Memory::alloc_static(sizeof(TKey) * (_get_resize_count(_capacity_mask) + 1)));
		}

		if (unlikely(_size > _get_resize_count(_capacity_mask))) {
			_resize_and_rehash(_capacity_mask * 2);
		}

		memnew_placement(&_keys[_size], TKey(p_key));

		_insert_metadata(p_hash, _size);
		_size++;
		return _size - 1;
	}

	void _init_from(const HashSet &p_other) {
		_capacity_mask = p_other._capacity_mask;
		uint32_t real_capacity = _capacity_mask + 1;
		_size = p_other._size;

		if (p_other._size == 0) {
			return;
		}

		_metadata = reinterpret_cast<Metadata *>(Memory::alloc_static(sizeof(Metadata) * real_capacity));
		_keys = reinterpret_cast<TKey *>(Memory::alloc_static(sizeof(TKey) * (_get_resize_count(_capacity_mask) + 1)));

		if constexpr (std::is_trivially_copyable_v<TKey>) {
			void *destination = _keys;
			const void *source = p_other._keys;
			memcpy(destination, source, sizeof(TKey) * _size);
		} else {
			for (uint32_t i = 0; i < _size; i++) {
				memnew_placement(&_keys[i], TKey(p_other._keys[i]));
			}
		}

		memcpy(_metadata, p_other._metadata, sizeof(Metadata) * real_capacity);
	}

public:
	/* Standard Godot Container API */

	_FORCE_INLINE_ uint32_t get_capacity() const { return _capacity_mask + 1; }
	_FORCE_INLINE_ uint32_t size() const { return _size; }

	_FORCE_INLINE_ bool is_empty() const {
		return _size == 0;
	}

	void clear() {
		if (_keys == nullptr || _size == 0) {
			return;
		}

		memset(_metadata, EMPTY_HASH, (_capacity_mask + 1) * sizeof(Metadata));
		if constexpr (!(std::is_trivially_destructible_v<TKey>)) {
			for (uint32_t i = 0; i < _size; i++) {
				_keys[i].~TKey();
			}
		}

		_size = 0;
	}

	_FORCE_INLINE_ bool has(const TKey &p_key) const {
		uint32_t _idx = 0;
		uint32_t meta_idx = 0;
		return _lookup_idx(p_key, _idx, meta_idx);
	}

	bool erase(const TKey &p_key) {
		uint32_t meta_idx = 0;
		uint32_t key_idx = 0;
		bool exists = _lookup_idx(p_key, key_idx, meta_idx);

		if (!exists) {
			return false;
		}

		uint32_t next_meta_idx = (meta_idx + 1) & _capacity_mask;
		while (_metadata[next_meta_idx].hash != EMPTY_HASH && _get_probe_length(next_meta_idx, _metadata[next_meta_idx].hash, _capacity_mask) != 0) {
			SWAP(_metadata[next_meta_idx], _metadata[meta_idx]);

			meta_idx = next_meta_idx;
			next_meta_idx = (next_meta_idx + 1) & _capacity_mask;
		}

		_metadata[meta_idx].hash = EMPTY_HASH;
		_keys[key_idx].~TKey();
		_size--;

		if (key_idx < _size) {
			memcpy((void *)&_keys[key_idx], (const void *)&_keys[_size], sizeof(TKey));
			uint32_t moved_key_idx = 0;
			uint32_t moved_meta_idx = 0;
			_lookup_idx(_keys[_size], moved_key_idx, moved_meta_idx);
			_metadata[moved_meta_idx].key_idx = key_idx;
		}

		return true;
	}

	// Replace the key of an entry in-place, without invalidating iterators or changing the entries position during iteration.
	// p_old_key must exist in the set and p_new_key must not, unless it is equal to p_old_key.
	bool replace_key(const TKey &p_old_key, const TKey &p_new_key) {
		if (p_old_key == p_new_key) {
			return true;
		}
		uint32_t meta_idx = 0;
		uint32_t key_idx = 0;
		ERR_FAIL_COND_V(_lookup_idx(p_new_key, key_idx, meta_idx), false);
		ERR_FAIL_COND_V(!_lookup_idx(p_old_key, key_idx, meta_idx), false);
		TKey &element = _keys[key_idx];

		uint32_t next_meta_idx = (meta_idx + 1) & _capacity_mask;
		while (_metadata[next_meta_idx].hash != EMPTY_HASH && _get_probe_length(next_meta_idx, _metadata[next_meta_idx].hash, _capacity_mask) != 0) {
			SWAP(_metadata[next_meta_idx], _metadata[meta_idx]);

			meta_idx = next_meta_idx;
			next_meta_idx = (next_meta_idx + 1) & _capacity_mask;
		}

		_metadata[meta_idx].hash = EMPTY_HASH;

		uint32_t hash = _hash(p_new_key);
		_insert_metadata(hash, key_idx);

		return true;
	}

	// Reserves space for a number of elements, useful to avoid many resizes and rehashes.
	// If adding a known (possibly large) number of elements at once, must be larger than old capacity.
	void reserve(uint32_t p_new_capacity) {
		if (_keys == nullptr) {
			_capacity_mask = MAX(4u, p_new_capacity);
			_capacity_mask = next_power_of_2(_capacity_mask) - 1;
			return; // Unallocated yet.
		}
		if (p_new_capacity <= get_capacity()) {
			if (p_new_capacity < size()) {
				WARN_VERBOSE("reserve() called with a capacity smaller than the current size. This is likely a mistake.");
			}
			return;
		}
		_resize_and_rehash(p_new_capacity);
	}

	/** Iterator API **/

	struct ConstIterator {
		_FORCE_INLINE_ const TKey &operator*() const {
			return *key;
		}
		_FORCE_INLINE_ const TKey *operator->() const {
			return key;
		}
		_FORCE_INLINE_ ConstIterator &operator++() {
			key++;
			return *this;
		}

		_FORCE_INLINE_ ConstIterator &operator--() {
			key--;
			if (key < begin) {
				key = end;
			}
			return *this;
		}

		_FORCE_INLINE_ bool operator==(const ConstIterator &b) const { return key == b.key; }
		_FORCE_INLINE_ bool operator!=(const ConstIterator &b) const { return key != b.key; }

		_FORCE_INLINE_ explicit operator bool() const {
			return key != end;
		}

		_FORCE_INLINE_ ConstIterator(TKey *p_key, TKey *p_begin, TKey *p_end) {
			key = p_key;
			begin = p_begin;
			end = p_end;
		}
		_FORCE_INLINE_ ConstIterator() {}
		_FORCE_INLINE_ ConstIterator(const ConstIterator &p_it) {
			key = p_it.key;
			begin = p_it.begin;
			end = p_it.end;
		}
		_FORCE_INLINE_ void operator=(const ConstIterator &p_it) {
			key = p_it.key;
			begin = p_it.begin;
			end = p_it.end;
		}

	private:
		TKey *key = nullptr;
		TKey *begin = nullptr;
		TKey *end = nullptr;
	};

	struct Iterator {
		_FORCE_INLINE_ TKey &operator*() const {
			return *key;
		}
		_FORCE_INLINE_ TKey *operator->() const {
			return key;
		}
		_FORCE_INLINE_ Iterator &operator++() {
			key++;
			return *this;
		}
		_FORCE_INLINE_ Iterator &operator--() {
			key--;
			if (key < begin) {
				key = end;
			}
			return *this;
		}

		_FORCE_INLINE_ bool operator==(const Iterator &b) const { return key == b.key; }
		_FORCE_INLINE_ bool operator!=(const Iterator &b) const { return key != b.key; }

		_FORCE_INLINE_ explicit operator bool() const {
			return key != end;
		}

		_FORCE_INLINE_ Iterator(TKey *p_key, TKey *p_begin, TKey *p_end) {
			key = p_key;
			begin = p_begin;
			end = p_end;
		}
		_FORCE_INLINE_ Iterator() {}
		_FORCE_INLINE_ Iterator(const Iterator &p_it) {
			key = p_it.key;
			begin = p_it.begin;
			end = p_it.end;
		}
		_FORCE_INLINE_ void operator=(const Iterator &p_it) {
			key = p_it.key;
			begin = p_it.begin;
			end = p_it.end;
		}

		operator ConstIterator() const {
			return ConstIterator(key, begin, end);
		}

	private:
		TKey *key = nullptr;
		TKey *begin = nullptr;
		TKey *end = nullptr;
	};

	_FORCE_INLINE_ Iterator begin() {
		return Iterator(_keys, _keys, _keys + _size);
	}
	_FORCE_INLINE_ Iterator end() {
		return Iterator(_keys + _size, _keys, _keys + _size);
	}
	_FORCE_INLINE_ Iterator last() {
		if (unlikely(_size == 0)) {
			return Iterator(nullptr, nullptr, nullptr);
		}
		return Iterator(_keys + _size - 1, _keys, _keys + _size);
	}

	Iterator find(const TKey &p_key) {
		uint32_t meta_idx = 0;
		uint32_t key_idx = 0;
		bool exists = _lookup_idx(p_key, key_idx, meta_idx);
		if (!exists) {
			return end();
		}
		return Iterator(_keys + key_idx, _keys, _keys + _size);
	}

	void remove(const Iterator &p_iter) {
		if (p_iter) {
			erase(*p_iter);
		}
	}

	_FORCE_INLINE_ ConstIterator begin() const {
		return ConstIterator(_keys, _keys, _keys + _size);
	}
	_FORCE_INLINE_ ConstIterator end() const {
		return ConstIterator(_keys + _size, _keys, _keys + _size);
	}
	_FORCE_INLINE_ ConstIterator last() const {
		if (unlikely(_size == 0)) {
			return ConstIterator(nullptr, nullptr, nullptr);
		}
		return ConstIterator(_keys + _size - 1, _keys, _keys + _size);
	}

	ConstIterator find(const TKey &p_key) const {
		uint32_t key_idx = 0;
		uint32_t meta_idx = 0;
		bool exists = _lookup_idx(p_key, key_idx, meta_idx);
		if (!exists) {
			return end();
		}
		return ConstIterator(_keys + key_idx, _keys, _keys + _size);
	}

	/* Insert */

	Iterator insert(const TKey &p_key) {
		uint32_t key_idx = 0;
		uint32_t meta_idx = 0;
		uint32_t hash = _hash(p_key);
		bool exists = _lookup_idx_with_hash(p_key, key_idx, meta_idx, hash);

		if (!exists) {
			key_idx = _insert(p_key, hash);
		}
		return Iterator(_keys + key_idx, _keys, _keys + _size);
	}

	// Inserts an element without checking if it already exists.
	Iterator insert_new(const TKey &p_key) {
		DEV_ASSERT(!has(p_key));
		uint32_t hash = _hash(p_key);
		uint32_t key_idx = _insert(p_key, hash);
		return Iterator(_keys + key_idx, _keys, _keys + _size);
	}

	/* Array methods. */

	// Returns the element index. If not found, returns -1.
	int get_index(const TKey &p_key) {
		uint32_t key_idx = 0;
		uint32_t meta_idx = 0;
		bool exists = _lookup_idx(p_key, key_idx, meta_idx);
		if (!exists) {
			return -1;
		}
		return key_idx;
	}

	TKey &get_by_index(uint32_t p_index) {
		CRASH_BAD_UNSIGNED_INDEX(p_index, _size);
		return _keys[p_index];
	}

	bool erase_by_index(uint32_t p_index) {
		if (p_index >= size()) {
			return false;
		}
		return erase(_keys[p_index]);
	}

	/* Constructors */

	HashSet(HashSet &&p_other) {
		_keys = p_other._keys;
		_metadata = p_other._metadata;
		_capacity_mask = p_other._capacity_mask;
		_size = p_other._size;

		p_other._keys = nullptr;
		p_other._metadata = nullptr;
		p_other._capacity_mask = 0;
		p_other._size = 0;
	}

	HashSet(const HashSet &p_other) {
		_init_from(p_other);
	}

	void operator=(const HashSet &p_other) {
		if (this == &p_other) {
			return; // Ignore self assignment.
		}

		reset();

		_init_from(p_other);
	}

	bool operator==(const HashSet &p_other) const {
		if (_size != p_other._size) {
			return false;
		}
		for (uint32_t i = 0; i < _size; i++) {
			if (!p_other.has(_keys[i])) {
				return false;
			}
		}
		return true;
	}
	bool operator!=(const HashSet &p_other) const {
		return !(*this == p_other);
	}

	HashSet(uint32_t p_initial_capacity) {
		// Capacity can't be 0 and must be 2^n - 1.
		_capacity_mask = MAX(4u, p_initial_capacity);
		_capacity_mask = next_power_of_2(_capacity_mask) - 1;
	}
	HashSet() :
			_capacity_mask(INITIAL_CAPACITY - 1) {
	}

	HashSet(std::initializer_list<TKey> p_init) {
		reserve(p_init.size());
		for (const TKey &E : p_init) {
			insert(E);
		}
	}

	void reset() {
		if (_keys != nullptr) {
			if constexpr (!(std::is_trivially_destructible_v<TKey>)) {
				for (uint32_t i = 0; i < _size; i++) {
					_keys[i].~TKey();
				}
			}
			Memory::free_static(_keys);
			Memory::free_static(_metadata);
			_keys = nullptr;
		}
		_capacity_mask = INITIAL_CAPACITY - 1;
		_size = 0;
	}

	~HashSet() {
		reset();
	}
};
