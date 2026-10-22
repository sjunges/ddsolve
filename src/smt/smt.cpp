/**
 * @file smt.cpp
 * @brief SMT solving infrastructure for linear comparisons.
 *
 * This file defines a templated State class that wraps an SMT solver
 * backend (CVC5 or Z3, selected at compile time) and provides incremental
 * solving for conjunctions of linear comparisons.  It manages variable
 * declarations, assertion stacks, and model extraction.  The entry points
 * decideReal and decideUInt dispatch to the appropriate real-valued or
 * integer-valued wrapper.
 *
 * @note The Doxygen-style comments in this file were generated with the help
 *       of AI (Qwen3.6-35B-A3B). They have been human-checked on a best-effort
 *       basis. No lines of logic in this file were written or modified by AI.
 */

#include "probes.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <memory>
#include <ranges>
#include <set>
#include <syncstream>
#include <version>

#include "ArithmeticTypes.hpp"
#include "LinearTerm.hpp"
#include "MetricMeasure.hpp"
#include "SmtWrapper.hpp"
#include "smt/smt_config.hpp"

namespace {

#if SMT_SOLVER == CVC5
using RealValuedWrapper = ddsolve::smt::Cvc5Wrapper<true>;
using IntValuedWrapper = ddsolve::smt::Cvc5Wrapper<false>;
#endif

#if SMT_SOLVER == Z3
using RealValuedWrapper = ddsolve::smt::Z3Wrapper<true>;
using IntValuedWrapper = ddsolve::smt::Z3Wrapper<false>;
#endif

#ifdef MEASURE_SMT
ddsolve::MetricMeasure<MEASURE_SMT_MAX_THREADS> gMeasure{};
#endif

/**
 * @brief Wraps an SMT solver backend for incremental solving of linear
 *        comparison conjunctions.
 *
 * Manages variable declarations, assertion stacks, and model extraction.
 * Supports both real-valued and integer-valued solving via different
 * Wrapper instantiations.  The class maintains thread-local state
 * (via getState<Wrapper>) for incremental solving, comparing queries
 * against the previous one to avoid redundant work.
 *
 * @tparam Wrapper  The SMT solver wrapper type (Cvc5Wrapper or Z3Wrapper).
 */
template <typename Wrapper>
class State {
 public:
  using SmtResult = ddsolve::expression::SmtResult;

  /**
   * @brief Solve a conjunction of linear comparisons using the SMT solver.
   *
   * Manages incremental solving by comparing against the previous query,
   * popping stale assertions, pushing new variables and assertions, and
   * calling the solver.  Optionally prints the model on satisfaction.
   *
   * @param conjunction  The linear comparison conjunction to decide.
   * @param print_model  Whether to print the satisfying model on stdout.
   * @return The SMT result (sat, unsat, or unknown).
   */
  [[nodiscard]] SmtResult decide(
      std::vector<ddsolve::expression::LinearComparison::WithParity>
          conjunction,
      const bool print_model) {
    auto var_ids{gatherVariables(conjunction)};

#ifdef USE_INCREMENTAL_SMT

    const auto mismatch_result{mismatch(conjunction)};

#ifdef SMT_DEBUG
    static std::mutex m{};
    m.lock();
    std::cout << "--- ---\n";
    for (const auto& i : last_querry_) {
      std::cout << (i.parity ? ' ' : '!') << i.comparison->str() << "; ";
    }
    std::cout << '\n';
    for (const auto& i : conjunction) {
      std::cout << (i.parity ? ' ' : '!') << i.comparison->str() << "; ";
    }
    std::cout << '\n';
    for (const auto& i :
         std::ranges::subrange(last_querry_.begin(), mismatch_result.in1)) {
      std::cout << (i.parity ? ' ' : '!') << i.comparison->str() << "; ";
    }
    std::cout << '\n';
#endif

    popAssertions(
        std::ranges::subrange(mismatch_result.in1, last_querry_.end()),
        var_ids);
    pushVariables(var_ids);

#ifdef SMT_DEBUG
    for (const auto& i : wrapper_.getAssertions()) {
      std::cout << i << "; ";
    }
    std::cout << '\n';
#endif

    pushAssertions(
        std::ranges::subrange(mismatch_result.in2, conjunction.end()));

    last_querry_ = std::move(conjunction);

#ifdef SMT_DEBUG
    for (const auto& i : wrapper_.getAssertions()) {
      std::cout << i << "; ";
    }
    std::cout << "\n---\n" << std::flush;
#endif

#ifdef MEASURE_SMT
    gMeasure.start();
#endif
    DTRACE_PROBE(ddsolve, solve_start);
    const auto rval{wrapper_.checkSat()};
    DTRACE_PROBE(ddsolve, solve_stop);
#ifdef MEASURE_SMT
    gMeasure.stop();
#endif

#else
    for (const auto& var_id : var_ids) {
      const auto term = wrapper_.mkComparison(
          zero_, variables_[var_id],
          ddsolve::expression::ComparisonType::kLessThanOrEqual);
      wrapper_.addAssertion(term);
    }
    wrapper_.push();
    for (const auto& comp : conjunction) {
      wrapper_.addAssertion(asExpression(comp));
    }

    const auto rval{wrapper_.checkSat()};
    wrapper_.pop();
#endif

#ifdef SMT_DEBUG
    if (rval == SmtResult::kSat) {
#else
    if (print_model && rval == SmtResult::kSat) {
#endif
      std::vector<Expression> variables{std::views::values(variables_) |
                                        std::ranges::to<std::vector>()};
#ifdef __cpp_lib_syncbuf
      std::osyncstream oss{std::cout};
#else
      std::ostream& oss{std::cout};
#endif

      oss << "{\n";
      // is_first hack is ugly
      bool is_first = true;
      for (auto&& [x, y] : std::views::drop(
               std::views::zip(variables, wrapper_.getValues(variables)), 1)) {
        if (!is_first) {
          oss << ",\n";
        }
        oss << '"' << x << '"' << ": " << y;
        is_first = false;
      }
      oss << "\n}\n";
    }
#ifdef SMT_DEBUG
    std::cout << "SatResult: " << ddsolve::expression::smtResultToStr(rval)
              << std::flush;
    m.unlock();
#endif

    return rval;
  }

 private:
  using Expression = Wrapper::Expression;

  /**
   * @brief Push assertions from a range onto the solver stack.
   *
   * Wraps each assertion in its own push level and increments
   * @p assertion_level_ to track nesting.
   *
   * @tparam R  A forward range of LinearComparison::WithParity.
   * @param tmp_range  The range of assertions to push.
   */
  template <std::ranges::forward_range R>
  void pushAssertions(R&& tmp_range) {
    for (const auto& comp : std::forward<R>(tmp_range)) {
      wrapper_.push();
      ++assertion_level_;
      wrapper_.addAssertion(asExpression(comp));
    }
  }

  /**
   * @brief Pop assertions from the solver stack and reclaim variable IDs.
   *
   * Pops assertions matching @p tmp_range, merging their associated variable
   * IDs into @p var_ids.  Handles the variable assertion tracking stack to
   * correctly manage per-variable assertion scopes.
   *
   * @tparam R     A forward range of LinearComparison::WithParity.
   * @param tmp_range  The range of assertions to pop.
   * @param var_ids    Set to merge reclaimed variable IDs into.
   */
  template <std::ranges::forward_range R>
  void popAssertions(R&& tmp_range,
                     std::set<ddsolve::expression::VariableId>& var_ids) {
    for (const auto& i : std::forward<R>(tmp_range)) {
      auto last_variable_assertions{variable_assertions_.empty()
                                        ? -1
                                        : variable_assertions_.back().first};
      while (assertion_level_ == last_variable_assertions) {
        var_ids.merge(variable_assertions_.back().second);
        variable_assertions_.pop_back();
        wrapper_.pop();
        --assertion_level_;

        last_variable_assertions = variable_assertions_.empty()
                                       ? -1
                                       : variable_assertions_.back().first;
      }

      wrapper_.pop();
      --assertion_level_;
    }
  }

  /**
   * @brief Push variable declarations and their non-negativity constraints.
   *
   * For each variable ID, declares the variable (if not already declared)
   * and asserts that it is greater than or equal to zero.  Wraps all
   * variable assertions in a single push level tracked by
   * @p variable_assertions_.
   *
   * @param var_ids  Set of variable IDs to declare and constrain.
   */
  void pushVariables(std::set<ddsolve::expression::VariableId>& var_ids) {
    if (!var_ids.empty()) {
      wrapper_.push();
      ++assertion_level_;
      variable_assertions_.emplace_back(assertion_level_, var_ids);

      for (const auto& var_id : var_ids) {
        const auto term = wrapper_.mkComparison(
            zero_, variables_[var_id],
            ddsolve::expression::ComparisonType::kLessThanOrEqual);
        wrapper_.addAssertion(term);
      }
    }
  }

  /**
   * @brief Collect variable IDs from a conjunction and declare them.
   *
   * Iterates over all terms in every comparison, registers new variables
   * with the solver, and returns the full set of variable IDs.
   *
   * @param conjunction  The conjunction of linear comparisons.
   * @return Set of variable IDs found in the conjunction.
   */
  std::set<ddsolve::expression::VariableId> gatherVariables(
      const std::vector<ddsolve::expression::LinearComparison::WithParity>&
          conjunction) {
    std::set<ddsolve::expression::VariableId> ids;

    for (const auto& comp : conjunction) {
      for (const auto& i : comp.comparison->getTerm()) {
        if (!variables_.contains(i.variable)) {
          ids.insert(i.variable);
          const auto variable =
              wrapper_.mkConst("x_" + std::to_string(i.variable));

          variables_[i.variable] = variable;
        }
      }
    }

    return ids;
  }

  /**
   * @brief Push a range of formulas onto the solver stack.
   *
   * Wraps each formula in its own push level and increments
   * @p assertion_level_ to track nesting.
   *
   * @tparam R  A range of solver expressions.
   * @param term_range  The formulas to push.
   */
  template <typename R>
  void push_formulas(R term_range) {
    for (const auto& term : term_range) {
      wrapper_.push();
      wrapper_.addAssertion(term);
      ++assertion_level_;
    }
  }

  /**
   * @brief Find the point of difference between the last query and a new one.
   *
   * Uses std::ranges::mismatch to compare @p last_querry_ against
   * @p current_querry, matching on parity and comparison equality.
   *
   * @param current_querry  The new query to compare against the last one.
   * @return The mismatch result containing iterators into both ranges.
   */
  auto mismatch(
      const std::vector<ddsolve::expression::LinearComparison::WithParity>&
          current_querry) {
    const auto rval{std::ranges::mismatch(
        last_querry_, current_querry, [](const auto& lhs, const auto& rhs) {
          return lhs.parity == rhs.parity && *lhs.comparison == *rhs.comparison;
        })};
    return rval;
  }

  /**
   * @brief Convert a linear comparison with parity into a solver expression.
   *
   * Handles trivially true/false comparisons, builds the sum of coefficients
   * and variables, forms the comparison, and negates it if the parity is
   * false.
   *
   * @param comp  The linear comparison with its parity flag.
   * @return The corresponding solver expression.
   */
  Expression asExpression(
      const ddsolve::expression::LinearComparison::WithParity& comp) {
    const bool is_negated = !comp.parity;
    // This (hopefully) deals with infinity too
    if (const auto check{comp.comparison->trivialCheck()};
        check != SmtResult::kUnknown) {
      const auto tmp{is_negated ? ddsolve::expression::flip(check) : check};

      const bool value{tmp != SmtResult::kUnsat};
      return value ? wrapper_.mkTrue() : wrapper_.mkFalse();
    }

    std::vector<Expression> sum{};
    sum.reserve(comp.comparison->getTerm().size());
    for (const auto& product : comp.comparison->getTerm()) {
      const auto& var{variables_.at(product.variable)};
      const auto coefficent{wrapper_.mkReal(product.coefficent.get_str())};
      sum.push_back(wrapper_.mkProduct({coefficent, var}));
    }

    Expression rval = wrapper_.mkComparison(wrapper_.mkSum(sum), zero_,
                                            comp.comparison->getType());

    if (is_negated) {
      rval = wrapper_.mkNegation(rval);
    }

    return rval;
  }

  Wrapper wrapper_{};
  Expression zero_{wrapper_.mkReal("0")};

  // The '0' variable is actully the constant one
  std::map<ddsolve::expression::VariableId, Expression> variables_{
      {0, wrapper_.mkReal("1")}};

  std::vector<std::pair<int, std::set<ddsolve::expression::VariableId>>>
      variable_assertions_;
  size_t assertion_level_{};

  std::vector<ddsolve::expression::LinearComparison::WithParity> last_querry_;
};

/**
 * @brief Get the thread-local SMT solving state for a given wrapper type.
 *
 * Returns a reference to a thread-local unique_ptr wrapping a State<Wrapper>.
 * Each thread has its own independent solver state for incremental solving.
 *
 * @tparam Wrapper  The SMT solver wrapper type (Cvc5Wrapper or Z3Wrapper).
 * @return Reference to the thread-local state.
 */
template <typename Wrapper>
auto& getState() {
  thread_local auto state{std::make_unique<State<Wrapper>>()};
  return state;
}

}  // namespace

namespace ddsolve::expression {

/**
 * @brief Decide satisfiability of a conjunction of real-valued comparisons.
 *
 * Delegates to the real-valued SMT solver backend via its thread-local state.
 *
 * @return The SMT result (sat, unsat, or unknown).
 */
[[nodiscard]] SmtResult LinearComparison::decideReal(
    const std::vector<LinearComparison::WithParity>& conjunction,
    const bool print_model) {
  STAP_PROBE(ddsolve, use_smt_real);
  auto& state{*getState<RealValuedWrapper>()};

  return state.decide(conjunction, print_model);
}

/**
 * @brief Decide satisfiability of a conjunction of unsigned integer
 * comparisons.
 *
 * Delegates to the integer-valued SMT solver backend via its thread-local
 * state.
 *
 * @return The SMT result (sat, unsat, or unknown).
 */
[[nodiscard]] SmtResult LinearComparison::decideUInt(
    const std::vector<LinearComparison::WithParity>& conjunction,
    const bool print_model) {
  STAP_PROBE(ddsolve, use_smt_uint);
  auto& state{*getState<IntValuedWrapper>()};

  return state.decide(conjunction, print_model);
}

/**
 * @brief Shut down all SMT solver backends and release resources.
 *
 * Resets both the real-valued and integer-valued thread-local states.
 * Writes SMT measurement data if MEASURE_SMT is enabled.
 */
void LinearComparison::shutdown() {
  static std::atomic_bool printed{false};
  getState<RealValuedWrapper>().reset();
  getState<IntValuedWrapper>().reset();

#ifdef MEASURE_SMT
  if (!printed.exchange(true, std::memory_order_relaxed)) {
    gMeasure.write();
  }
#endif
}

}  // namespace ddsolve::expression
