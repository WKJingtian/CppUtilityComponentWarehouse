#pragma once
#include <array>
#include <cassert>
#include <cstddef>
#include <functional>
#include <memory>
#include <random>
#include <utility>

template<typename T, typename Compare = std::less<T>, int MaxLevel = 24>
class IndexedSkipList
{
	static_assert(MaxLevel > 0, "IndexedSkipList: MaxLevel must be positive");

private:
	struct Node
	{
		std::unique_ptr<T> value;
		std::array<Node*, MaxLevel> next;
		std::array<std::size_t, MaxLevel> span;

		Node() : value(nullptr)
		{
			next.fill(nullptr);
			span.fill(0);
		}

		template<typename U>
		explicit Node(U&& v) : value(std::make_unique<T>(std::forward<U>(v)))
		{
			next.fill(nullptr);
			span.fill(0);
		}
	};

	Node* _head;
	std::size_t _size;
	int _level;
	Compare _comp;
	std::mt19937 _rng;
	std::uniform_real_distribution<double> _dist;

public:
	IndexedSkipList() :
		_head(new Node()),
		_size(0),
		_level(1),
		_comp(Compare()),
		_rng(std::random_device{}()),
		_dist(0.0, 1.0)
	{
	}

	explicit IndexedSkipList(Compare comp) :
		_head(new Node()),
		_size(0),
		_level(1),
		_comp(std::move(comp)),
		_rng(std::random_device{}()),
		_dist(0.0, 1.0)
	{
	}

	~IndexedSkipList()
	{
		Clear();
		delete _head;
		_head = nullptr;
	}

	IndexedSkipList(const IndexedSkipList&) = delete;
	IndexedSkipList& operator= (const IndexedSkipList&) = delete;
	IndexedSkipList(IndexedSkipList&&) = delete;
	IndexedSkipList& operator= (IndexedSkipList&&) = delete;

	std::size_t Size() const { return _size; }
	bool Empty() const { return _size == 0; }

	void Clear()
	{
		Node* current = _head->next[0];
		while (current)
		{
			Node* next = current->next[0];
			delete current;
			current = next;
		}
		_head->next.fill(nullptr);
		_head->span.fill(0);
		_size = 0;
		_level = 1;
	}

	bool Contains(const T& value) const
	{
		return Find(value) != nullptr;
	}

	T* Find(const T& value)
	{
		Node* x = _head;
		for (int i = _level - 1; i >= 0; --i)
		{
			while (x->next[i] && _comp(*x->next[i]->value, value))
			{
				x = x->next[i];
			}
		}
		x = x->next[0];
		if (x && Equals(*x->value, value))
		{
			return x->value.get();
		}
		return nullptr;
	}

	const T* Find(const T& value) const
	{
		const Node* x = _head;
		for (int i = _level - 1; i >= 0; --i)
		{
			while (x->next[i] && _comp(*x->next[i]->value, value))
			{
				x = x->next[i];
			}
		}
		x = x->next[0];
		if (x && Equals(*x->value, value))
		{
			return x->value.get();
		}
		return nullptr;
	}

	bool Insert(T value)
	{
		std::array<Node*, MaxLevel> update{};
		std::array<std::size_t, MaxLevel> rank{};
		Node* x = _head;

		for (int i = _level - 1; i >= 0; --i)
		{
			rank[i] = (i == _level - 1) ? 0 : rank[i + 1];
			while (x->next[i] && _comp(*x->next[i]->value, value))
			{
				rank[i] += x->span[i];
				x = x->next[i];
			}
			update[i] = x;
		}

		x = x->next[0];
		if (x && Equals(*x->value, value))
		{
			return false;
		}

		int newLevel = RandomLevel();
		if (newLevel > _level)
		{
			for (int i = _level; i < newLevel; ++i)
			{
				rank[i] = 0;
				update[i] = _head;
				_head->span[i] = _size;
			}
			_level = newLevel;
		}

		Node* node = new Node(std::move(value));
		for (int i = 0; i < newLevel; ++i)
		{
			node->next[i] = update[i]->next[i];
			update[i]->next[i] = node;

			node->span[i] = update[i]->span[i] - (rank[0] - rank[i]);
			update[i]->span[i] = (rank[0] - rank[i]) + 1;
		}

		for (int i = newLevel; i < _level; ++i)
		{
			update[i]->span[i] += 1;
		}

		_size++;
		return true;
	}

	bool Erase(const T& value)
	{
		std::array<Node*, MaxLevel> update{};
		Node* x = _head;
		for (int i = _level - 1; i >= 0; --i)
		{
			while (x->next[i] && _comp(*x->next[i]->value, value))
			{
				x = x->next[i];
			}
			update[i] = x;
		}

		Node* target = x->next[0];
		if (!target || !Equals(*target->value, value))
		{
			return false;
		}
		RemoveNode(target, update);
		return true;
	}

	bool EraseAt(std::size_t index)
	{
		if (index >= _size)
		{
			return false;
		}

		std::array<Node*, MaxLevel> update{};
		Node* x = _head;
		std::size_t traversed = 0;
		for (int i = _level - 1; i >= 0; --i)
		{
			while (x->next[i] && traversed + x->span[i] <= index)
			{
				traversed += x->span[i];
				x = x->next[i];
			}
			update[i] = x;
		}

		Node* target = update[0]->next[0];
		if (!target)
		{
			return false;
		}
		RemoveNode(target, update);
		return true;
	}

	T* At(std::size_t index)
	{
		if (index >= _size)
		{
			return nullptr;
		}

		Node* x = _head;
		std::size_t traversed = 0;
		for (int i = _level - 1; i >= 0; --i)
		{
			while (x->next[i] && traversed + x->span[i] <= index)
			{
				traversed += x->span[i];
				x = x->next[i];
			}
		}

		Node* target = x->next[0];
		return target ? target->value.get() : nullptr;
	}

	const T* At(std::size_t index) const
	{
		if (index >= _size)
		{
			return nullptr;
		}

		const Node* x = _head;
		std::size_t traversed = 0;
		for (int i = _level - 1; i >= 0; --i)
		{
			while (x->next[i] && traversed + x->span[i] <= index)
			{
				traversed += x->span[i];
				x = x->next[i];
			}
		}

		const Node* target = x->next[0];
		return target ? target->value.get() : nullptr;
	}

	T& operator[] (std::size_t index)
	{
		T* p = At(index);
		assert(p && "IndexedSkipList: index out of range");
		return *p;
	}

	const T& operator[] (std::size_t index) const
	{
		const T* p = At(index);
		assert(p && "IndexedSkipList: index out of range");
		return *p;
	}

private:
	int RandomLevel()
	{
		constexpr double kP = 0.5;
		int level = 1;
		while (level < MaxLevel && _dist(_rng) < kP)
		{
			level++;
		}
		return level;
	}

	bool Equals(const T& a, const T& b) const
	{
		return !_comp(a, b) && !_comp(b, a);
	}

	void RemoveNode(Node* target, std::array<Node*, MaxLevel>& update)
	{
		for (int i = 0; i < _level; ++i)
		{
			if (update[i]->next[i] == target)
			{
				update[i]->span[i] += target->span[i] - 1;
				update[i]->next[i] = target->next[i];
			}
			else
			{
				update[i]->span[i] -= 1;
			}
		}

		delete target;
		_size--;
		while (_level > 1 && _head->next[_level - 1] == nullptr)
		{
			_level--;
		}
	}
};
