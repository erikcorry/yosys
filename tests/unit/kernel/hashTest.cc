#include <gtest/gtest.h>
#include "kernel/yosys_common.h"

#include <algorithm>
#include <memory>
#include <unordered_set>

YOSYS_NAMESPACE_BEGIN

static Hasher hash(int x)
{
	Hasher h;
	h.eat(x);
	return h;
}

TEST(CommutativeTest, basic)
{
	hashlib::commutative_hash comm1;
	comm1.eat(hash(1));
	comm1.eat(hash(2));
	hashlib::commutative_hash comm2;
	comm2.eat(hash(2));
	comm2.eat(hash(1));
	EXPECT_EQ(comm1.hash_into(Hasher()).yield(), comm2.hash_into(Hasher()).yield());
}

TEST(PoolHashTest, collisions)
{
	uint64_t collisions = 0;
	std::unordered_set<Hasher::hash_t> hashes;
	for (int i = 0; i < 1000; ++i) {
		for (int j = i + 1; j < 1000; ++j) {
			pool<int> p1;
			p1.insert(i);
			p1.insert(j);
			auto h = p1.hash_into(Hasher()).yield();
			if (!hashes.insert(h).second) {
				++collisions;
			}
		}
	}
	std::cout << "pool<int> collisions: " << collisions << std::endl;
	EXPECT_LT(collisions, 10'000);
}

TEST(PoolHashTest, subset_collisions)
{
	uint64_t collisions = 0;
	std::unordered_set<Hasher::hash_t> hashes;
	for (int i = 0; i < 1000 * 1000; ++i) {

		pool<int> p1;
		for (int b = 0; i >> b; ++b) {
			if ((i >> b) & 1) {
				p1.insert(b);
			}
		}
		auto h = p1.hash_into(Hasher()).yield();
		if (!hashes.insert(h).second) {
			++collisions;
		}

	}
	std::cout << "pool<int> subset collisions: " << collisions << std::endl;
	EXPECT_LT(collisions, 100);
}

// Iteration order of a pool, which is newest first
template<typename K>
static std::vector<K> order(const pool<K> &p)
{
	return std::vector<K>(p.begin(), p.end());
}

TEST(PoolOrderTest, erase_keeps_order)
{
	pool<int> p;
	for (int i = 0; i < 10; i++)
		p.insert(i);
	p.erase(3);
	p.erase(7);
	p.erase(0);
	EXPECT_EQ(order(p), (std::vector<int>{9, 8, 6, 5, 4, 2, 1}));
	EXPECT_EQ(p.size(), 7u);
	p.insert(3);
	EXPECT_EQ(order(p), (std::vector<int>{3, 9, 8, 6, 5, 4, 2, 1}));
	for (int i = 0; i < GetSize(p); i++)
		EXPECT_EQ(*p.element(i), order(p)[i]);
}

TEST(PoolOrderTest, erase_order_independent)
{
	pool<int> a, b;
	for (int i = 0; i < 100; i++) {
		a.insert(i);
		b.insert(i);
	}
	for (int i = 0; i < 100; i += 3)
		a.erase(i);
	for (int i = 99; i >= 0; i--)
		if (i % 3 == 0)
			b.erase(i);
	EXPECT_EQ(order(a), order(b));
}

TEST(PoolOrderTest, erase_while_iterating)
{
	pool<int> p;
	for (int i = 0; i < 1000; i++)
		p.insert(i);
	std::vector<int> visited;
	for (auto it = p.begin(); it != p.end();) {
		visited.push_back(*it);
		if (*it % 3 != 0)
			it = p.erase(it);
		else
			++it;
	}
	EXPECT_EQ(GetSize(visited), 1000);
	std::vector<int> expected;
	for (int i = 999; i >= 0; i--)
		if (i % 3 == 0)
			expected.push_back(i);
	EXPECT_EQ(order(p), expected);

	// Erasing the current element by key in a range-based for loop
	visited.clear();
	for (int x : p) {
		visited.push_back(x);
		if (x % 2 == 0)
			p.erase(x);
	}
	EXPECT_EQ(GetSize(visited), GetSize(expected));
	for (int x : p)
		EXPECT_EQ(x % 2, 1);
}

TEST(PoolOrderTest, insert_while_iterating)
{
	pool<int> p;
	for (int i = 0; i < 100; i++)
		p.insert(i);
	for (int i = 0; i < 90; i++)
		p.erase(i * 7 % 100);
	std::vector<int> before = order(p);
	std::vector<int> visited;
	for (int x : p) {
		visited.push_back(x);
		p.insert(x + 1000);
	}
	EXPECT_EQ(visited, before);
}

TEST(PoolOrderTest, random_against_model)
{
	pool<int> p;
	std::vector<int> model; // insertion order, oldest first
	uint32_t rng = 1;
	auto next = [&]() { rng = rng * 1103515245 + 12345; return (rng >> 16) & 0x7fff; };
	for (int step = 0; step < 20000; step++) {
		int key = next() % 300;
		if (next() % 3 == 0) {
			auto it = std::find(model.begin(), model.end(), key);
			EXPECT_EQ(p.erase(key), it != model.end() ? 1 : 0);
			if (it != model.end())
				model.erase(it);
		} else {
			bool inserted = p.insert(key).second;
			bool absent = std::find(model.begin(), model.end(), key) == model.end();
			EXPECT_EQ(inserted, absent);
			if (absent)
				model.push_back(key);
		}
		if (step % 97 == 0) {
			std::vector<int> expected(model.rbegin(), model.rend());
			ASSERT_EQ(order(p), expected);
			ASSERT_EQ(p.size(), model.size());
			for (int k = 0; k < 300; k++)
				ASSERT_EQ(p.count(k) != 0, std::find(model.begin(), model.end(), k) != model.end());
			if (!model.empty())
				ASSERT_EQ(*p.element(GetSize(model) / 2), expected[model.size() / 2]);
			pool<int> copy = p;
			ASSERT_EQ(order(copy), expected);
			ASSERT_TRUE(copy == p);
		}
	}
}

TEST(DictOrderTest, erase_destroys_value)
{
	auto value = std::make_shared<int>(42);
	dict<int, std::shared_ptr<int>> d;
	d[1] = value;
	d[2] = value;
	d[3] = value;
	EXPECT_EQ(value.use_count(), 4);
	d.erase(2);
	EXPECT_EQ(value.use_count(), 3);
	std::vector<int> keys;
	for (auto &it : d)
		keys.push_back(it.first);
	EXPECT_EQ(keys, (std::vector<int>{3, 1}));
	auto rkeys = std::vector<int>();
	for (auto it = d.rbegin(); it != d.rend(); ++it)
		rkeys.push_back(it->first);
	EXPECT_EQ(rkeys, (std::vector<int>{1, 3}));
	d.clear();
	EXPECT_EQ(value.use_count(), 1);
}

YOSYS_NAMESPACE_END
