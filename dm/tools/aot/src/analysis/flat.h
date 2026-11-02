// Sorted vectors with the part of std::set's and std::map's interface discovery's functions use (src/analysis/discover.h): a function's blocks,
// its sets of roots and its maps of tables, pads and brs. At Monster Hunter Rise's size (about 600,000 functions, 3.6 million blocks) a std::map
// node and its allocation cost 112 bytes per 56-byte block, and five empty std::sets and three empty std::maps 384 bytes per function (README.md,
// "Memory at scale"); these hold the elements alone, and a sparse one (the sets and small maps most functions leave empty) holds them through
// one pointer, null while empty. The order is the key's, as the standard containers', so every iteration sees what it saw before. Unlike those
// containers, an insertion moves the elements after it (and a growth all of them): no reference or iterator into one survives an insertion,
// which discovery's users never keep across one (a walk builds a function's blocks in address order, each by its address once).
#pragma once

#include <algorithm>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace nxaot::analysis {

// The elements' vector, held in place (dense) or through a pointer that is null while there is none (sparse)
template <class E, bool Sparse>
class FlatStore {
public:
    std::vector<E>& Get() { return v; }
    const std::vector<E>& View() const { return v; }
    std::vector<E>* Mutable() { return &v; }  // for iterating: never allocates
    size_t Bytes() const { return v.capacity() * sizeof(E); }  // the elements' allocation (memory.h adds malloc's overhead)
    bool Held() const { return false; }       // a vector of its own besides (sparse)
    void Release() { std::vector<E>().swap(v); }

private:
    std::vector<E> v;
};

template <class E>
class FlatStore<E, true> {
public:
    FlatStore() = default;
    FlatStore(const FlatStore& o) : p(o.p ? std::make_unique<std::vector<E>>(*o.p) : nullptr) {}
    FlatStore(FlatStore&&) noexcept = default;
    FlatStore& operator=(const FlatStore& o) {
        p = o.p ? std::make_unique<std::vector<E>>(*o.p) : nullptr;
        return *this;
    }
    FlatStore& operator=(FlatStore&&) noexcept = default;
    std::vector<E>& Get() {
        if (!p) p = std::make_unique<std::vector<E>>();
        return *p;
    }
    const std::vector<E>& View() const { return p ? *p : Empty(); }
    std::vector<E>* Mutable() { return p ? p.get() : &Empty(); }  // the shared empty vector is only iterated (begin == end), never changed
    size_t Bytes() const { return p ? p->capacity() * sizeof(E) : 0; }
    bool Held() const { return p != nullptr; }
    void Release() { p.reset(); }

private:
    static std::vector<E>& Empty() {
        static std::vector<E> e;
        return e;
    }
    std::unique_ptr<std::vector<E>> p;
};

template <class T, bool Sparse = true>
class FlatSet {
public:
    using value_type = T;
    using const_iterator = typename std::vector<T>::const_iterator;
    using iterator = const_iterator;

    const_iterator begin() const { return s.View().begin(); }
    const_iterator end() const { return s.View().end(); }
    size_t size() const { return s.View().size(); }
    bool empty() const { return s.View().empty(); }
    void clear() { s.Release(); }
    size_t count(const T& x) const { return std::binary_search(begin(), end(), x) ? 1 : 0; }
    std::pair<const_iterator, bool> insert(const T& x) {
        std::vector<T>& v = s.Get();
        if (v.empty() || v.back() < x) {  // in order: appended
            v.push_back(x);
            return {v.end() - 1, true};
        }
        const auto it = std::lower_bound(v.begin(), v.end(), x);
        if (it != v.end() && !(x < *it)) return {it, false};
        const auto at = v.insert(it, x);
        return {at, true};
    }
    const_iterator erase(const_iterator it) { return s.Get().erase(it); }
    const FlatStore<T, Sparse>& Store() const { return s; }

private:
    FlatStore<T, Sparse> s;
};

template <class K, class V, bool Sparse = false>
class FlatMap {
public:
    using value_type = std::pair<K, V>;
    using iterator = typename std::vector<value_type>::iterator;
    using const_iterator = typename std::vector<value_type>::const_iterator;
    using reverse_iterator = typename std::vector<value_type>::reverse_iterator;
    using const_reverse_iterator = typename std::vector<value_type>::const_reverse_iterator;

    iterator begin() { return s.Mutable()->begin(); }
    iterator end() { return s.Mutable()->end(); }
    const_iterator begin() const { return s.View().begin(); }
    const_iterator end() const { return s.View().end(); }
    reverse_iterator rbegin() { return s.Mutable()->rbegin(); }
    reverse_iterator rend() { return s.Mutable()->rend(); }
    const_reverse_iterator rbegin() const { return s.View().rbegin(); }
    const_reverse_iterator rend() const { return s.View().rend(); }
    size_t size() const { return s.View().size(); }
    bool empty() const { return s.View().empty(); }
    void clear() {
        if constexpr (Sparse) s.Release();
        else s.Get().clear();  // a dense map keeps its room: a walk builds its blocks again
    }
    void reserve(size_t n) { s.Get().reserve(n); }
    void shrink_to_fit() {
        if (!s.View().empty() || !Sparse) s.Get().shrink_to_fit();
    }

    const_iterator lower_bound(const K& k) const { return std::lower_bound(begin(), end(), k, Less()); }
    iterator lower_bound(const K& k) { return std::lower_bound(begin(), end(), k, Less()); }
    const_iterator upper_bound(const K& k) const { return std::upper_bound(begin(), end(), k, Greater()); }
    iterator upper_bound(const K& k) { return std::upper_bound(begin(), end(), k, Greater()); }
    const_iterator find(const K& k) const {
        const auto it = lower_bound(k);
        return it != end() && !(k < it->first) ? it : end();
    }
    iterator find(const K& k) {
        const auto it = lower_bound(k);
        return it != end() && !(k < it->first) ? it : end();
    }
    size_t count(const K& k) const { return find(k) != end() ? 1 : 0; }
    const V& at(const K& k) const {
        const auto it = find(k);
        if (it == end()) throw std::out_of_range("FlatMap::at");
        return it->second;
    }
    V& operator[](const K& k) { return emplace(k, V{}).first->second; }
    size_t erase(const K& k) {
        const auto it = find(k);
        if (it == end()) return 0;
        s.Get().erase(it);
        return 1;
    }
    template <class... A>
    std::pair<iterator, bool> emplace(const K& k, A&&... a) {
        std::vector<value_type>& v = s.Get();
        if (v.empty() || v.back().first < k) {  // in order: appended
            v.emplace_back(std::piecewise_construct, std::forward_as_tuple(k), std::forward_as_tuple(std::forward<A>(a)...));
            return {v.end() - 1, true};
        }
        const auto it = std::lower_bound(v.begin(), v.end(), k, Less());
        if (it != v.end() && !(k < it->first)) return {it, false};
        const auto at = v.insert(it, value_type(std::piecewise_construct, std::forward_as_tuple(k), std::forward_as_tuple(std::forward<A>(a)...)));
        return {at, true};
    }
    const FlatStore<value_type, Sparse>& Store() const { return s; }

private:
    struct Less {
        bool operator()(const value_type& e, const K& k) const { return e.first < k; }
    };
    struct Greater {
        bool operator()(const K& k, const value_type& e) const { return k < e.first; }
    };
    FlatStore<value_type, Sparse> s;
};

}  // namespace nxaot::analysis
