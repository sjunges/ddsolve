#ifndef UTILS_HPP
#define UTILS_HPP

/**
 * @file utils.hpp
 * @brief Utility types, hash helpers, and range algorithms.
 *
 * This header provides:
 *   - Indirection functors (indirection, indirection_less,
 * indirection_equal_to) for pointer-like types used in ordered containers.
 *   - Hash concepts (MemberHashable, StdHashable, Hashable) and corresponding
 *     hash functors (IndirectHash, MemberHash, SpanHash).
 *   - MergeIterator and MergeView – a sorted merge of two forward ranges,
 *     analogous to std::ranges::merge but exposing an iterator type.
 *
 * @note The Doxygen-style comments in this file were generated with the help
 *       of AI (Qwen3.6-35B-A3B). They have been human-checked on a best-effort
 *       basis. No lines of logic in this file were written or modified by AI.
 */

#include <xxhash.h>

#include <concepts>
#include <cstddef>
#include <functional>
#include <iterator>
#include <ranges>
#include <span>
#include <utility>
#include <version>

namespace ddsolve {

// Apple libc++ lacks std::ranges::const_iterator_t.
#ifdef __cpp_lib_ranges_as_const
template <class R>
using const_iterator_t = std::ranges::const_iterator_t<const R&>;
template <class R>
using const_sentinel_t = std::ranges::const_sentinel_t<const R&>;
#else
template <class R>
using const_iterator_t = std::ranges::iterator_t<const R&>;
template <class R>
using const_sentinel_t = std::ranges::sentinel_t<const R&>;
#endif

/**
 * @brief Functor that dereferences pointer-like types.
 *
 * Given a pointer-like object, returns the value it points to.
 * Useful for using smart pointers or raw pointers as keys in
 * ordered containers that expect value comparison rather than
 * pointer-address comparison.
 */
struct indirection {
  /**
   * @brief Dereferences the pointer-like type and returns the referenced value.
   */
  template <typename T>
  constexpr decltype(auto) operator()(T&& t) const {
    return *std::forward<T>(t);
  }
};

/**
 * @brief Comparator for pointer-like types that compares dereferenced values.
 *
 * Compares the values pointed to by two pointer-like objects using
 * the less-than operator, rather than comparing the pointers themselves.
 */
struct indirection_less {
  /**
   * @brief Compares dereferenced values using less-than.
   */
  template <typename T>
  constexpr bool operator()(const T& lhs, const T& rhs) const {
    return *lhs < *rhs;
  }
};

/**
 * @brief Equality comparator for pointer-like types.
 *
 * Compares the values pointed to by two pointer-like objects for
 * equality, rather than comparing the pointers themselves.
 */
struct indirection_equal_to {
  /**
   * @brief Compares dereferenced values for equality.
   */
  template <typename T>
  constexpr bool operator()(const T& lhs, const T& rhs) const {
    return *lhs == *rhs;
  }
};

/**
 * @brief Concept for types that have a member `hash()` method.
 *
 * A type `T` satisfies `MemberHashable` if `a.hash()` is well-formed
 * and returns a value implicitly convertible to `size_t`.
 */
template <typename T>
concept MemberHashable = requires(T a) {
  { a.hash() } -> std::convertible_to<size_t>;
};

/**
 * @brief Concept for types that have a `std::hash` specialization.
 *
 * A type `T` satisfies `StdHashable` if `std::hash<T>{}(a)` is
 * well-formed and returns a value implicitly convertible to `size_t`.
 */
template <typename T>
concept StdHashable = requires(T a) {
  { std::hash<T>{}(a) } -> std::convertible_to<size_t>;
};

/**
 * @brief Concept for hashable types.
 *
 * A type `T` satisfies `Hashable` if it satisfies either
 * `MemberHashable<T>` or `StdHashable<T>`.
 */
template <typename T>
concept Hashable = MemberHashable<T> || StdHashable<T>;

/**
 * @brief Hash functor for pointer-like types that dereference to a `Hashable`
 * value.
 *
 * Given a pointer `Ptr` whose dereferenced type is `const T&`, this functor
 * computes a hash by calling either `(*ptr).hash()` (if `T` is
 * `MemberHashable`) or `std::hash<T>{}(*ptr)` (if `T` is only
 * `StdHashable`).
 *
 * @tparam T  The hashable type that the pointer dereferences to.
 */
template <Hashable T>
struct IndirectHash {
  /**
   * @brief Hashes the dereferenced value.
   */
  template <typename Ptr>
    requires MemberHashable<T> && requires(const Ptr& p) {
      { *p } -> std::convertible_to<const T&>;
    }
  [[nodiscard]] constexpr size_t operator()(const Ptr& ptr) const {
    return (*ptr).hash();
  }

  /**
   * @brief Hashes the dereferenced value with a seed.
   */
  template <typename Ptr>
    requires(!MemberHashable<T>) && StdHashable<T> && requires(const Ptr& p) {
      { *p } -> std::convertible_to<const T&>;
    }
  [[nodiscard]] constexpr size_t operator()(const Ptr& ptr) const {
    return std::hash<T>{}(*ptr);
  }
};

/**
 * @brief Hash functor for types with a member `hash()` method.
 *
 * Invokes `a.hash()` on the given argument and returns the resulting
 * `size_t` value.
 */
struct MemberHash {
  /**
   * @brief Computes hash via the member hash() method.
   */
  template <MemberHashable T>
  [[nodiscard]] constexpr size_t operator()(const T& a) const {
    return a.hash();
  }
};

/**
 * @brief Hash functor for `std::span` types using xxHash.
 *
 * Computes a hash by interpreting the span's underlying data as bytes
 * and passing them to XXH3. Provides both a seedless variant and a
 * seeded variant.
 */
struct SpanHash {
  /**
   * @brief Hash a span without a seed.
   * @param a  The span to hash.
   * @return The xxHash3-64 hash of the span's underlying bytes.
   */
  template <typename T>
  [[nodiscard]] size_t operator()(const std::span<T> a) const {
    const auto bytes{std::as_bytes(a)};

    return XXH3_64bits(bytes.data(), bytes.size());
  }

  /**
   * @brief Hash a span with a seed.
   * @param a    The span to hash.
   * @param seed The xxHash3 seed value.
   * @return The xxHash3-64 hash of the span's underlying bytes with the given
   * seed.
   */
  template <typename T>
  [[nodiscard]] size_t operator()(const std::span<T> a,
                                  const uint64_t seed) const {
    const auto bytes{std::as_bytes(a)};

    return XXH3_64bits_withSeed(bytes.data(), bytes.size(), seed);
  }
};

}  // namespace ddsolve

namespace ddsolve::views {

/**
 * @brief Iterator that merges two forward ranges in sorted order.
 *
 * Walks through two forward ranges simultaneously, yielding elements in
 * the order dictated by a comparison function. Both ranges must be
 * pre-sorted according to the same comparator, and their value types
 * must be the same. The iterator maintains two cursors (one per range)
 * and advances the cursor pointing to the smaller element.
 *
 * @tparam It1  Iterator type for the first range.
 * @tparam S1   Sentinel type for the first range.
 * @tparam It2  Iterator type for the second range.
 * @tparam S2   Sentinel type for the second range.
 * @tparam Comp Comparison function type (defaults to std::ranges::less).
 */
template <std::forward_iterator It1, std::sentinel_for<It1> S1,
          std::forward_iterator It2, std::sentinel_for<It2> S2,
          class Comp = std::ranges::less>
  requires std::same_as<std::iter_value_t<It1>, std::iter_value_t<It2>> &&
           std::same_as<std::iter_reference_t<It1>,
                        std::iter_reference_t<It2>> &&
           std::indirect_strict_weak_order<Comp, It1, It2>
class MergeIterator {
 public:
  /**
   * @brief The difference type for this iterator.
   */
  using difference_type = std::ptrdiff_t;
  /**
   * @brief The value type of the elements produced by this iterator.
   */
  using value_type = std::iter_value_t<It1>;

  /**
   * @brief Construct a MergeIterator from two ranges and a comparator.
   * @param r1  The first sorted range.
   * @param r2  The second sorted range.
   * @param comp  The comparison function to determine element order.
   */
  template <std::ranges::forward_range R1, std::ranges::forward_range R2>
  constexpr MergeIterator(const R1& r1, const R2& r2, const Comp& comp = {})
      : a_begin_{std::ranges::cbegin(r1)},
        a_end_{r1.cend()},
        b_begin_{r2.cbegin()},
        b_end_{r2.cend()},
        comp_{comp} {};

  /**
   * @brief Default constructor – creates a default-constructed iterator.
   */
  constexpr MergeIterator() = default;
  /**
   * @brief Destructor – default.
   */
  constexpr ~MergeIterator() = default;

  /**
   * @brief Copy constructor – default.
   */
  constexpr MergeIterator(const MergeIterator&) = default;
  /**
   * @brief Move constructor – default.
   */
  constexpr MergeIterator(MergeIterator&&) = default;

  /**
   * @brief Copy assignment – default.
   */
  constexpr MergeIterator& operator=(const MergeIterator&) = default;
  /**
   * @brief Move assignment – default.
   */
  constexpr MergeIterator& operator=(MergeIterator&&) = default;

  /**
   * @brief Dereference the current position.
   * @return The smaller of the two current elements, or the element from
   *         the non-empty range if one side is exhausted.
   */
  constexpr std::iter_reference_t<It1> operator*() const {
    if (a_begin_ == a_end_) {
      return *b_begin_;
    }
    if (b_begin_ == b_end_) {
      return *a_begin_;
    }
    return std::invoke(comp_, *a_begin_, *b_begin_) ? *a_begin_ : *b_begin_;
  }

  /**
   * @brief Prefix increment – advances to the next merged element.
   * @return A reference to this iterator.
   */
  constexpr MergeIterator& operator++() {
    if (a_begin_ == a_end_) {
      ++b_begin_;
    } else if (b_begin_ == b_end_) {
      ++a_begin_;
    } else {
      if (std::invoke(comp_, *a_begin_, *b_begin_)) {
        ++a_begin_;
      } else {
        ++b_begin_;
      }
    }

    return *this;
  }

  /**
   * @brief Postfix increment – returns a copy before advancing.
   * @return A copy of the iterator prior to incrementing.
   */
  constexpr MergeIterator operator++(int) {
    auto tmp = *this;
    ++*this;
    return tmp;
  }

  /**
   * @brief Compute the distance between this iterator and another.
   * @param other  The other iterator to measure against.
   * @return The number of merged elements between @p other and this iterator.
   */
  constexpr difference_type operator-(const MergeIterator& other) const {
    return std::ranges::distance(other.a_begin_, a_begin_) +
           std::ranges::distance(other.b_begin_, b_begin_);
  }

  /**
   * @brief Equality comparison.
   * @param other  The other iterator to compare with.
   * @return true if both iterators point to the same positions in both ranges.
   */
  constexpr bool operator==(const MergeIterator& other) const {
    return std::forward_as_tuple(a_begin_, b_begin_) ==
           std::forward_as_tuple(other.a_begin_, other.b_begin_);
  }

  /**
   * @brief Construct an end iterator for this merged range.
   * @return A MergeIterator positioned past the last element of both ranges.
   */
  [[nodiscard]] constexpr MergeIterator end() const {
    MergeIterator rval{*this};
    rval.a_begin_ = a_end_;
    rval.b_begin_ = b_end_;
    return rval;
  }

 private:
  It1 a_begin_;
  S1 a_end_;
  It2 b_begin_;
  S2 b_end_;

  Comp comp_;
};

template <class R1, class R2, class Comp = std::ranges::less>
MergeIterator(const R1&, const R2&, Comp = {})
    -> MergeIterator<const_iterator_t<R1>,
                     const_sentinel_t<R1>,
                     const_iterator_t<R2>,
                     const_sentinel_t<R2>, Comp>;

static_assert(std::forward_iterator<
              decltype(MergeIterator(std::vector<int>{}, std::vector<int>{}))>);

/**
 * @brief A view that merges two forward ranges in sorted order.
 *
 * Wraps two pre-sorted ranges and exposes them as a single merged range
 * via a const_iterator of type MergeIterator. Behaves analogously to
 * std::ranges::merge but exposes an iterator type for direct traversal.
 *
 * @tparam R1  The type of the first forward range.
 * @tparam R2  The type of the second forward range.
 * @tparam Comp Comparison function type (defaults to std::ranges::less).
 */
template <std::ranges::forward_range R1, std::ranges::forward_range R2,
          class Comp = std::ranges::less>
class MergeView : public std::ranges::view_interface<MergeView<R1, R2, Comp>> {
 public:
  /**
   * @brief The const iterator type for traversing the merged range.
   */
  using const_iterator =
      MergeIterator<const_iterator_t<R1>,
                    const_sentinel_t<R1>,
                    const_iterator_t<R2>,
                    const_sentinel_t<R2>, Comp>;

  /**
   * @brief Construct a MergeView from two ranges and a comparator.
   * @param r1  The first sorted range.
   * @param r2  The second sorted range.
   * @param comp  The comparison function to determine element order.
   */
  constexpr MergeView(const R1& r1, const R2& r2, const Comp& comp = {})
      : begin_{MergeIterator(r1, r2, comp)} {}

  /**
   * @brief Get an iterator to the beginning of the merged range.
   * @return A const_iterator to the first element of the merged output.
   */
  [[nodiscard]]
  constexpr const_iterator begin() const {
    return begin_;
  }

  /**
   * @brief Get a sentinel iterator for the end of the merged range.
   * @return A const_iterator positioned past the last element.
   */
  [[nodiscard]]
  constexpr const_iterator end() const {
    return begin_.end();
  }

 private:
  const_iterator begin_;
};

static_assert(std::ranges::forward_range<
              decltype(MergeView(std::vector<int>{}, std::vector<int>{}))>);

}  // namespace ddsolve::views

#endif /* UTILS_HPP */
