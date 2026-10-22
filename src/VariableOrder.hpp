#ifndef VARIABLEORDER_HPP
#define VARIABLEORDER_HPP

/**
 * @file VariableOrder.hpp
 * @brief Variable ordering and substitution infrastructure for decision
 * diagrams.
 *
 * This header defines three core types used to manage the mapping between
 * logical variables and decision-diagram levels:
 *   - Substitution – represents a variable-to-term replacement.
 *   - VariableOrder – maps comparisons to Sylvan MTBDD variables and
 *     provides operations for obtaining representations, adding variables,
 *     and performing substitutions across the entire order.
 *   - OrderedMtbddPointer – a helper that ties an MTBDD to a VariableOrder
 *     so that garbage collection knows which orders to keep alive.
 *
 * @note The Doxygen-style comments in this file were generated with the help
 *       of AI (Qwen3.6-35B-A3B). They have been human-checked on a best-effort
 *       basis. No lines of logic in this file were written or modified by AI.
 */

#include <algorithm>
#include <atomic>
#include <concepts>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <sylvan_obj.hpp>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ArithmeticTypes.hpp"
#include "LinearTerm.hpp"
#include "utils.hpp"

namespace ddsolve {

/**
 * @brief Identifier type for substitutions.
 *
 * Used to uniquely identify each substitution instance.
 */
using SubstitutionId = int;

/**
 * @brief Type for Sylvan decision-diagram variables.
 *
 * Maps logical variables (comparisons) to levels in the Sylvan MTBDD.
 */
using SylvanVariable = uint32_t;

/**
 * @brief Represents a substitution of a variable with a term.
 *
 * A substitution maps a logical variable to a term expression,
 * used when performing variable substitution across decision diagrams.
 *
 * @tparam Term  The term type (e.g. LinearTerm or SmtTerm).
 */
template <typename Term>
class Substitution {
 public:
  /**
   * @brief Construct a substitution for a variable with a given term.
   * @param var  The variable to substitute.
   * @param term The term to substitute it with.
   */
  Substitution(const Term::Variable& var, Term::ConstTermPointer term)
      : var_{var}, term_{std::move(term)} {}

  /**
   * @brief Get the unique ID of this substitution.
   * @return The substitution's unique ID.
   */
  [[nodiscard]] uint64_t getId() const {
    return id_;
  }

  /**
   * @brief Get the variable that is being substituted.
   * @return The variable ID.
   */
  [[nodiscard]] Term::Variable getVariable() const {
    return var_;
  }

  /**
   * @brief Get the term that replaces the variable.
   * @return The term to substitute with.
   */
  [[nodiscard]] Term::ConstTermPointer getTerm() const {
    return term_;
  }

 private:
  inline static std::atomic_uint64_t next_id{};

  uint64_t id_{next_id.fetch_add(1, std::memory_order_relaxed)};
  Term::Variable var_;
  Term::ConstTermPointer term_;
};

/**
 * @brief Manages the mapping between logical variables and decision-diagram
 * levels.
 *
 * A variable order maps comparison expressions to Sylvan MTBDD variable
 * indices (levels), and provides operations for obtaining diagram
 * representations, adding variables, and performing substitutions across
 * the entire order.
 *
 * @tparam Term  The term type (e.g. LinearTerm or SmtTerm).
 */
template <typename Term>
class VariableOrder {
 public:
  /**
   * @brief Shared pointer to a mutable VariableOrder.
   */
  using VariableOrderPointer = std::shared_ptr<VariableOrder<Term>>;
  /**
   * @brief Shared pointer to a const VariableOrder.
   */
  using ConstVariableOrderPointer = std::shared_ptr<const VariableOrder<Term>>;

  /**
   * @brief Swap the contents of this variable order with another.
   * @param other  The variable order to swap with.
   */
  void swap(VariableOrder& other) noexcept {
    std::swap(id_, other.id_);
    std::swap(references_, other.references_);
    std::swap(to_variable_, other.to_variable_);
    std::swap(to_comparison_, other.to_comparison_);
  }

  VariableOrder(const VariableOrder& other) = delete;
  /**
   * @brief Default constructor – creates an empty variable order.
   */
  VariableOrder() = default;
  /**
   * @brief Move constructor.
   */
  VariableOrder(VariableOrder&& other) = default;
  ~VariableOrder() = default;

  VariableOrder& operator=(const VariableOrder& other) = delete;

  /**
   * @brief Move assignment operator.
   */
  VariableOrder& operator=(VariableOrder&& other) noexcept = default;

  /**
   * @brief Obtain an MTBDD representation of a comparison.
   * @param comp  The comparison to represent.
   * @return An MTBDD encoding the comparison under this variable order.
   *
   * For LinearTerm, handles all comparison types (equality, inequality,
   * less-than, etc.) by decomposing them into less-than comparisons.
   * For other term types, uses a simple variable-based encoding.
   */
  sylvan::Mtbdd obtainRepresentation(const Term::ConstComparisonPointer& comp) {
    if (const auto check{comp->trivialCheck()};
        check != expression::SmtResult::kUnknown) {
      return check == expression::SmtResult::kSat ? sylvan::Mtbdd::mtbddOne()
                                                  : sylvan::Mtbdd::mtbddZero();
    }

    // Can be expanded for other types in the future
    if constexpr (!std::is_same_v<Term, expression::LinearTerm>) {
      return sylvan::Mtbdd::mtbddVar(addVariable(comp));
    } else {
      const auto& term{comp->getTerm()};

      switch (comp->getType()) {
        [[likely]] case expression::ComparisonType::kLessThan:
          // recursion base case
          return sylvan::Mtbdd::mtbddVar(addVariable(comp));
        case expression::ComparisonType::kEquality:
          return !obtainRepresentation(
                     std::make_shared<const typename Term::Comparison>(
                         term.scale(-1),
                         expression::ComparisonType::kLessThan)) *
                 !obtainRepresentation(
                     std::make_shared<const typename Term::Comparison>(
                         term, expression::ComparisonType::kLessThan));
        case expression::ComparisonType::kInequality:
          return obtainRepresentation(
                     std::make_shared<const typename Term::Comparison>(
                         term.scale(-1),
                         expression::ComparisonType::kLessThan)) +
                 obtainRepresentation(
                     std::make_shared<const typename Term::Comparison>(
                         term, expression::ComparisonType::kLessThan));
        case expression::ComparisonType::kLessThanOrEqual:
          return !obtainRepresentation(
              std::make_shared<const typename Term::Comparison>(
                  term.scale(-1), expression::ComparisonType::kLessThan));
        case expression::ComparisonType::kGreaterThan:
          return obtainRepresentation(
              std::make_shared<const typename Term::Comparison>(
                  term.scale(-1), expression::ComparisonType::kLessThan));
        case expression::ComparisonType::kGreaterThanOrEqual:
          return !obtainRepresentation(
              std::make_shared<const typename Term::Comparison>(
                  term, expression::ComparisonType::kLessThan));
        [[unlikely]] default:
          return sylvan::Mtbdd::mtbddZero();
      }
    }
  }

  /**
   * @brief Add a comparison as a Sylvan variable in this order.
   * @param comp  The comparison to add.
   * @return The Sylvan variable index assigned to the comparison.
   *
   * For LinearTerm, only canonical less-than comparisons are accepted.
   * If the comparison is already registered, returns the existing index.
   */
  SylvanVariable addVariable(
      const typename Term::ConstComparisonPointer& comp) {
    if constexpr (std::is_same_v<Term, expression::LinearTerm>) {
      if (comp->getType() != expression::ComparisonType::kLessThan) {
        throw std::invalid_argument{
            "Tried to add non canonical comparison as sylvan variable."};
      }
    }
    const auto it{to_variable_.find(comp)};
    if (it != to_variable_.end()) {
      return it->second;
    }

    const auto new_var = to_variable_.size();
    to_variable_[comp] = new_var;
    to_comparison_[new_var] = comp;

    assert(to_variable_.size() == to_comparison_.size());
    return new_var;
  }

  /**
   * @brief Add multiple comparisons as Sylvan variables.
   * @param comps  A range of comparisons to add.
   *
   * Iterates over the range and adds each comparison via addVariable().
   */
  template <typename R>
    requires std::ranges::forward_range<R> && std::ranges::sized_range<R> &&
             std::convertible_to<std::ranges::range_value_t<R>,
                                 const typename Term::ConstComparisonPointer>
  void addVariables(const R& comps) {
    for (const auto& comp : comps) {
      addVariable(comp);
    }

    assert(to_variable_.size() == to_comparison_.size());
  }

  /**
   * @brief Get the comparison for a given Sylvan variable index.
   * @param var  The Sylvan variable index.
   * @return The comparison mapped to this index, or an empty pointer if not
   *         found.
   */
  [[nodiscard]] Term::ConstComparisonPointer getComparison(
      SylvanVariable var) const {
    const auto it{to_comparison_.find(var)};

    if (it != to_comparison_.end()) {
      return it->second;
    }

    return {};
  }

  /**
   * @brief Get the Sylvan variable index for a comparison.
   * @param comp  The comparison to look up.
   * @return The variable index, or empty optional if not found.
   */
  [[nodiscard]] std::optional<SylvanVariable> getVariable(
      const Term::ConstComparisonPointer& comp) const {
    const auto it{to_variable_.find(comp)};

    if (it != to_variable_.end()) {
      return it->second;
    }

    return {};
  }

  /**
   * @brief Get all Sylvan variable indices in this order.
   * @return A vector of all variable indices (in the order they were added).
   */
  [[nodiscard]] std::vector<SylvanVariable> getVariables() const {
    return std::views::elements<1>(to_variable_) |
           std::ranges::to<std::vector>();
  }

  /**
   * @brief Get all comparisons in this order.
   * @return A vector of all comparisons (in the order they were added).
   */
  [[nodiscard]] std::vector<typename Term::ConstComparisonPointer>
  getComparisons() const {
    return std::views::elements<0>(to_variable_) |
           std::ranges::to<std::vector>();
  }

  /**
   * @brief Substitute a variable with a term in a range of MTBDDs.
   * @param var   The variable to substitute.
   * @param term  The term to substitute it with.
   * @param old_vars  The old variable indices to substitute in.
   * @return A vector of new MTBDDs with the substitution applied.
   *
   * For each variable in @p old_vars, gets the comparison, applies the
   * substitution to the comparison, and obtains a new MTBDD representation.
   */
  template <typename R>
    requires std::ranges::forward_range<R> && std::ranges::sized_range<R> &&
             std::convertible_to<std::ranges::range_value_t<R>,
                                 const SylvanVariable>
  std::vector<sylvan::Mtbdd> substitute(const Term::Variable& var,
                                        const Term::Term& term,
                                        const R& old_vars) {
    return std::views::transform(
               old_vars,
               [this, &var, &term](const auto& i) {
                 return obtainRepresentation(
                     std::make_shared<const typename Term::Comparison>(
                         getComparison(i)->substitute(var, term)));
               }) |
           std::ranges::to<std::vector>();
  }

  /**
   * @brief Register an MTBDD with this variable order for garbage collection.
   * @param mtbdd  A weak pointer to the MTBDD to register.
   *
   * If the internal references vector is at capacity, garbage collection
   * is performed first. The MTBDD is stored as a weak reference so that
   * the variable order keeps the MTBDD alive as long as this order exists.
   */
  void registerMtbdd(const std::weak_ptr<sylvan::Mtbdd>& mtbdd) {
    if (references_.size() == references_.capacity()) {
      // realloc would take place...
      collectGarbidge();
    }
    references_.push_back(mtbdd);
  }

  /**
   * @brief Remove expired MTBDD references and deduplicate.
   *
   * Erases all expired weak references, then sorts and removes duplicates
   * based on ownership to ensure each registered MTBDD is tracked exactly
   * once.
   */
  void collectGarbidge() {
    std::erase_if(references_, [](auto&& ref) { return ref.expired(); });

    std::ranges::sort(references_, std::owner_less{});
    const auto unique_deleter{
        std::ranges::unique(references_, [](const auto& lhs, const auto& rhs) {
          return !lhs.owner_before(rhs) && !rhs.owner_before(lhs);
        })};
    references_.erase(unique_deleter.begin(), unique_deleter.end());
  }

  /**
   * @brief Get the unique ID of this variable order.
   * @return The variable order's unique ID.
   */
  [[nodiscard]] uint64_t getId() const {
    return id_;
  }

 private:
  inline static std::atomic_uint64_t next_id{};

  uint64_t id_{next_id.fetch_add(1, std::memory_order_relaxed)};

  std::vector<std::weak_ptr<sylvan::Mtbdd>> references_;

  std::unordered_map<typename Term::ConstComparisonPointer, SylvanVariable,
                     IndirectHash<typename Term::Comparison>,
                     indirection_equal_to>
      to_variable_;
  std::unordered_map<SylvanVariable, typename Term::ConstComparisonPointer>
      to_comparison_;
};

/**
 * @brief A helper that ties an MTBDD to a VariableOrder for garbage collection.
 *
 * Stores a shared pointer to an MTBDD and registers it with the given
 * VariableOrder so that the order keeps the MTBDD alive for as long as
 * the order itself exists.
 *
 * @tparam Term  The term type (e.g. LinearTerm or SmtTerm).
 */
template <typename Term>
class OrderedMtbddPointer {
 public:
  /**
   * @brief Construct an OrderedMtbddPointer from a variable order and MTBDD.
   * @param order  The variable order to register with.
   * @param mtbdd  The MTBDD to wrap.
   *
   * Registers the MTBDD with the variable order for garbage collection.
   */
  OrderedMtbddPointer(const std::shared_ptr<VariableOrder<Term>>& order,
                      const sylvan::Mtbdd& mtbdd)
      : ptr_{std::make_shared<sylvan::Mtbdd>(mtbdd)} {
    order->registerMtbdd(ptr_);
  }

  /**
   * @brief Construct an OrderedMtbddPointer with a given variable order.
   * @param order  The variable order to register with.
   *
   * Creates an empty (zero) MTBDD wrapped in this pointer.
   */
  explicit OrderedMtbddPointer(
      const std::shared_ptr<VariableOrder<Term>>& order)
      : OrderedMtbddPointer{order, {}} {}

  /**
   * @brief Dereference to get the underlying MTBDD.
   * @return A reference to the underlying Sylvan MTBDD.
   */
  sylvan::Mtbdd& operator*() const noexcept {
    return *ptr_;
  }

  /**
   * @brief Arrow operator to access the underlying MTBDD.
   * @return A pointer to the underlying Sylvan MTBDD.
   */
  sylvan::Mtbdd* operator->() const noexcept {
    return ptr_.get();
  }

  /**
   * @brief Conversion to bool – always returns true.
   * @return true, indicating the pointer is valid.
   */
  constexpr explicit operator bool() const noexcept {
    return true;
  }

 private:
  std::shared_ptr<sylvan::Mtbdd> ptr_;
};

}  // namespace ddsolve

#endif /* VARIABLEORDER_HPP */
