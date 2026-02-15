#pragma once
#include <assert.h>
#include <cstddef>
#include <functional>
#include <type_traits>
#include <utility>

template<typename KEY, typename VAL, typename Compare = std::less<KEY>>
class RBTree
{
private:
	enum class Color : unsigned char
	{
		Red,
		Black
	};

	struct Node
	{
		KEY key;
		VAL value;
		Color color;
		Node* parent;
		Node* left;
		Node* right;

		Node(const KEY& k, const VAL& v) :
			key(k), value(v), color(Color::Red), parent(nullptr), left(nullptr), right(nullptr) {}

		Node(const KEY& k, VAL&& v) :
			key(k), value(std::move(v)), color(Color::Red), parent(nullptr), left(nullptr), right(nullptr) {}

		Node(KEY&& k, VAL&& v) :
			key(std::move(k)), value(std::move(v)), color(Color::Red), parent(nullptr), left(nullptr), right(nullptr) {}
	};

	Node* _root;
	std::size_t _size;
	Compare _comp;

public:
	RBTree() :
		_root(nullptr),
		_size(0),
		_comp(Compare()) {}

	explicit RBTree(Compare comp) :
		_root(nullptr),
		_size(0),
		_comp(std::move(comp)) {}

	~RBTree()
	{
		Clear();
	}

	RBTree(const RBTree&) = delete;
	RBTree& operator= (const RBTree&) = delete;

	RBTree(RBTree&& other) noexcept :
		_root(other._root),
		_size(other._size),
		_comp(std::move(other._comp))
	{
		other._root = nullptr;
		other._size = 0;
	}

	RBTree& operator= (RBTree&& other) noexcept
	{
		if (this != &other)
		{
			Clear();
			_root = other._root;
			_size = other._size;
			_comp = std::move(other._comp);
			other._root = nullptr;
			other._size = 0;
		}
		return *this;
	}

	std::size_t Size() const { return _size; }
	bool Empty() const { return _size == 0; }

	void Clear()
	{
		DestroySubtree(_root);
		_root = nullptr;
		_size = 0;
	}

	bool Contains(const KEY& key) const
	{
		return FindNode(key) != nullptr;
	}

	VAL* Find(const KEY& key)
	{
		Node* node = FindNode(key);
		return node ? &node->value : nullptr;
	}

	const VAL* Find(const KEY& key) const
	{
		const Node* node = FindNode(key);
		return node ? &node->value : nullptr;
	}

	template<typename K, typename V>
	bool Insert(K&& key, V&& value)
	{
		static_assert(std::is_constructible_v<KEY, K&&>, "RedBlackTree: Invalid key type");
		static_assert(std::is_constructible_v<VAL, V&&>, "RedBlackTree: Invalid value type");

		Node* parent = nullptr;
		Node* current = _root;
		while (current)
		{
			parent = current;
			if (_comp(key, current->key))
			{
				current = current->left;
			}
			else if (_comp(current->key, key))
			{
				current = current->right;
			}
			else
			{
				current->value = std::forward<V>(value);
				return false;
			}
		}

		Node* node = new Node(std::forward<K>(key), std::forward<V>(value));
		node->parent = parent;
		if (!parent)
		{
			_root = node;
		}
		else if (_comp(node->key, parent->key))
		{
			parent->left = node;
		}
		else
		{
			parent->right = node;
		}

		InsertFixup(node);
		_size++;
		return true;
	}

	bool Erase(const KEY& key)
	{
		Node* z = FindNode(key);
		if (!z)
		{
			return false;
		}

		Node* y = z;
		Color yOriginalColor = y->color;
		Node* x = nullptr;
		Node* xParent = nullptr;

		if (!z->left)
		{
			x = z->right;
			xParent = z->parent;
			Transplant(z, z->right);
		}
		else if (!z->right)
		{
			x = z->left;
			xParent = z->parent;
			Transplant(z, z->left);
		}
		else
		{
			y = Minimum(z->right);
			yOriginalColor = y->color;
			x = y->right;
			if (y->parent == z)
			{
				xParent = y;
				if (x)
					x->parent = y;
			}
			else
			{
				xParent = y->parent;
				Transplant(y, y->right);
				y->right = z->right;
				y->right->parent = y;
			}

			Transplant(z, y);
			y->left = z->left;
			y->left->parent = y;
			y->color = z->color;
		}

		delete z;
		_size--;

		if (yOriginalColor == Color::Black)
		{
			DeleteFixup(x, xParent);
		}
		return true;
	}

private:
	static bool IsRed(const Node* node) { return node && node->color == Color::Red; }
	static bool IsBlack(const Node* node) { return !node || node->color == Color::Black; }

	void DestroySubtree(Node* node)
	{
		if (!node)
			return;
		DestroySubtree(node->left);
		DestroySubtree(node->right);
		delete node;
	}

	Node* FindNode(const KEY& key)
	{
		Node* current = _root;
		while (current)
		{
			if (_comp(key, current->key))
			{
				current = current->left;
			}
			else if (_comp(current->key, key))
			{
				current = current->right;
			}
			else
			{
				return current;
			}
		}
		return nullptr;
	}

	const Node* FindNode(const KEY& key) const
	{
		const Node* current = _root;
		while (current)
		{
			if (_comp(key, current->key))
			{
				current = current->left;
			}
			else if (_comp(current->key, key))
			{
				current = current->right;
			}
			else
			{
				return current;
			}
		}
		return nullptr;
	}

	Node* Minimum(Node* node) const
	{
		assert(node);
		while (node->left)
			node = node->left;
		return node;
	}

	void RotateLeft(Node* x)
	{
		Node* y = x->right;
		assert(y);
		x->right = y->left;
		if (y->left)
			y->left->parent = x;
		y->parent = x->parent;
		if (!x->parent)
			_root = y;
		else if (x == x->parent->left)
			x->parent->left = y;
		else
			x->parent->right = y;
		y->left = x;
		x->parent = y;
	}

	void RotateRight(Node* y)
	{
		Node* x = y->left;
		assert(x);
		y->left = x->right;
		if (x->right)
			x->right->parent = y;
		x->parent = y->parent;
		if (!y->parent)
			_root = x;
		else if (y == y->parent->right)
			y->parent->right = x;
		else
			y->parent->left = x;
		x->right = y;
		y->parent = x;
	}

	void InsertFixup(Node* z)
	{
		while (IsRed(z->parent))
		{
			if (z->parent == z->parent->parent->left)
			{
				Node* y = z->parent->parent->right;
				if (IsRed(y))
				{
					z->parent->color = Color::Black;
					y->color = Color::Black;
					z->parent->parent->color = Color::Red;
					z = z->parent->parent;
				}
				else
				{
					if (z == z->parent->right)
					{
						z = z->parent;
						RotateLeft(z);
					}
					z->parent->color = Color::Black;
					z->parent->parent->color = Color::Red;
					RotateRight(z->parent->parent);
				}
			}
			else
			{
				Node* y = z->parent->parent->left;
				if (IsRed(y))
				{
					z->parent->color = Color::Black;
					y->color = Color::Black;
					z->parent->parent->color = Color::Red;
					z = z->parent->parent;
				}
				else
				{
					if (z == z->parent->left)
					{
						z = z->parent;
						RotateRight(z);
					}
					z->parent->color = Color::Black;
					z->parent->parent->color = Color::Red;
					RotateLeft(z->parent->parent);
				}
			}
		}
		_root->color = Color::Black;
	}

	void Transplant(Node* u, Node* v)
	{
		if (!u->parent)
			_root = v;
		else if (u == u->parent->left)
			u->parent->left = v;
		else
			u->parent->right = v;
		if (v)
			v->parent = u->parent;
	}

	void DeleteFixup(Node* x, Node* xParent)
	{
		while ((x != _root) && IsBlack(x))
		{
			if (!xParent)
				break;

			if (x == xParent->left)
			{
				Node* w = xParent->right;
				if (!w)
				{
					x = xParent;
					xParent = xParent->parent;
					continue;
				}

				if (IsRed(w))
				{
					w->color = Color::Black;
					xParent->color = Color::Red;
					RotateLeft(xParent);
					w = xParent->right;
				}

				if (IsBlack(w->left) && IsBlack(w->right))
				{
					w->color = Color::Red;
					x = xParent;
					xParent = xParent->parent;
				}
				else
				{
					if (IsBlack(w->right))
					{
						if (w->left)
							w->left->color = Color::Black;
						w->color = Color::Red;
						RotateRight(w);
						w = xParent->right;
					}
					w->color = xParent->color;
					xParent->color = Color::Black;
					if (w->right)
						w->right->color = Color::Black;
					RotateLeft(xParent);
					x = _root;
				}
			}
			else
			{
				Node* w = xParent->left;
				if (!w)
				{
					x = xParent;
					xParent = xParent->parent;
					continue;
				}

				if (IsRed(w))
				{
					w->color = Color::Black;
					xParent->color = Color::Red;
					RotateRight(xParent);
					w = xParent->left;
				}

				if (IsBlack(w->left) && IsBlack(w->right))
				{
					w->color = Color::Red;
					x = xParent;
					xParent = xParent->parent;
				}
				else
				{
					if (IsBlack(w->left))
					{
						if (w->right)
							w->right->color = Color::Black;
						w->color = Color::Red;
						RotateLeft(w);
						w = xParent->left;
					}
					w->color = xParent->color;
					xParent->color = Color::Black;
					if (w->left)
						w->left->color = Color::Black;
					RotateRight(xParent);
					x = _root;
				}
			}
		}
		if (x)
			x->color = Color::Black;
	}
};

template<typename KEY, typename Compare>
class RBTree<KEY, void, Compare>
{
private:
	enum class Color : unsigned char
	{
		Red,
		Black
	};

	struct Node
	{
		KEY key;
		Color color;
		Node* parent;
		Node* left;
		Node* right;

		Node(const KEY& k) :
			key(k), color(Color::Red), parent(nullptr), left(nullptr), right(nullptr) {}

		Node(KEY&& k) :
			key(std::move(k)), color(Color::Red), parent(nullptr), left(nullptr), right(nullptr) {}
	};

	Node* _root;
	std::size_t _size;
	Compare _comp;

public:
	RBTree() :
		_root(nullptr),
		_size(0),
		_comp(Compare()) {}

	explicit RBTree(Compare comp) :
		_root(nullptr),
		_size(0),
		_comp(std::move(comp)) {}

	~RBTree()
	{
		Clear();
	}

	RBTree(const RBTree&) = delete;
	RBTree& operator= (const RBTree&) = delete;

	RBTree(RBTree&& other) noexcept :
		_root(other._root),
		_size(other._size),
		_comp(std::move(other._comp))
	{
		other._root = nullptr;
		other._size = 0;
	}

	RBTree& operator= (RBTree&& other) noexcept
	{
		if (this != &other)
		{
			Clear();
			_root = other._root;
			_size = other._size;
			_comp = std::move(other._comp);
			other._root = nullptr;
			other._size = 0;
		}
		return *this;
	}

	std::size_t Size() const { return _size; }
	bool Empty() const { return _size == 0; }

	void Clear()
	{
		DestroySubtree(_root);
		_root = nullptr;
		_size = 0;
	}

	bool Contains(const KEY& key) const
	{
		return FindNode(key) != nullptr;
	}

	bool Insert(const KEY& key)
	{
		return EmplaceKey(key);
	}

	bool Insert(KEY&& key)
	{
		return EmplaceKey(std::move(key));
	}

	bool Erase(const KEY& key)
	{
		Node* z = FindNode(key);
		if (!z)
		{
			return false;
		}

		Node* y = z;
		Color yOriginalColor = y->color;
		Node* x = nullptr;
		Node* xParent = nullptr;

		if (!z->left)
		{
			x = z->right;
			xParent = z->parent;
			Transplant(z, z->right);
		}
		else if (!z->right)
		{
			x = z->left;
			xParent = z->parent;
			Transplant(z, z->left);
		}
		else
		{
			y = Minimum(z->right);
			yOriginalColor = y->color;
			x = y->right;
			if (y->parent == z)
			{
				xParent = y;
				if (x)
					x->parent = y;
			}
			else
			{
				xParent = y->parent;
				Transplant(y, y->right);
				y->right = z->right;
				y->right->parent = y;
			}

			Transplant(z, y);
			y->left = z->left;
			y->left->parent = y;
			y->color = z->color;
		}

		delete z;
		_size--;

		if (yOriginalColor == Color::Black)
		{
			DeleteFixup(x, xParent);
		}
		return true;
	}

private:
	static bool IsRed(const Node* node) { return node && node->color == Color::Red; }
	static bool IsBlack(const Node* node) { return !node || node->color == Color::Black; }

	void DestroySubtree(Node* node)
	{
		if (!node)
			return;
		DestroySubtree(node->left);
		DestroySubtree(node->right);
		delete node;
	}

	Node* FindNode(const KEY& key) const
	{
		Node* current = _root;
		while (current)
		{
			if (_comp(key, current->key))
			{
				current = current->left;
			}
			else if (_comp(current->key, key))
			{
				current = current->right;
			}
			else
			{
				return current;
			}
		}
		return nullptr;
	}

	Node* Minimum(Node* node) const
	{
		assert(node);
		while (node->left)
			node = node->left;
		return node;
	}

	void RotateLeft(Node* x)
	{
		Node* y = x->right;
		assert(y);
		x->right = y->left;
		if (y->left)
			y->left->parent = x;
		y->parent = x->parent;
		if (!x->parent)
			_root = y;
		else if (x == x->parent->left)
			x->parent->left = y;
		else
			x->parent->right = y;
		y->left = x;
		x->parent = y;
	}

	void RotateRight(Node* y)
	{
		Node* x = y->left;
		assert(x);
		y->left = x->right;
		if (x->right)
			x->right->parent = y;
		x->parent = y->parent;
		if (!y->parent)
			_root = x;
		else if (y == y->parent->right)
			y->parent->right = x;
		else
			y->parent->left = x;
		x->right = y;
		y->parent = x;
	}

	template<typename K>
	bool EmplaceKey(K&& key)
	{
		static_assert(std::is_constructible_v<KEY, K&&>, "RBTree<void>: Invalid key type");

		Node* parent = nullptr;
		Node* current = _root;
		while (current)
		{
			parent = current;
			if (_comp(key, current->key))
			{
				current = current->left;
			}
			else if (_comp(current->key, key))
			{
				current = current->right;
			}
			else
			{
				return false;
			}
		}

		Node* node = new Node(std::forward<K>(key));
		node->parent = parent;
		if (!parent)
		{
			_root = node;
		}
		else if (_comp(node->key, parent->key))
		{
			parent->left = node;
		}
		else
		{
			parent->right = node;
		}

		InsertFixup(node);
		_size++;
		return true;
	}

	void InsertFixup(Node* z)
	{
		while (IsRed(z->parent))
		{
			if (z->parent == z->parent->parent->left)
			{
				Node* y = z->parent->parent->right;
				if (IsRed(y))
				{
					z->parent->color = Color::Black;
					y->color = Color::Black;
					z->parent->parent->color = Color::Red;
					z = z->parent->parent;
				}
				else
				{
					if (z == z->parent->right)
					{
						z = z->parent;
						RotateLeft(z);
					}
					z->parent->color = Color::Black;
					z->parent->parent->color = Color::Red;
					RotateRight(z->parent->parent);
				}
			}
			else
			{
				Node* y = z->parent->parent->left;
				if (IsRed(y))
				{
					z->parent->color = Color::Black;
					y->color = Color::Black;
					z->parent->parent->color = Color::Red;
					z = z->parent->parent;
				}
				else
				{
					if (z == z->parent->left)
					{
						z = z->parent;
						RotateRight(z);
					}
					z->parent->color = Color::Black;
					z->parent->parent->color = Color::Red;
					RotateLeft(z->parent->parent);
				}
			}
		}
		_root->color = Color::Black;
	}

	void Transplant(Node* u, Node* v)
	{
		if (!u->parent)
			_root = v;
		else if (u == u->parent->left)
			u->parent->left = v;
		else
			u->parent->right = v;
		if (v)
			v->parent = u->parent;
	}

	void DeleteFixup(Node* x, Node* xParent)
	{
		while ((x != _root) && IsBlack(x))
		{
			if (!xParent)
				break;

			if (x == xParent->left)
			{
				Node* w = xParent->right;
				if (!w)
				{
					x = xParent;
					xParent = xParent->parent;
					continue;
				}

				if (IsRed(w))
				{
					w->color = Color::Black;
					xParent->color = Color::Red;
					RotateLeft(xParent);
					w = xParent->right;
				}

				if (IsBlack(w->left) && IsBlack(w->right))
				{
					w->color = Color::Red;
					x = xParent;
					xParent = xParent->parent;
				}
				else
				{
					if (IsBlack(w->right))
					{
						if (w->left)
							w->left->color = Color::Black;
						w->color = Color::Red;
						RotateRight(w);
						w = xParent->right;
					}
					w->color = xParent->color;
					xParent->color = Color::Black;
					if (w->right)
						w->right->color = Color::Black;
					RotateLeft(xParent);
					x = _root;
				}
			}
			else
			{
				Node* w = xParent->left;
				if (!w)
				{
					x = xParent;
					xParent = xParent->parent;
					continue;
				}

				if (IsRed(w))
				{
					w->color = Color::Black;
					xParent->color = Color::Red;
					RotateRight(xParent);
					w = xParent->left;
				}

				if (IsBlack(w->left) && IsBlack(w->right))
				{
					w->color = Color::Red;
					x = xParent;
					xParent = xParent->parent;
				}
				else
				{
					if (IsBlack(w->left))
					{
						if (w->right)
							w->right->color = Color::Black;
						w->color = Color::Red;
						RotateLeft(w);
						w = xParent->left;
					}
					w->color = xParent->color;
					xParent->color = Color::Black;
					if (w->left)
						w->left->color = Color::Black;
					RotateRight(xParent);
					x = _root;
				}
			}
		}
		if (x)
			x->color = Color::Black;
	}
};
