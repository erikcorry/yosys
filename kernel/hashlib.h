// This is free and unencumbered software released into the public domain.
//
// Anyone is free to copy, modify, publish, use, compile, sell, or
// distribute this software, either in source code form or as a compiled
// binary, for any purpose, commercial or non-commercial, and by any
// means.

// -------------------------------------------------------
// Written by Claire Xenia Wolf <claire@yosyshq.com> in 2014
// -------------------------------------------------------

#ifndef HASHLIB_H
#define HASHLIB_H

#include <array>
#include <atomic>
#include <new>
#include <stdexcept>
#include <algorithm>
#include <optional>
#include <string>
#include <variant>
#include <vector>
#include <type_traits>
#include <stdint.h>

#define YS_HASHING_VERSION 1

namespace hashlib {

/**
 * HASHING
 *
 * Also refer to docs/source/yosys_internals/hashing.rst
 *
 * The Hasher knows how to hash 32 and 64-bit integers. That's it.
 * In the future, it could be expanded to do vectors with SIMD.
 *
 * The Hasher doesn't know how to hash common standard containers
 * and compositions. However, hashlib provides centralized wrappers.
 *
 * Hashlib doesn't know how to hash silly Yosys-specific types.
 * Hashlib doesn't depend on Yosys and can be used standalone.
 * Please don't use hashlib standalone for new projects.
 * Never directly include kernel/hashlib.h in Yosys code.
 * Instead include kernel/yosys_common.h
 *
 * The hash_ops type is now always left to its default value, derived
 * from templated functions through SFINAE. Providing custom ops is
 * still supported.
 *
 * HASH TABLES
 *
 * We implement associative data structures with separate chaining.
 * Linked lists use integers into the indirection hashtable array
 * instead of pointers.
 */

#if defined(__GNUC__) || defined(__clang__)
#  define HASHLIB_ATTRIBUTE_WARN_UNUSED __attribute__((warn_unused))
#else
#  define HASHLIB_ATTRIBUTE_WARN_UNUSED
#endif

const int hashtable_size_trigger = 2;
const int hashtable_size_factor = 3;

namespace legacy {
	inline uint32_t djb2_add(uint32_t a, uint32_t b) {
		return ((a << 5) + a) + b;
	}
};

template<typename T>
struct hash_ops;

inline unsigned int mkhash_xorshift(unsigned int a) {
	if (sizeof(a) == 4) {
		a ^= a << 13;
		a ^= a >> 17;
		a ^= a << 5;
	} else if (sizeof(a) == 8) {
		a ^= a << 13;
		a ^= a >> 7;
		a ^= a << 17;
	} else
		throw std::runtime_error("mkhash_xorshift() only implemented for 32 bit and 64 bit ints");
	return a;
}

class HasherDJB32 {
public:
	using hash_t = uint32_t;

	HasherDJB32() {
		// traditionally 5381 is used as starting value for the djb2 hash
		state = 5381;
	}

private:
	uint32_t state;
	// The XOR version of DJB2
	[[nodiscard]]
	static uint32_t djb2_xor(uint32_t a, uint32_t b) {
		uint32_t hash = ((a << 5) + a) ^ b;
		return hash;
	}
public:
	void hash32(uint32_t i) {
		state = djb2_xor(i, state);
		state = mkhash_xorshift(state);
		return;
	}
	void hash64(uint64_t i) {
		state = djb2_xor((uint32_t)(i & 0xFFFFFFFFULL), state);
		state = djb2_xor((uint32_t)(i >> 32ULL), state);
		state = mkhash_xorshift(state);
		return;
	}
	[[nodiscard]]
	hash_t yield() const {
		return (hash_t)state;
	}

	template<typename T>
	void eat(T&& t) {
		*this = hash_ops<std::remove_cv_t<std::remove_reference_t<T>>>::hash_into(std::forward<T>(t), *this);
	}

	template<typename T>
	void eat(const T& t) {
		*this = hash_ops<T>::hash_into(t, *this);
	}

	[[deprecated]]
	void commutative_eat(hash_t t) {
		state ^= t;
	}

	void force(hash_t new_state) {
		state = new_state;
	}
};

using Hasher = HasherDJB32;

// Boilerplate compressor for trivially implementing
// top-level hash method with hash_into
#define HASH_TOP_LOOP_FST [[nodiscard]] static inline Hasher hash
#define HASH_TOP_LOOP_SND { \
	Hasher h; \
	h = hash_into(a, h); \
	return h; \
}

template<typename T>
struct hash_ops {
	static inline bool cmp(const T &a, const T &b) {
		return a == b;
	}
	[[nodiscard]] static inline Hasher hash_into(const T &a, Hasher h) {
		if constexpr (std::is_integral_v<T>) {
			static_assert(sizeof(T) <= sizeof(uint64_t));
			if (sizeof(T) == sizeof(uint64_t))
				h.hash64(a);
			else
				h.hash32(a);
			return h;
		} else if constexpr (std::is_enum_v<T>) {
			using u_type = std::underlying_type_t<T>;
			return hash_ops<u_type>::hash_into((u_type) a, h);
		} else if constexpr (std::is_pointer_v<T>) {
			return hash_ops<uintptr_t>::hash_into((uintptr_t) a, h);
		} else if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, std::string_view>) {
			int size = a.size();
			int i = 0;
			while (i + 8 < size) {
				uint64_t v;
				memcpy(&v, a.data() + i, 8);
				h.hash64(v);
				i += 8;
			}
			uint64_t v = 0;
			memcpy(&v, a.data() + i, size - i);
			h.hash64(v);
			return h;
		} else {
			return a.hash_into(h);
		}
	}
	HASH_TOP_LOOP_FST (const T &a) HASH_TOP_LOOP_SND
};

template<typename P, typename Q> struct hash_ops<std::pair<P, Q>> {
	static inline bool cmp(const std::pair<P, Q> &a, const std::pair<P, Q> &b) {
		return a == b;
	}
	[[nodiscard]] static inline Hasher hash_into(const std::pair<P, Q> &a, Hasher h) {
		h = hash_ops<P>::hash_into(a.first, h);
		h = hash_ops<Q>::hash_into(a.second, h);
		return h;
	}
	HASH_TOP_LOOP_FST (const std::pair<P, Q> &a) HASH_TOP_LOOP_SND
	[[nodiscard]] static inline Hasher hash(const P &p, const Q &q) {
		Hasher h;
		h = hash_ops<P>::hash_into(p, h);
		h = hash_ops<Q>::hash_into(q, h);
		return h;
	}
};

template<typename... T> struct hash_ops<std::tuple<T...>> {
	static inline bool cmp(const std::tuple<T...> &a, const std::tuple<T...> &b) {
		return a == b;
	}
	template<size_t I = 0>
	static inline typename std::enable_if<I == sizeof...(T), Hasher>::type hash_into(const std::tuple<T...> &, Hasher h) {
		return h;
	}
	template<size_t I = 0>
	static inline typename std::enable_if<I != sizeof...(T), Hasher>::type hash_into(const std::tuple<T...> &a, Hasher h) {
		typedef hash_ops<typename std::tuple_element<I, std::tuple<T...>>::type> element_ops_t;
		h = hash_into<I+1>(a, h);
		h = element_ops_t::hash_into(std::get<I>(a), h);
		return h;
	}
	HASH_TOP_LOOP_FST (const std::tuple<T...> &a) HASH_TOP_LOOP_SND
};

template<typename T> struct hash_ops<std::vector<T>> {
	static inline bool cmp(const std::vector<T> &a, const std::vector<T> &b) {
		return a == b;
	}
	[[nodiscard]] static inline Hasher hash_into(const std::vector<T> &a, Hasher h) {
		h.eat((uint32_t)a.size());
		for (auto k : a)
			h.eat(k);
		return h;
	}
	HASH_TOP_LOOP_FST (const std::vector<T> &a) HASH_TOP_LOOP_SND
};

template<typename T, size_t N> struct hash_ops<std::array<T, N>> {
    static inline bool cmp(const std::array<T, N> &a, const std::array<T, N> &b) {
        return a == b;
    }
    [[nodiscard]] static inline Hasher hash_into(const std::array<T, N> &a, Hasher h) {
        for (const auto& k : a)
            h = hash_ops<T>::hash_into(k, h);
        return h;
    }
	HASH_TOP_LOOP_FST (const std::array<T, N> &a) HASH_TOP_LOOP_SND
};

struct hash_cstr_ops {
	static inline bool cmp(const char *a, const char *b) {
		return strcmp(a, b) == 0;
	}
	[[nodiscard]] static inline Hasher hash_into(const char *a, Hasher h) {
		while (*a)
			h.hash32(*(a++));
		return h;
	}
	HASH_TOP_LOOP_FST (const char *a) HASH_TOP_LOOP_SND
};

template <> struct hash_ops<char*> : hash_cstr_ops {};

struct hash_ptr_ops {
	static inline bool cmp(const void *a, const void *b) {
		return a == b;
	}
	[[nodiscard]] static inline Hasher hash_into(const void *a, Hasher h) {
		return hash_ops<uintptr_t>::hash_into((uintptr_t)a, h);
	}
	HASH_TOP_LOOP_FST (const void *a) HASH_TOP_LOOP_SND
};

struct hash_obj_ops {
	static inline bool cmp(const void *a, const void *b) {
		return a == b;
	}
	template<typename T>
	[[nodiscard]] static inline Hasher hash_into(const T *a, Hasher h) {
		if (a)
			h = a->hash_into(h);
		else
			h.eat(0);
		return h;
	}
	template<typename T>
	HASH_TOP_LOOP_FST (const T *a) HASH_TOP_LOOP_SND
};
/**
 * If you find yourself using this function, think hard
 * about if it's the right thing to do. Mixing finalized
 * hashes together with XORs or worse can destroy
 * desirable qualities of the hash function
 */
template<typename T>
[[nodiscard]]
Hasher::hash_t run_hash(const T& obj) {
	return hash_ops<T>::hash(obj).yield();
}

/** Refer to docs/source/yosys_internals/hashing.rst */
template<typename T>
[[nodiscard]]
[[deprecated]]
inline unsigned int mkhash(const T &v) {
	return (unsigned int) run_hash<T>(v);
}

template<> struct hash_ops<std::monostate> {
	static inline bool cmp(std::monostate a, std::monostate b) {
		return a == b;
	}
	[[nodiscard]] static inline Hasher hash_into(std::monostate, Hasher h) {
		return h;
	}
};

template<typename... T> struct hash_ops<std::variant<T...>> {
	static inline bool cmp(const std::variant<T...> &a, const std::variant<T...> &b) {
		return a == b;
	}
	[[nodiscard]] static inline Hasher hash_into(const std::variant<T...> &a, Hasher h) {
		std::visit([& h](const auto &v) { h.eat(v); }, a);
		h.eat(a.index());
		return h;
	}
};

template<typename T> struct hash_ops<std::optional<T>> {
	static inline bool cmp(const std::optional<T> &a, const std::optional<T> &b) {
		return a == b;
	}
	[[nodiscard]] static inline Hasher hash_into(const std::optional<T> &a, Hasher h) {
		if(a.has_value())
			h.eat(*a);
		else
			h.eat(0);
		return h;
	}
};

inline unsigned int hashtable_size(unsigned int min_size)
{
	// Primes as generated by https://oeis.org/A175953
	static std::vector<unsigned int> zero_and_some_primes = {
		0, 23, 29, 37, 47, 59, 79, 101, 127, 163, 211, 269, 337, 431, 541, 677,
		853, 1069, 1361, 1709, 2137, 2677, 3347, 4201, 5261, 6577, 8231, 10289,
		12889, 16127, 20161, 25219, 31531, 39419, 49277, 61603, 77017, 96281,
		120371, 150473, 188107, 235159, 293957, 367453, 459317, 574157, 717697,
		897133, 1121423, 1401791, 1752239, 2190299, 2737937, 3422429, 4278037,
		5347553, 6684443, 8355563, 10444457, 13055587, 16319519, 20399411,
		25499291, 31874149, 39842687, 49803361, 62254207, 77817767, 97272239,
		121590311, 151987889, 189984863, 237481091, 296851369, 371064217,
		463830313, 579787991, 724735009, 905918777, 1132398479, 1415498113,
		1769372713, 2211715897, 2764644887, 3455806139
	};

	for (auto p : zero_and_some_primes)
		if (p >= min_size) return p;

	if (sizeof(unsigned int) == 4)
		throw std::length_error("hash table exceeded maximum size.\nDesign is likely too large for yosys to handle, if possible try not to flatten the design.");

	for (auto p : zero_and_some_primes)
		if (100129 * p > min_size) return 100129 * p;

	throw std::length_error("hash table exceeded maximum size.");
}

// Entries of dict and pool live in an insertion-ordered vector, and the
// hashtable only holds indices into it, like Python's dict. Erasing an entry
// destroys its value and leaves a hole (next == entry_hole) so that the
// order of the remaining entries doesn't depend on what was erased or in
// which order. Holes are squeezed out by compact(), copying and sort(), and
// by erase() when that doesn't invalidate any iterators that erase() must
// keep valid (see do_erase()). insert() never moves entries.
// The last entry of the vector is never a hole.
const int entry_hole = -2;

template<typename V>
struct hashtable_entry
{
	union { V udata; };
	int next;

	hashtable_entry() : next(entry_hole) { }
	hashtable_entry(const V &udata, int next) : udata(udata), next(next) { }
	hashtable_entry(V &&udata, int next) : udata(std::move(udata)), next(next) { }

	hashtable_entry(const hashtable_entry &other) : next(other.next) {
		if (!other.is_hole())
			new (&udata) V(other.udata);
	}
	hashtable_entry(hashtable_entry &&other) noexcept(std::is_nothrow_move_constructible_v<V>) : next(other.next) {
		if (!other.is_hole())
			new (&udata) V(std::move(other.udata));
	}
	hashtable_entry &operator=(const hashtable_entry &other) {
		if (this != &other) {
			make_hole();
			if (!other.is_hole())
				new (&udata) V(other.udata);
			next = other.next;
		}
		return *this;
	}
	hashtable_entry &operator=(hashtable_entry &&other) noexcept(std::is_nothrow_move_constructible_v<V>) {
		if (this != &other) {
			make_hole();
			if (!other.is_hole())
				new (&udata) V(std::move(other.udata));
			next = other.next;
		}
		return *this;
	}
	~hashtable_entry() {
		if (!is_hole())
			udata.~V();
	}

	bool is_hole() const { return next == entry_hole; }
	// Requires this to be a hole and other to not be one, makes other a hole.
	// The entry is unlinked, so callers must rehash afterwards.
	void move_from(hashtable_entry &other) {
		new (&udata) V(std::move(other.udata));
		next = -1;
		other.make_hole();
	}
	void make_hole() {
		if (!is_hole()) {
			udata.~V();
			next = entry_hole;
		}
	}
};

template<typename K, typename T, typename OPS = hash_ops<K>> class dict;
template<typename K, int offset = 0, typename OPS = hash_ops<K>> class idict;
template<typename K, typename OPS = hash_ops<K>> class pool;
template<typename K, typename OPS = hash_ops<K>> class mfp;

// Computes the hash value of an unordered set of elements.
// See https://www.preprints.org/manuscript/201710.0192/v1/download.
// This is the Sum(4) algorithm from that paper, which has good collision resistance,
// much better than Sum(1) or Xor(1) (and somewhat better than Xor(4)).
class commutative_hash {
public:
	commutative_hash() {
		buckets.fill(0);
	}
	template <typename T>
	void eat(const T &obj) {
		eat(hash_ops<T>::hash(obj));
	}
	void eat(const Hasher &h) {
		Hasher::hash_t v = h.yield();
		size_t index = v & (buckets.size() - 1);
		buckets[index] += v;
	}
	[[nodiscard]] Hasher hash_into(Hasher h) const {
		for (auto b : buckets)
			h.eat(b);
		return h;
	}
private:
	std::array<Hasher::hash_t, 4> buckets;
};

template<typename K, typename T, typename OPS>
class HASHLIB_ATTRIBUTE_WARN_UNUSED dict {
	typedef hashtable_entry<std::pair<K, T>> entry_t;

	std::vector<int> hashtable;
	std::vector<entry_t> entries;
	int n_holes = 0;
	int first_hole = 0; // only meaningful if n_holes > 0
	OPS ops;
	static_assert(std::is_nothrow_default_constructible_v<OPS>, "move ops are noexcept and default-construct OPS");

#ifdef NDEBUG
	static inline void do_assert(bool) { }
#else
	static inline void do_assert(bool cond) {
		if (!cond) throw std::runtime_error("dict<> assert failed.");
	}
#endif

	Hasher::hash_t do_hash(const K &key) const
	{
		Hasher::hash_t hash = 0;
		if (!hashtable.empty())
			hash = ops.hash(key).yield() % (unsigned int)(hashtable.size());
		return hash;
	}

	void do_rehash()
	{
		hashtable.clear();
		hashtable.resize(hashtable_size(entries.capacity() * hashtable_size_factor), -1);

		for (int i = 0; i < int(entries.size()); i++) {
			if (entries[i].is_hole())
				continue;
			Hasher::hash_t hash = do_hash(entries[i].udata.first);
			entries[i].next = hashtable[hash];
			hashtable[hash] = i;
		}
	}

	// Squeeze out the holes, assuming there are none below index. Entries
	// below index stay where they are.
	void do_compact_from(int index)
	{
		int w = index;
		for (int r = index; r < int(entries.size()); r++) {
			if (entries[r].is_hole())
				continue;
			if (r != w)
				entries[w].move_from(entries[r]);
			w++;
		}
		entries.erase(entries.begin() + w, entries.end());
		n_holes = 0;
		do_rehash();
	}

	void do_compact()
	{
		if (n_holes > 0)
			do_compact_from(first_hole);
	}

	void do_permute(const std::vector<int> &perm)
	{
		std::vector<entry_t> new_entries(perm.size());
		for (int i = 0; i < int(perm.size()); i++)
			new_entries[i].move_from(entries[perm[i]]);
		entries.swap(new_entries);
		do_rehash();
	}

	void do_copy(const dict &other)
	{
		entries.clear();
		n_holes = 0;
		if (other.n_holes == 0) {
			entries = other.entries;
		} else {
			entries.reserve(other.size());
			for (auto &it : other.entries)
				if (!it.is_hole())
					entries.push_back(it);
		}
		do_rehash();
	}

	int do_erase(int index, Hasher::hash_t hash)
	{
		do_assert(index < int(entries.size()));
		if (hashtable.empty() || index < 0)
			return 0;

		int k = hashtable[hash];
		do_assert(0 <= k && k < int(entries.size()));

		if (k == index) {
			hashtable[hash] = entries[index].next;
		} else {
			while (entries[k].next != index) {
				k = entries[k].next;
				do_assert(0 <= k && k < int(entries.size()));
			}
			entries[k].next = entries[index].next;
		}

		entries[index].make_hole();
		first_hole = n_holes > 0 ? std::min(first_hole, index) : index;
		n_holes++;

		while (!entries.empty() && entries.back().is_hole()) {
			entries.pop_back();
			n_holes--;
		}

		// Once half the entries are holes, compact if that only moves
		// entries above the erased one. Those are the ones that iterators
		// which haven't passed the erased entry yet would still visit, and
		// erase() is allowed to invalidate such iterators.
		if (entries.empty())
			hashtable.clear();
		else if (2 * n_holes >= int(entries.size()) && first_hole == index)
			do_compact_from(index);

		return 1;
	}

	// Index of the next entry in iteration order, which goes from the
	// back of the vector to the front. Tolerates index being past the
	// end because erase() may have removed trailing holes.
	int iter_next(int index) const
	{
		index = std::min(index, int(entries.size())) - 1;
		while (index >= 0 && entries[index].is_hole())
			index--;
		return index;
	}

	int iter_prev(int index) const
	{
		index++;
		while (entries[index].is_hole())
			index++;
		return index;
	}

	int element_index(int n) const
	{
		int index = int(entries.size())-1;
		if (n_holes == 0)
			return index-n;
		while (n-- > 0 && index >= 0)
			index = iter_next(index);
		return index;
	}

	int do_lookup(const K &key, Hasher::hash_t &hash)
	{
		if (hashtable.empty())
			return -1;

		if (entries.size() * hashtable_size_trigger > hashtable.size()) {
			do_rehash();
			hash = do_hash(key);
		}

		return do_lookup_internal(key, hash);
	}

	int do_lookup_internal(const K &key, Hasher::hash_t hash) const
	{
		int index = hashtable[hash];

		while (index >= 0 && !ops.cmp(entries[index].udata.first, key)) {
			index = entries[index].next;
			do_assert(-1 <= index && index < int(entries.size()));
		}

		return index;
	}

	int do_lookup_no_rehash(const K &key, Hasher::hash_t hash) const
	{
		if (hashtable.empty())
			return -1;

		return do_lookup_internal(key, hash);
	}

	int do_insert(const K &key, const Hasher::hash_t &hash)
	{
		if (hashtable.empty()) {
			entries.emplace_back(std::pair<K, T>(key, T()), -1);
			do_rehash();
		} else {
			entries.emplace_back(std::pair<K, T>(key, T()), hashtable[hash]);
			hashtable[hash] = entries.size() - 1;
		}
		return entries.size() - 1;
	}

	int do_insert(const std::pair<K, T> &value, const Hasher::hash_t &hash)
	{
		if (hashtable.empty()) {
			entries.emplace_back(value, -1);
			do_rehash();
		} else {
			entries.emplace_back(value, hashtable[hash]);
			hashtable[hash] = entries.size() - 1;
		}
		return entries.size() - 1;
	}

	int do_insert(std::pair<K, T> &&rvalue, const Hasher::hash_t &hash)
	{
		if (hashtable.empty()) {
			entries.emplace_back(std::forward<std::pair<K, T>>(rvalue), -1);
			do_rehash();
		} else {
			entries.emplace_back(std::forward<std::pair<K, T>>(rvalue), hashtable[hash]);
			hashtable[hash] = entries.size() - 1;
		}
		return entries.size() - 1;
	}

public:
	class const_iterator
	{
		friend class dict;
	protected:
		const dict *ptr;
		int index;
		const_iterator(const dict *ptr, int index) : ptr(ptr), index(index) { }
	public:
		typedef std::bidirectional_iterator_tag iterator_category;
		typedef std::pair<K, T> value_type;
		typedef ptrdiff_t difference_type;
		typedef const std::pair<K, T>* pointer;
		typedef const std::pair<K, T>& reference;
		const_iterator() { }
		const_iterator operator++() { index = ptr->iter_next(index); return *this; }
		const_iterator operator++(int) { const_iterator tmp = *this; index = ptr->iter_next(index); return tmp; }
		const_iterator operator--() { index = ptr->iter_prev(index); return *this; }
		const_iterator operator--(int) { const_iterator tmp = *this; index = ptr->iter_prev(index); return tmp; }
		const_iterator operator+=(int amt) {
			if (ptr->n_holes == 0)
				index -= amt;
			else
				while (amt-- > 0)
					index = ptr->iter_next(index);
			return *this;
		}
		bool operator<(const const_iterator &other) const { return index > other.index; }
		bool operator==(const const_iterator &other) const { return index == other.index; }
		bool operator!=(const const_iterator &other) const { return index != other.index; }
		const std::pair<K, T> &operator*() const { return ptr->entries[index].udata; }
		const std::pair<K, T> *operator->() const { return &ptr->entries[index].udata; }
	};

	class iterator
	{
		friend class dict;
	protected:
		dict *ptr;
		int index;
		iterator(dict *ptr, int index) : ptr(ptr), index(index) { }
	public:
		typedef std::forward_iterator_tag iterator_category;
		typedef std::pair<K, T> value_type;
		typedef ptrdiff_t difference_type;
		typedef std::pair<K, T>* pointer;
		typedef std::pair<K, T>& reference;
		iterator() { }
		iterator operator++() { index = ptr->iter_next(index); return *this; }
		iterator operator+=(int amt) {
			if (ptr->n_holes == 0)
				index -= amt;
			else
				while (amt-- > 0)
					index = ptr->iter_next(index);
			return *this;
		}
		bool operator<(const iterator &other) const { return index > other.index; }
		bool operator==(const iterator &other) const { return index == other.index; }
		bool operator!=(const iterator &other) const { return index != other.index; }
		std::pair<K, T> &operator*() { return ptr->entries[index].udata; }
		std::pair<K, T> *operator->() { return &ptr->entries[index].udata; }
		const std::pair<K, T> &operator*() const { return ptr->entries[index].udata; }
		const std::pair<K, T> *operator->() const { return &ptr->entries[index].udata; }
		operator const_iterator() const { return const_iterator(ptr, index); }
	};
	using reverse_iterator = std::reverse_iterator<const_iterator>;
	reverse_iterator rbegin() const {
		return std::make_reverse_iterator(end());
	}
	reverse_iterator rend() const {
		return std::make_reverse_iterator(begin());
	}

	constexpr dict()
	{
	}

	dict(const dict &other)
	{
		do_copy(other);
	}

	dict(dict &&other) noexcept
	{
		swap(other);
	}

	dict &operator=(const dict &other) {
		if (this != &other)
			do_copy(other);
		return *this;
	}

	dict &operator=(dict &&other) noexcept {
		clear();
		swap(other);
		return *this;
	}

	dict(const std::initializer_list<std::pair<K, T>> &list)
	{
		for (auto &it : list)
			insert(it);
	}

	template<class InputIterator>
	dict(InputIterator first, InputIterator last)
	{
		insert(first, last);
	}

	template<class InputIterator>
	void insert(InputIterator first, InputIterator last)
	{
		for (; first != last; ++first)
			insert(*first);
	}

	std::pair<iterator, bool> insert(const K &key)
	{
		Hasher::hash_t hash = do_hash(key);
		int i = do_lookup(key, hash);
		if (i >= 0)
			return std::pair<iterator, bool>(iterator(this, i), false);
		i = do_insert(key, hash);
		return std::pair<iterator, bool>(iterator(this, i), true);
	}

	std::pair<iterator, bool> insert(const std::pair<K, T> &value)
	{
		Hasher::hash_t hash = do_hash(value.first);
		int i = do_lookup(value.first, hash);
		if (i >= 0)
			return std::pair<iterator, bool>(iterator(this, i), false);
		i = do_insert(value, hash);
		return std::pair<iterator, bool>(iterator(this, i), true);
	}

	std::pair<iterator, bool> insert(std::pair<K, T> &&rvalue)
	{
		Hasher::hash_t hash = do_hash(rvalue.first);
		int i = do_lookup(rvalue.first, hash);
		if (i >= 0)
			return std::pair<iterator, bool>(iterator(this, i), false);
		i = do_insert(std::forward<std::pair<K, T>>(rvalue), hash);
		return std::pair<iterator, bool>(iterator(this, i), true);
	}

	std::pair<iterator, bool> emplace(K const &key, T const &value)
	{
		Hasher::hash_t hash = do_hash(key);
		int i = do_lookup(key, hash);
		if (i >= 0)
			return std::pair<iterator, bool>(iterator(this, i), false);
		i = do_insert(std::make_pair(key, value), hash);
		return std::pair<iterator, bool>(iterator(this, i), true);
	}

	std::pair<iterator, bool> emplace(K const &key, T &&rvalue)
	{
		Hasher::hash_t hash = do_hash(key);
		int i = do_lookup(key, hash);
		if (i >= 0)
			return std::pair<iterator, bool>(iterator(this, i), false);
		i = do_insert(std::make_pair(key, std::forward<T>(rvalue)), hash);
		return std::pair<iterator, bool>(iterator(this, i), true);
	}

	std::pair<iterator, bool> emplace(K &&rkey, T const &value)
	{
		Hasher::hash_t hash = do_hash(rkey);
		int i = do_lookup(rkey, hash);
		if (i >= 0)
			return std::pair<iterator, bool>(iterator(this, i), false);
		i = do_insert(std::make_pair(std::forward<K>(rkey), value), hash);
		return std::pair<iterator, bool>(iterator(this, i), true);
	}

	std::pair<iterator, bool> emplace(K &&rkey, T &&rvalue)
	{
		Hasher::hash_t hash = do_hash(rkey);
		int i = do_lookup(rkey, hash);
		if (i >= 0)
			return std::pair<iterator, bool>(iterator(this, i), false);
		i = do_insert(std::make_pair(std::forward<K>(rkey), std::forward<T>(rvalue)), hash);
		return std::pair<iterator, bool>(iterator(this, i), true);
	}

	int erase(const K &key)
	{
		Hasher::hash_t hash = do_hash(key);
		int index = do_lookup(key, hash);
		return do_erase(index, hash);
	}

	iterator erase(iterator it)
	{
		Hasher::hash_t hash = do_hash(it->first);
		do_erase(it.index, hash);
		return ++it;
	}

	int count(const K &key) const
	{
		Hasher::hash_t hash = do_hash(key);
		int i = do_lookup_no_rehash(key, hash);
		return i < 0 ? 0 : 1;
	}

	int count(const K &key, const_iterator it) const
	{
		Hasher::hash_t hash = do_hash(key);
		int i = do_lookup_no_rehash(key, hash);
		return i < 0 || i > it.index ? 0 : 1;
	}

	iterator find(const K &key)
	{
		Hasher::hash_t hash = do_hash(key);
		int i = do_lookup(key, hash);
		if (i < 0)
			return end();
		return iterator(this, i);
	}

	const_iterator find(const K &key) const
	{
		Hasher::hash_t hash = do_hash(key);
		int i = do_lookup_no_rehash(key, hash);
		if (i < 0)
			return end();
		return const_iterator(this, i);
	}

	T& at(const K &key)
	{
		Hasher::hash_t hash = do_hash(key);
		int i = do_lookup(key, hash);
		if (i < 0)
			throw std::out_of_range("dict::at()");
		return entries[i].udata.second;
	}

	const T& at(const K &key) const
	{
		Hasher::hash_t hash = do_hash(key);
		int i = do_lookup_no_rehash(key, hash);
		if (i < 0)
			throw std::out_of_range("dict::at()");
		return entries[i].udata.second;
	}

	const T& at(const K &key, const T &defval) const
	{
		Hasher::hash_t hash = do_hash(key);
		int i = do_lookup_no_rehash(key, hash);
		if (i < 0)
			return defval;
		return entries[i].udata.second;
	}

	T& operator[](const K &key)
	{
		Hasher::hash_t hash = do_hash(key);
		int i = do_lookup(key, hash);
		if (i < 0)
			i = do_insert(std::pair<K, T>(key, T()), hash);
		return entries[i].udata.second;
	}

	template<typename Compare = std::less<K>>
	void sort(Compare comp = Compare())
	{
		do_compact();
		std::vector<int> perm(entries.size());
		for (int i = 0; i < int(perm.size()); i++)
			perm[i] = i;
		std::stable_sort(perm.begin(), perm.end(), [&](int a, int b){ return comp(entries[b].udata.first, entries[a].udata.first); });
		do_permute(perm);
	}

	void swap(dict &other)
	{
		hashtable.swap(other.hashtable);
		entries.swap(other.entries);
		std::swap(n_holes, other.n_holes);
		std::swap(first_hole, other.first_hole);
	}

	bool operator==(const dict &other) const {
		if (size() != other.size())
			return false;
		for (auto &it : entries) {
			if (it.is_hole())
				continue;
			auto oit = other.find(it.udata.first);
			if (oit == other.end() || !(oit->second == it.udata.second))
				return false;
		}
		return true;
	}

	bool operator!=(const dict &other) const {
		return !operator==(other);
	}

	[[nodiscard]] Hasher hash_into(Hasher h) const {
		commutative_hash comm;
		for (auto &it : entries) {
			if (it.is_hole())
				continue;
			Hasher entry_hash;
			entry_hash.eat(it.udata.first);
			entry_hash.eat(it.udata.second);
			comm.eat(entry_hash);
		}
		return comm.hash_into(h);
	}

	void reserve(size_t n) { entries.reserve(n); }
	size_t size() const { return entries.size() - n_holes; }
	bool empty() const { return entries.empty(); }
	void clear() { hashtable.clear(); entries.clear(); n_holes = 0; }

	// Squeeze out the holes left by erase(). This invalidates iterators.
	// element() is only O(1) on a dict without holes.
	void compact() { do_compact(); }

	iterator begin() { return iterator(this, int(entries.size())-1); }
	iterator element(int n) { return iterator(this, element_index(n)); }
	iterator end() { return iterator(nullptr, -1); }

	const_iterator begin() const { return const_iterator(this, int(entries.size())-1); }
	const_iterator element(int n) const { return const_iterator(this, element_index(n)); }
	const_iterator end() const { return const_iterator(this, -1); }
};

template<typename K, typename OPS>
class HASHLIB_ATTRIBUTE_WARN_UNUSED pool
{
	template<typename, int, typename> friend class idict;

protected:
	typedef hashtable_entry<K> entry_t;

	std::vector<int> hashtable;
	std::vector<entry_t> entries;
	int n_holes = 0;
	int first_hole = 0; // only meaningful if n_holes > 0
	OPS ops;
	static_assert(std::is_nothrow_default_constructible_v<OPS>, "move ops are noexcept and default-construct OPS");

#ifdef NDEBUG
	static inline void do_assert(bool) { }
#else
	static inline void do_assert(bool cond) {
		if (!cond) throw std::runtime_error("pool<> assert failed.");
	}
#endif

	Hasher::hash_t do_hash(const K &key) const
	{
		Hasher::hash_t hash = 0;
		if (!hashtable.empty())
			hash = ops.hash(key).yield() % (unsigned int)(hashtable.size());
		return hash;
	}

	void do_rehash()
	{
		hashtable.clear();
		hashtable.resize(hashtable_size(entries.capacity() * hashtable_size_factor), -1);

		for (int i = 0; i < int(entries.size()); i++) {
			if (entries[i].is_hole())
				continue;
			Hasher::hash_t hash = do_hash(entries[i].udata);
			entries[i].next = hashtable[hash];
			hashtable[hash] = i;
		}
	}

	// Squeeze out the holes, assuming there are none below index. Entries
	// below index stay where they are.
	void do_compact_from(int index)
	{
		int w = index;
		for (int r = index; r < int(entries.size()); r++) {
			if (entries[r].is_hole())
				continue;
			if (r != w)
				entries[w].move_from(entries[r]);
			w++;
		}
		entries.erase(entries.begin() + w, entries.end());
		n_holes = 0;
		do_rehash();
	}

	void do_compact()
	{
		if (n_holes > 0)
			do_compact_from(first_hole);
	}

	void do_permute(const std::vector<int> &perm)
	{
		std::vector<entry_t> new_entries(perm.size());
		for (int i = 0; i < int(perm.size()); i++)
			new_entries[i].move_from(entries[perm[i]]);
		entries.swap(new_entries);
		do_rehash();
	}

	void do_copy(const pool &other)
	{
		entries.clear();
		n_holes = 0;
		if (other.n_holes == 0) {
			entries = other.entries;
		} else {
			entries.reserve(other.size());
			for (auto &it : other.entries)
				if (!it.is_hole())
					entries.push_back(it);
		}
		do_rehash();
	}

	int do_erase(int index, Hasher::hash_t hash)
	{
		do_assert(index < int(entries.size()));
		if (hashtable.empty() || index < 0)
			return 0;

		int k = hashtable[hash];
		if (k == index) {
			hashtable[hash] = entries[index].next;
		} else {
			while (entries[k].next != index) {
				k = entries[k].next;
				do_assert(0 <= k && k < int(entries.size()));
			}
			entries[k].next = entries[index].next;
		}

		entries[index].make_hole();
		first_hole = n_holes > 0 ? std::min(first_hole, index) : index;
		n_holes++;

		while (!entries.empty() && entries.back().is_hole()) {
			entries.pop_back();
			n_holes--;
		}

		// Once half the entries are holes, compact if that only moves
		// entries above the erased one. Those are the ones that iterators
		// which haven't passed the erased entry yet would still visit, and
		// erase() is allowed to invalidate such iterators.
		if (entries.empty())
			hashtable.clear();
		else if (2 * n_holes >= int(entries.size()) && first_hole == index)
			do_compact_from(index);

		return 1;
	}

	// See dict::iter_next()
	int iter_next(int index) const
	{
		index = std::min(index, int(entries.size())) - 1;
		while (index >= 0 && entries[index].is_hole())
			index--;
		return index;
	}

	int element_index(int n) const
	{
		int index = int(entries.size())-1;
		if (n_holes == 0)
			return index-n;
		while (n-- > 0 && index >= 0)
			index = iter_next(index);
		return index;
	}

	int do_lookup(const K &key, Hasher::hash_t &hash)
	{
		if (hashtable.empty())
			return -1;

		if (entries.size() * hashtable_size_trigger > hashtable.size()) {
			do_rehash();
			hash = do_hash(key);
		}

		return do_lookup_internal(key, hash);
	}

	int do_lookup_internal(const K &key, Hasher::hash_t hash) const
	{
		int index = hashtable[hash];

		while (index >= 0 && !ops.cmp(entries[index].udata, key)) {
			index = entries[index].next;
			do_assert(-1 <= index && index < int(entries.size()));
		}

		return index;
	}

	int do_lookup_no_rehash(const K &key, Hasher::hash_t hash) const
	{
		if (hashtable.empty())
			return -1;

		return do_lookup_internal(key, hash);
	}

	int do_insert(const K &value, Hasher::hash_t &hash)
	{
		if (hashtable.empty()) {
			entries.emplace_back(value, -1);
			do_rehash();
			hash = do_hash(value);
		} else {
			entries.emplace_back(value, hashtable[hash]);
			hashtable[hash] = entries.size() - 1;
		}
		return entries.size() - 1;
	}

	int do_insert(K &&rvalue, Hasher::hash_t &hash)
	{
		if (hashtable.empty()) {
			entries.emplace_back(std::forward<K>(rvalue), -1);
			do_rehash();
			hash = do_hash(entries.back().udata);
		} else {
			entries.emplace_back(std::forward<K>(rvalue), hashtable[hash]);
			hashtable[hash] = entries.size() - 1;
		}
		return entries.size() - 1;
	}

public:
	class const_iterator
	{
		friend class pool;
	protected:
		const pool *ptr;
		int index;
		const_iterator(const pool *ptr, int index) : ptr(ptr), index(index) { }
	public:
		typedef std::forward_iterator_tag iterator_category;
		typedef K value_type;
		typedef ptrdiff_t difference_type;
		typedef K* pointer;
		typedef K& reference;
		const_iterator() { }
		const_iterator operator++() { index = ptr->iter_next(index); return *this; }
		bool operator==(const const_iterator &other) const { return index == other.index; }
		bool operator!=(const const_iterator &other) const { return index != other.index; }
		const K &operator*() const { return ptr->entries[index].udata; }
		const K *operator->() const { return &ptr->entries[index].udata; }
	};

	class iterator
	{
		friend class pool;
	protected:
		pool *ptr;
		int index;
		iterator(pool *ptr, int index) : ptr(ptr), index(index) { }
	public:
		typedef std::forward_iterator_tag iterator_category;
		typedef K value_type;
		typedef ptrdiff_t difference_type;
		typedef K* pointer;
		typedef K& reference;
		iterator() { }
		iterator operator++() { index = ptr->iter_next(index); return *this; }
		bool operator==(const iterator &other) const { return index == other.index; }
		bool operator!=(const iterator &other) const { return index != other.index; }
		K &operator*() { return ptr->entries[index].udata; }
		K *operator->() { return &ptr->entries[index].udata; }
		const K &operator*() const { return ptr->entries[index].udata; }
		const K *operator->() const { return &ptr->entries[index].udata; }
		operator const_iterator() const { return const_iterator(ptr, index); }
	};

	constexpr pool()
	{
	}

	pool(const pool &other)
	{
		do_copy(other);
	}

	pool(pool &&other) noexcept
	{
		swap(other);
	}

	pool &operator=(const pool &other) {
		if (this != &other)
			do_copy(other);
		return *this;
	}

	pool &operator=(pool &&other) noexcept {
		clear();
		swap(other);
		return *this;
	}

	pool(const std::initializer_list<K> &list)
	{
		for (auto &it : list)
			insert(it);
	}

	template<class InputIterator>
	pool(InputIterator first, InputIterator last)
	{
		insert(first, last);
	}

	template<class InputIterator>
	void insert(InputIterator first, InputIterator last)
	{
		for (; first != last; ++first)
			insert(*first);
	}

	std::pair<iterator, bool> insert(const K &value)
	{
		Hasher::hash_t hash = do_hash(value);
		int i = do_lookup(value, hash);
		if (i >= 0)
			return std::pair<iterator, bool>(iterator(this, i), false);
		i = do_insert(value, hash);
		return std::pair<iterator, bool>(iterator(this, i), true);
	}

	std::pair<iterator, bool> insert(K &&rvalue)
	{
		Hasher::hash_t hash = do_hash(rvalue);
		int i = do_lookup(rvalue, hash);
		if (i >= 0)
			return std::pair<iterator, bool>(iterator(this, i), false);
		i = do_insert(std::forward<K>(rvalue), hash);
		return std::pair<iterator, bool>(iterator(this, i), true);
	}

	template<typename... Args>
	std::pair<iterator, bool> emplace(Args&&... args)
	{
		return insert(K(std::forward<Args>(args)...));
	}

	int erase(const K &key)
	{
		Hasher::hash_t hash = do_hash(key);
		int index = do_lookup(key, hash);
		return do_erase(index, hash);
	}

	iterator erase(iterator it)
	{
		Hasher::hash_t hash = do_hash(*it);
		do_erase(it.index, hash);
		return ++it;
	}

	int count(const K &key) const
	{
		Hasher::hash_t hash = do_hash(key);
		int i = do_lookup_no_rehash(key, hash);
		return i < 0 ? 0 : 1;
	}

	int count(const K &key, const_iterator it) const
	{
		Hasher::hash_t hash = do_hash(key);
		int i = do_lookup_no_rehash(key, hash);
		return i < 0 || i > it.index ? 0 : 1;
	}

	iterator find(const K &key)
	{
		Hasher::hash_t hash = do_hash(key);
		int i = do_lookup(key, hash);
		if (i < 0)
			return end();
		return iterator(this, i);
	}

	const_iterator find(const K &key) const
	{
		Hasher::hash_t hash = do_hash(key);
		int i = do_lookup_no_rehash(key, hash);
		if (i < 0)
			return end();
		return const_iterator(this, i);
	}

	bool operator[](const K &key)
	{
		Hasher::hash_t hash = do_hash(key);
		int i = do_lookup(key, hash);
		return i >= 0;
	}

	template<typename Compare = std::less<K>>
	void sort(Compare comp = Compare())
	{
		do_compact();
		std::vector<int> perm(entries.size());
		for (int i = 0; i < int(perm.size()); i++)
			perm[i] = i;
		std::stable_sort(perm.begin(), perm.end(), [&](int a, int b){ return comp(entries[b].udata, entries[a].udata); });
		do_permute(perm);
	}

	K pop()
	{
		iterator it = begin();
		K ret = *it;
		erase(it);
		return ret;
	}

	void swap(pool &other)
	{
		hashtable.swap(other.hashtable);
		entries.swap(other.entries);
		std::swap(n_holes, other.n_holes);
		std::swap(first_hole, other.first_hole);
	}

	bool operator==(const pool &other) const {
		if (size() != other.size())
			return false;
		for (auto &it : entries)
			if (!it.is_hole() && !other.count(it.udata))
				return false;
		return true;
	}

	bool operator!=(const pool &other) const {
		return !operator==(other);
	}

	[[nodiscard]] Hasher hash_into(Hasher h) const {
		commutative_hash comm;
		for (auto &it : entries) {
			if (!it.is_hole())
				comm.eat(ops.hash(it.udata));
		}
		return comm.hash_into(h);
	}

	void reserve(size_t n) { entries.reserve(n); }
	size_t size() const { return entries.size() - n_holes; }
	bool empty() const { return entries.empty(); }
	void clear() { hashtable.clear(); entries.clear(); n_holes = 0; }

	// See dict::compact()
	void compact() { do_compact(); }

	iterator begin() { return iterator(this, int(entries.size())-1); }
	iterator element(int n) { return iterator(this, element_index(n)); }
	iterator end() { return iterator(nullptr, -1); }

	const_iterator begin() const { return const_iterator(this, int(entries.size())-1); }
	const_iterator element(int n) const { return const_iterator(this, element_index(n)); }
	const_iterator end() const { return const_iterator(nullptr, -1); }
};

template<typename K, int offset, typename OPS>
class HASHLIB_ATTRIBUTE_WARN_UNUSED idict
{
	pool<K, OPS> database;

public:
	class const_iterator
	{
		friend class idict;
	protected:
		const idict &container;
		int index;
		const_iterator(const idict &container, int index) : container(container), index(index) { }
	public:
		typedef std::forward_iterator_tag iterator_category;
		typedef K value_type;
		typedef ptrdiff_t difference_type;
		typedef K* pointer;
		typedef K& reference;
		const_iterator() { }
		const_iterator operator++() { index++; return *this; }
		bool operator==(const const_iterator &other) const { return index == other.index; }
		bool operator!=(const const_iterator &other) const { return index != other.index; }
		const K &operator*() const { return container[index]; }
		const K *operator->() const { return &container[index]; }
	};

	constexpr idict()
	{
	}

	int operator()(const K &key)
	{
		Hasher::hash_t hash = database.do_hash(key);
		int i = database.do_lookup(key, hash);
		if (i < 0)
			i = database.do_insert(key, hash);
		return i + offset;
	}

	int at(const K &key) const
	{
		Hasher::hash_t hash = database.do_hash(key);
		int i = database.do_lookup_no_rehash(key, hash);
		if (i < 0)
			throw std::out_of_range("idict::at()");
		return i + offset;
	}

	int at(const K &key, int defval) const
	{
		Hasher::hash_t hash = database.do_hash(key);
		int i = database.do_lookup_no_rehash(key, hash);
		if (i < 0)
			return defval;
		return i + offset;
	}

	int count(const K &key) const
	{
		Hasher::hash_t hash = database.do_hash(key);
		int i = database.do_lookup_no_rehash(key, hash);
		return i < 0 ? 0 : 1;
	}

	int lookup(const K &key) const
	{
		Hasher::hash_t hash = database.do_hash(key);
		return database.do_lookup_no_rehash(key, hash);
	}

	void expect(const K &key, int i)
	{
		int j = (*this)(key);
		if (i != j)
			throw std::out_of_range("idict::expect()");
	}

	const K &operator[](int index) const
	{
		return database.entries.at(index - offset).udata;
	}

	void swap(idict &other)
	{
		database.swap(other.database);
	}

	void reserve(size_t n) { database.reserve(n); }
	size_t size() const { return database.size(); }
	bool empty() const { return database.empty(); }
	void clear() { database.clear(); }

	const_iterator begin() const { return const_iterator(*this, offset); }
	const_iterator element(int n) const { return const_iterator(*this, n); }
	const_iterator end() const { return const_iterator(*this, offset + size()); }
};

/**
 * Union-find data structure with a promotion method
 * mfp stands for "merge, find, promote"
 * i-prefixed methods operate on indices in parents
*/
template<typename K, typename OPS>
class HASHLIB_ATTRIBUTE_WARN_UNUSED mfp
{
	idict<K, 0, OPS> database;
	class AtomicParent {
	public:
		explicit AtomicParent(int p) : parent(p) {}
		AtomicParent(const AtomicParent &other) : parent(other.get()) {}
		AtomicParent &operator=(const AtomicParent &other) { set(other.get()); return *this; }
		int get() const { return parent.load(std::memory_order_relaxed); }
		void set(int p) { parent.store(p, std::memory_order_relaxed); }
	private:
		std::atomic<int> parent;
	};
	std::vector<AtomicParent> parents;

public:
	typedef typename idict<K, 0>::const_iterator const_iterator;

	constexpr mfp()
	{
	}

	// Finds a given element's index. If it isn't in the data structure,
	// it is added as its own set
	int operator()(const K &key)
	{
		int i = database(key);
		// If the lookup caused the database to grow,
		// also add a corresponding entry in parents initialized to -1 (no parent)
		if (parents.size() < database.size()) {
			parents.emplace_back(-1);
		}
		return i;
	}

	// Finds an element at given index
	const K &operator[](int index) const
	{
		return database[index];
	}

	// Why this method is correct for concurent ifind() calls:
	// Consider the mfp state after the last non-const method call before
	// a particular call to ifind(i). In this state, i's parent chain leads
	// to some root R. Let S be the set of integers s such that ifind(s) = R
	// in this state. Let 'orig_parents' be the value of 'parents' in this state.
	//
	// Now consider the concurrent calls to ifind(s), s ∈ S, before the next non-const method
	// call. Consider the atomic writes performed by various ifind() calls, in any causally
	// consistent order. The first atomic write can only set parents[k] to R, because the
	// atomic read of parents[p] in the first while loop can only observe the value
	// 'orig_parents[p]'. Subsequent writes can also only set parents[k] to R, because the
	// parents[p] reads either observe 'orig_parents[p]' or R (and observing R ends the first
	// while loop immediately). Thus all parents[p] reads observe either 'orig_parents[p]'
	// or R, so ifind() always returns R.
	int ifind(int i) const
	{
		int p = i, k = i;

		while (true) {
			int pp = parents[p].get();
			if (pp < 0)
				break;
			p = pp;
		}
		// p is now the representative of i
		// Now we traverse from i up to the representative again
		// and make p the parent of all the nodes along the way.
		// This is a side effect and doesn't affect the return value.
		// It speeds up future find operations
		while (k != p) {
			int next_k = parents[k].get();
			const_cast<AtomicParent*>(&parents[k])->set(p);
			k = next_k;
		}

		return p;
	}

	// Merge sets if the given indices belong to different sets.
	// Makes ifind(j) the root of the merged set.
	void imerge(int i, int j)
	{
		i = ifind(i);
		j = ifind(j);

		if (i != j)
			parents[i].set(j);
	}

	void ipromote(int i)
	{
		int k = i;

		while (k != -1) {
			int next_k = parents[k].get();
			parents[k].set(i);
			k = next_k;
		}

		parents[i].set(-1);
	}

	int lookup(const K &a)
	{
		return ifind((*this)(a));
	}

	const K &find(const K &a) const
	{
		int i = database.at(a, -1);
		if (i < 0)
			return a;
		return (*this)[ifind(i)];
	}

	void merge(const K &a, const K &b)
	{
		imerge((*this)(a), (*this)(b));
	}

	void promote(const K &a)
	{
		int i = database.at(a, -1);
		if (i >= 0)
			ipromote(i);
	}

	void swap(mfp &other)
	{
		database.swap(other.database);
		parents.swap(other.parents);
	}

	void reserve(size_t n) { database.reserve(n); }
	size_t size() const { return database.size(); }
	bool empty() const { return database.empty(); }
	void clear() { database.clear(); parents.clear(); }

	const_iterator begin() const { return database.begin(); }
	const_iterator element(int n) const { return database.element(n); }
	const_iterator end() const { return database.end(); }
};

} /* namespace hashlib */

#endif
