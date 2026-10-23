#ifndef XDD_HPP
#define XDD_HPP

/**
 * @file XDD.hpp
 * @brief Extended Decision Diagram base class.
 *
 * XDD is a templated base class that wraps a Sylvan MTBDD (Multi-Terminal
 * Binary Decision Diagram) together with a variable order.  It provides a
 * uniform interface for navigating, inspecting, and transforming decision
 * diagrams regardless of the leaf type (boolean for XBDD, numeric for XADD).
 *
 * The class is designed to be inherited by concrete diagram types:
 *   - XBDD – extended binary decision diagram (boolean leaf values)
 *   - XADD – extended arithmetic decision diagram (numeric leaf values)
 *
 * @tparam T  Concrete derived diagram type (used for CRTP).
 * @tparam Term Term type that describes the variable/leaf semantics.
 *
 * @note The Doxygen-style comments in this file were generated with the help
 *       of AI (Qwen3.6-35B-A3B). They have been human-checked on a best-effort
 *       basis. No lines of logic in this file were written or modified by AI.
 */

#include <gmpxx.h>

#include <cstdio>
#include <memory>
#include <stdexcept>
#include <sylvan_obj.hpp>
#include <unordered_map>

#include "LinearTerm.hpp"
#include "VariableOrder.hpp"
#include "gsl.hpp"

namespace ddsolve {

template <typename Term>
class XBDD;

/**
 * @brief Extended Decision Diagram – a parameterised wrapper around a Sylvan
 *        MTBDD with an associated variable order.
 *
 * Every XDD carries a @p VariableOrder that maps decision-diagram levels to
 * logical variables (comparisons).  The underlying Sylvan MTBDD stores the
 * actual diagram structure, while the derived type @p T determines the
 * meaning of the leaf values.
 *
 * The constructor that takes an MTBDD is protected to enforce construction
 * through derived types, preserving the invariant that the variable order
 * matches the diagram.
 */
template <typename T, typename Term>
class XDD {
 protected:
  /**
   * @brief Construct an XDD from a raw Sylvan MTBDD and a variable order.
   * @param mtbdd  The underlying Sylvan MTBDD.
   * @param order  The variable order for this diagram.
   *
   * @note This constructor is protected because the caller must ensure that
   *       @p order is consistent with the levels in @p mtbdd.
   */
  XDD(const sylvan::Mtbdd& mtbdd,
      VariableOrder<Term>::VariableOrderPointer order)
      : order_{std::move(order)}, mtbdd_{order_, mtbdd} {}

  ~XDD() = default;

 public:
  /**
   * @brief Swap the contents of two XDDs.
   * @param other  The XDD to swap with.
   */
  void swap(XDD& other) noexcept {
    std::swap(mtbdd_, other.mtbdd_);
    std::swap(order_, other.order_);
  };

  /**
   * @brief Construct an XDD with a given variable order (empty diagram).
   * @param order  The variable order for this diagram.
   */
  explicit XDD(VariableOrder<Term>::VariableOrderPointer order)
      : XDD({}, order) {}

  /**
   * @brief Default constructor.
   */
  XDD() = default;
  /**
   * @brief Default copy constructor.
   */
  XDD(const XDD& other) = default;
  /**
   * @brief Default move constructor.
   */
  XDD(XDD&& other) = default;
  /**
   * @brief Default copy assignment operator.
   */
  XDD& operator=(const XDD& other) = default;
  /**
   * @brief Default move assignment operator.
   */
  XDD& operator=(XDD&& other) noexcept = default;

  /**
   * @brief Check equality of two XDDs.
   * @param other  The other XDD to compare with.
   * @return true if both the variable order and the underlying MTBDD are equal.
   */
  bool operator==(const XDD<T, Term>& other) const {
    return (order_ == other.order_) && getMtbdd() == other.getMtbdd();
  }

  /**
   * @brief Get the top-level comparison variable of the diagram.
   * @return The comparison at the highest decision level, or an empty
   *         pointer if the diagram is a leaf.
   */
  [[nodiscard]] Term::ConstComparisonPointer getTopVar() const {
    return order_->getComparison(getMtbdd().TopVar());
  }

  /**
   * @brief Retrieve the "then" (high) child as the derived type.
   * @return The high child of the top node.
   */
  [[nodiscard]] T getThen() const {
    return T{{getMtbdd().Then(), order_}};
  }

  /**
   * @brief Retrieve the "else" (low) child as the derived type.
   * @return The low child of the top node.
   */
  [[nodiscard]] T getElse() const {
    return T{{getMtbdd().Else(), order_}};
  }

  /**
   * @brief Export the diagram to Graphviz DOT format.
   * @return A string containing the DOT representation.
   */
  [[nodiscard]] std::string toDot() const {
    gsl::owner<FILE*> file = std::tmpfile();
    if (file == nullptr) {
      throw std::runtime_error{"Can't create tmpfile!"};
    }

    sylvan::mtbdd_fprintdot(file, getMtbdd().GetMTBDD());

    std::fflush(file);
    if (std::fseek(file, 0, SEEK_END) != 0) {
      throw std::runtime_error{"Can't seek in tmpfile!"};
    }
    const auto filesize{std::ftell(file)};
    if (std::fseek(file, 0, SEEK_SET) != 0) {
      throw std::runtime_error{"Can't seek in tmpfile!"};
    }
    std::string rval{};
    rval.resize(filesize);
    if (std::fread(rval.data(), 1, filesize, file) != filesize) {
      throw std::runtime_error{"Read in tmpfile was short!"};
    }
    std::fclose(file);

    return rval;
  }

  /**
   * @brief Check whether the diagram is a leaf node.
   * @return true if the diagram has no decision levels.
   */
  [[nodiscard]] bool isLeaf() const {
    return getMtbdd().isLeaf();
  }

  /**
   * @brief Check whether the diagram represents the constant true value.
   * @return true if the diagram is a non-zero leaf.
   */
  [[nodiscard]] bool isTrue() const {
    return isLeaf() && !getMtbdd().isZero();
  }

  /**
   * @brief Check whether the diagram represents the constant false value.
   * @return true if the diagram is the zero leaf.
   */
  [[nodiscard]] bool isFalse() const {
    return getMtbdd().isZero();
  }

  /**
   * @brief Get the number of nodes in the diagram.
   * @return Node count of the underlying MTBDD.
   */
  [[nodiscard]] size_t getNodeCount() {
    return getMtbdd().NodeCount();
  }

  /**
   * @brief Get the number of cases of the diagram.
   * @return Number of root-to-leaf paths, i.e., the number of leaves of the
   *         tree obtained by unfolding all sharing of the underlying MTBDD.
   *
   * This is the number of cases of the case expression the diagram represents,
   * without merging cases that share a value. It can be exponentially larger
   * than the node count.
   */
  [[nodiscard]] mpz_class getCaseCount() const {
    std::unordered_map<decltype(getMtbdd().GetMTBDD()), mpz_class> memo;
    auto count = [&memo](auto&& self, auto node) -> mpz_class {
      if (sylvan::mtbdd_isleaf(node)) {
        return 1;
      }
      if (const auto it{memo.find(node)}; it != memo.end()) {
        return it->second;
      }
      mpz_class cases{self(self, sylvan::mtbdd_getlow(node)) +
                      self(self, sylvan::mtbdd_gethigh(node))};
      memo.emplace(node, cases);
      return cases;
    };
    return count(count, getMtbdd().GetMTBDD());
  }

  /**
   * @brief Transform the diagram to a new variable order.
   * @param order  The target variable order.
   * @return A new diagram in the requested order.
   *
   * The variable order is updated to include all variables from the current
   * order.  The diagram is composed with a variable reordering map.
   */
  [[nodiscard]] T withOrder(
      const VariableOrder<Term>::VariableOrderPointer& order) const {
    order->addVariables(order_->getComparisons());

    sylvan::MtbddMap reorder{};
    for (const auto& from : order_->getVariables()) {
      const auto& to{order->getVariable(order_->getComparison(from))};

      assert(to);
      reorder.put(from, sylvan::Bdd::bddVar(*to));
    }

    return T{{getMtbdd().Compose(reorder), order}};
  }

  /**
   * @brief Substitute a variable with a term in the diagram.
   * @param var  The variable to substitute.
   * @param term  The term to substitute it with.
   * @return A new diagram with the substitution applied.
   */
  [[nodiscard]] T substitute(const Term::Variable& var,
                             const Term::ConstTermPointer& term) const;

  /**
   * @brief Get the variable order associated with this diagram.
   * @brief Returns a reference to the variable order.
   */
  auto& getOrder() const {
    return order_;
  }

  /**
   * @brief Get the underlying Sylvan MTBDD.
   * @return reference to the underlying MTBDD.
   */
  auto& getMtbdd() const {
    return *mtbdd_;
  }

 private:
  VariableOrder<Term>::VariableOrderPointer order_{
      std::make_shared<VariableOrder<Term>>()};
  OrderedMtbddPointer<Term> mtbdd_{order_};
};
}  // namespace ddsolve

#endif /* XDD_HPP */
