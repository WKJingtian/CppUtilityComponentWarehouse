#pragma once
#include <assert.h>
#include <list>
#include <unordered_map>

template<typename VAL>
class DefaultValueDeleter
{
public:
	void operator() (VAL& v)
	{
		if constexpr (std::is_pointer_v<VAL>) {
			delete v;
			v = nullptr;
		}
	}
};

template<typename KEY, typename VAL, class ValueDeleter = DefaultValueDeleter<VAL>>
class LRUCache
{
private:
	struct Node
	{
		KEY k;
		VAL v;
		int refCount;

		Node(const KEY& k, VAL&& v) :
			k(k), v(std::move(v)), refCount(1) {}

		Node(const KEY& k, const VAL& v) :
			k(k), v(v), refCount(1) {}
	};

public:
	using Iter = typename std::list<Node>::iterator;

private:
	int _cacheCapacity;
	int _size;
	std::list<Node> _nodesNotInUse;
	std::list<Node> _nodesInUse;
	std::unordered_map<KEY, Iter> _kvMap;
	ValueDeleter _deleter;

public:
	class Handle
	{
		friend class LRUCache<KEY, VAL>;

		LRUCache* _cache;
		Node* _node;

		Handle() : _cache(nullptr), _node(nullptr) {}
		Handle(LRUCache* cache, Node* node) :
			_cache(cache), _node(node)
		{
			if (IsValid())
				_cache->OnNodeRefered(node);
		}

		void Reset()
		{
			if (IsValid())
			{
				_cache->OnNodeRefFreed(_node);
				_cache = nullptr;
				_node = nullptr;
			}
		}

	public:
		~Handle()
		{
			Reset();
		}

		Handle(const Handle&) = delete;
		Handle& operator= (const Handle&) = delete;
		Handle& operator= (Handle&&) noexcept = delete;

		Handle(Handle&& other) noexcept :
			_cache(other._cache), _node(other._node)
		{
			other._cache = nullptr;
			other._node = nullptr;
		}

		bool IsValid() const { return _node != nullptr && _cache != nullptr; }

		VAL& Get()
		{
			assert(IsValid());
			return _node->v;
		}
	};

	Handle Get(const KEY& k)
	{
		auto target = _kvMap.find(k);
		if (target != _kvMap.end())
		{
			return Handle(this, &(*target->second));
		}
		return Handle();
	}

	template <typename V>
	Handle Put(const KEY& k, V&& v)
	{
		static_assert(std::is_constructible_v<VAL, V&&>, "LRUCache: Invalid Put Arg Type");

		auto target = _kvMap.find(k);
		if (target != _kvMap.end())
		{
			target->second->v = std::forward<V>(v);
			return Handle(this, &(*target->second));
		}

		_size++;
		OnNewNodeGenerated(); // trim the cache first to prevent removing the new node

		auto newNode = _nodesNotInUse.emplace(_nodesNotInUse.end(), k, std::forward<V>(v));
		_kvMap[k] = newNode;
		return Handle(this, &(*newNode));
	}

	LRUCache(int cap) :
		_cacheCapacity(cap),
		_size(0),
		_nodesNotInUse(std::list<Node>()),
		_nodesInUse(std::list<Node>()),
		_kvMap(std::unordered_map<KEY, Iter>())
	{ }

	~LRUCache()
	{
		assert(_nodesInUse.empty());
		_cacheCapacity = 0;
		OnNewNodeGenerated(); // reuse this code to clean up the list
	}

	LRUCache(const LRUCache&) = delete;
	LRUCache& operator= (const LRUCache&) = delete;
	LRUCache& operator= (LRUCache&&) noexcept = delete;
	LRUCache(LRUCache&& other) noexcept = delete;

private:
	void OnNodeRefered(Node* node)
	{
		auto it = _kvMap.find(node->k); assert(it != _kvMap.end());
		assert(_kvMap.contains(node->k));
		if (node->refCount == 1)
		{
			_nodesInUse.splice(_nodesInUse.end(), _nodesNotInUse, it->second);
		}
		node->refCount++;
	}

	void OnNodeRefFreed(Node* node)
	{
		auto it = _kvMap.find(node->k); assert(it != _kvMap.end());
		assert(node->refCount > 1);
		node->refCount--;
		if (node->refCount == 1)
		{
			_nodesNotInUse.splice(_nodesNotInUse.end(), _nodesInUse, it->second);
		}
	}

	void OnNewNodeGenerated()
	{
		while (_size > _cacheCapacity && !_nodesNotInUse.empty())
		{
			auto toRemove = _nodesNotInUse.begin();
			auto key = toRemove->k;
			_deleter(toRemove->v);
			_nodesNotInUse.erase(toRemove);
			_kvMap.erase(key);
			_size--;
		}
	}
};