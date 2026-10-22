/**
 * @file SmtTerm.cpp
 * @brief Implementation of SMT term and comparison operations using CVC5.
 *
 * This file provides the concrete implementation of SmtTerm and SmtComparison
 * methods.  It uses a thread-local CVC5 solver (via the parser interface) to
 * parse, simplify, and evaluate SMT-LIB string expressions.  Global state
 * manages symbol declarations and assumption stacks shared across threads.
 *
 * @note The Doxygen-style comments in this file were generated with the help
 *       of AI (Qwen3.6-35B-A3B). They have been human-checked on a best-effort
 *       basis. No lines of logic in this file were written or modified by AI.
 */

#include "SmtTerm.hpp"

#include <cvc5/cvc5.h>
#include <cvc5/cvc5_parser.h>
#include "probes.hpp"

#include <algorithm>
#include <memory>
#include <mutex>
#include <ranges>
#include <shared_mutex>
#include <vector>

#include "ArithmeticTypes.hpp"
#include "smt/Cvc5Wrapper.hpp"
#include "smt/SmtWrapper.hpp"
#include "smt/smt_config.hpp"

namespace {
// #if SMT_SOLVER == CVC5
// #endif

// #if SMT_SOLVER == Z3
//   using RealValuedWrapper = ddsolve::smt::Z3Wrapper<true>;
//   using IntValuedWrapper = ddsolve::smt::Z3Wrapper<false>;
// #endif

class ConstGatherer {
 public:
  void visit(const cvc5::Term& term) {
    if (term.hasSymbol() && term.getKind() != cvc5::Kind::VARIABLE) {
      terms_.insert(term);
    } else {
      for (auto&& c : term) {
        visit(c);
      }
    }
  }

  void visit(const std::vector<cvc5::Term>& terms) {
    for (auto&& term : terms) {
      visit(term);
    }
  }

  [[nodiscard]]
  const std::set<cvc5::Term>& getTerms() const {
    return terms_;
  }

 private:
  std::set<cvc5::Term> terms_;
};

class GlobalState {
 public:
  [[nodiscard]]
  std::pair<std::shared_lock<std::shared_mutex>, std::vector<std::string>*>
  getCmds() {
    return std::make_pair(std::shared_lock{mutex}, &cmds_);
  }

  void addSymbol(std::string symbol, std::string cmd) {
    {
      std::shared_lock lock{mutex};
      if (auto iter{symbols_.find(symbol)}; iter != symbols_.end()) {
        return;
      }
    }
    std::unique_lock lock{mutex};
    // std::cerr << cmd << '\n';
    symbols_.insert(std::move(symbol));
    cmds_.push_back(std::move(cmd));
  }

  void pushAssumption(
      ddsolve::expression::SmtComparison::WithParity assumption) {
    std::unique_lock lock{mutex};
    assumptions_.push_back(std::move(assumption));
    ++assumptions_tag_;
  }

  void popAssumption() {
    std::unique_lock lock{mutex};
    assumptions_.pop_back();
    ++assumptions_tag_;
  }

  std::tuple<std::shared_lock<std::shared_mutex>, size_t,
             const std::vector<ddsolve::expression::SmtComparison::WithParity>*>
  getAssumptions() {
    return {std::shared_lock{mutex}, assumptions_tag_, &assumptions_};
  }

 private:
  std::shared_mutex mutex;
  std::set<std::string> symbols_;
  std::vector<std::string> cmds_;
  std::vector<ddsolve::expression::SmtComparison::WithParity> assumptions_;
  size_t assumptions_tag_{};
} gGlobalState{};

class LocalState {
 private:
  auto parseTerm(const ddsolve::expression::SmtTerm& term) {
    try {
#ifdef USE_SMTTERM_CACHE
      if (cache_[term.hash() & 1023].first == term.str()) {
        // std::cerr << "HIT!!!!\n";
        DTRACE_PROBE(ddsolve, smtterm_cache_hit);
        return cache_[term.hash() & 1023].second;
      }
      DTRACE_PROBE(ddsolve, smtterm_cache_miss);
#endif

      parser_.appendIncrementalStringInput(term.str());
      parser_.appendIncrementalStringInput("\n");

      auto tmp{parser_.nextTerm()};
      while (tmp.isNull()) {
        tmp = parser_.nextTerm();
      }

#ifdef USE_SMTTERM_CACHE
      cache_[term.hash() & 1023] = std::make_pair(term.str(), tmp);
#endif
      return tmp;
    } catch (cvc5::parser::ParserException& e) {
      std::cerr << "Something went wrong parsing an SmtTerm: " << e.what()
                << '\n';
      throw;
    }
  }

  auto parseWithParity(
      const ddsolve::expression::SmtComparison::WithParity& wp) {
    if (wp.parity) {
      return parseTerm(wp.comparison->getTerm());
    }
    ddsolve::expression::SmtTerm negated{"(not " + wp.comparison->str() + ")"};
    return parseTerm(negated);
  }

 public:
  explicit LocalState() {
    solver_.setLogic("ALL");
    solver_.setOption("produce-models", "true");
    solver_.setOption("tlimit-per", "5000");

    // solver_.setOption("enum-inst", "true");
    // solver_.setOption("finite-model-find", "true");
    solver_.setOption("mbqi", "true");

    parser_.setIncrementalStringInput(cvc5::modes::InputLanguage::SMT_LIB_2_6,
                                      "InternalCmdSrc");
  }

  template <typename R>
  auto mkTerm(cvc5::Kind kind, R&& range) {
    DTRACE_PROBE(ddsolve, smtterm_mkTerm);
    updateCmds();

    std::vector<cvc5::Term> terms{std::forward<R>(range) |
                                  std::views::transform([this](auto&& i) {
                                    return this->parseTerm(*i);
                                  }) |
                                  std::ranges::to<std::vector>()};

    return solver_.simplify(term_manager_.mkTerm(kind, terms), false);
  }

  auto substitute(const ddsolve::expression::SmtTerm& lhs,
                  const ddsolve::expression::SmtTerm& var,
                  const ddsolve::expression::SmtTerm& rhs) {
    updateCmds();
    return solver_.simplify(
        parseTerm(lhs).substitute(parseTerm(var), parseTerm(rhs)), false);
  }

  auto simplify(const ddsolve::expression::SmtTerm& term) {
    return solver_.simplify(parseTerm(term), false);
  }

  ddsolve::expression::SmtResult decide(
      const std::vector<ddsolve::expression::SmtComparison::WithParity>&
          conjunction,
      const bool print_model) {
    using ddsolve::expression::SmtResult;
    updateAssumptions();

    solver_.push();
    for (auto&& i : conjunction) {
      solver_.assertFormula(parseWithParity(i));
    }

#ifdef SMT_DEBUG
    std::cout << "\n" << std::flush;

    for (const auto& i : solver_.getAssertions()) {
      std::cout << i << "; ";
    }
    std::cout << '\n';
#endif

    DTRACE_PROBE(ddsolve, solve_start);
    const auto result{solver_.checkSat()};
    DTRACE_PROBE(ddsolve, solve_stop);

    auto rval{SmtResult::kUnknown};
    if (result.isSat()) {
      rval = SmtResult::kSat;
    }
    if (result.isUnsat()) {
      rval = SmtResult::kUnsat;
    }

#ifdef SMT_DEBUG
    std::cout << smtResultToStr(rval) << '\n';
#endif
#ifdef SMT_DEBUG
    if (rval == SmtResult::kSat) {
#else
    if (print_model && rval == SmtResult::kSat) {
#endif

      ConstGatherer gatherer{};
      gatherer.visit(solver_.getAssertions());
      std::cout << "{\n";
      for (auto&& i : gatherer.getTerms()) {
        std::cout << "  " << i.getSymbol() << " = " << solver_.getValue(i)
                  << ";\n";
      }
      std::cout << "}\n";
    }

    solver_.pop();
    return rval;
  }

 private:
  size_t last_cmd_idx_{};
  size_t last_assumption_tag_{};
  cvc5::TermManager term_manager_;
  cvc5::Solver solver_{term_manager_};
  cvc5::parser::SymbolManager symbol_manager_{term_manager_};
  cvc5::parser::InputParser parser_{&solver_, &symbol_manager_};

#ifdef USE_SMTTERM_CACHE
  std::vector<std::pair<std::string, cvc5::Term>> cache_{1024};
#endif

  void updateCmds() {
    auto tmp{gGlobalState.getCmds()};
    for (; last_cmd_idx_ < tmp.second->size(); ++last_cmd_idx_) {
      parser_.appendIncrementalStringInput((*tmp.second)[last_cmd_idx_]);
      parser_.appendIncrementalStringInput("\n");
      auto cmd{parser_.nextCommand()};
      while (cmd.isNull()) {
        cmd = parser_.nextCommand();
      }

      cmd.invoke(&solver_, &symbol_manager_, std::cerr);
    }
  }

  void updateAssumptions() {
    updateCmds();
    auto [lock, assumption_tag, assumptions]{gGlobalState.getAssumptions()};
    if (assumption_tag == last_assumption_tag_) {
      return;
    }
    last_assumption_tag_ = assumption_tag;

    solver_.resetAssertions();
    for (auto&& i : *assumptions) {
      solver_.assertFormula(parseWithParity(i));
    }
  }
};

auto& getLocalStatePtr() {
  thread_local auto state{std::make_unique<LocalState>()};

  return state;
}

}  // namespace

namespace ddsolve::expression {

SmtTerm SmtTerm::simplify() const {
  if (isSimplified_) {
    return *this;
  }

  auto& ls{*getLocalStatePtr()};
  return SmtTerm{ls.simplify(*this).toString()};
}

SmtTerm SmtTerm::mkIntVar(const std::string& name) {
  gGlobalState.addSymbol(name, "(declare-const " + name + " Int)");

  return SmtTerm{name};
}

SmtTerm SmtTerm::mkRealVar(const std::string& name) {
  gGlobalState.addSymbol(name, "(declare-const " + name + " Real)");

  return SmtTerm{name};
}

SmtTerm SmtTerm::mkReal(const std::string& value) {
  return SmtTerm{value, false}.simplify();
}

SmtTerm SmtTerm::mkInt(const std::string& value) {
  return SmtTerm{value};
}

SmtTerm SmtTerm::operator+(const SmtTerm& other) const {
  auto& ls{*getLocalStatePtr()};

  return SmtTerm{
      ls.mkTerm(cvc5::Kind::ADD, std::vector{this, &other}).toString()};
}

SmtTerm SmtTerm::operator/(const SmtTerm& other) const {
  auto& ls{*getLocalStatePtr()};

  return SmtTerm{
      ls.mkTerm(cvc5::Kind::DIVISION, std::vector{this, &other}).toString()};
}

SmtTerm SmtTerm::operator*(const SmtTerm& other) const {
  auto& ls{*getLocalStatePtr()};

  return SmtTerm{
      ls.mkTerm(cvc5::Kind::MULT, std::vector{this, &other}).toString()};
}

SmtTerm SmtTerm::operator-(const SmtTerm& other) const {
  auto& ls{*getLocalStatePtr()};

  return SmtTerm{
      ls.mkTerm(cvc5::Kind::SUB, std::vector{this, &other}).toString()};
}

SmtTerm SmtTerm::substitute(const Variable& var, const SmtTerm& other) const {
  auto& ls{*getLocalStatePtr()};

  return SmtTerm{ls.substitute(*this, var, other).toString()};
}

SmtComparison SmtComparison::substitute(const SmtTerm::Variable& var,
                                        const SmtTerm& other) const {
  auto& ls{*getLocalStatePtr()};

  return SmtComparison{ls.substitute(term_, var, other).toString()};
}

[[nodiscard]] SmtResult SmtComparison::decide(
    const std::vector<WithParity>& conjunction, bool print_model) {
  return getLocalStatePtr()->decide(conjunction, print_model);
  // return SmtResult::kUnknown;
}

void SmtComparison::shutdown() {
  getLocalStatePtr().reset();
}

SmtComparison::SmtComparison(const SmtTerm& lhs, const SmtTerm& rhs,
                             const ComparisonType& type) {
  auto& ls{*getLocalStatePtr()};
  term_ =
      SmtTerm{ls.mkTerm(smt::cvc5TranslateToKind(type), std::vector{&lhs, &rhs})
                  .toString()};
}

SmtTerm SmtTerm::mkRealFunction(const std::string& name, unsigned int arity) {
  std::stringstream ss{};
  ss << "(declare-fun " << name << "(";
  for (int k{}; k < arity; ++k) {
    ss << "Real ";
  }
  ss << ") Real)";

  gGlobalState.addSymbol(name, ss.str());

  return SmtTerm{name};
}

SmtTerm SmtTerm::mkIntFunction(const std::string& name, unsigned int arity) {
  std::stringstream ss{};
  ss << "(declare-fun " << name << "(";
  for (int k{}; k < arity; ++k) {
    ss << "Int ";
  }
  ss << ") Int)";

  gGlobalState.addSymbol(name, ss.str());

  return SmtTerm{name};
}

SmtTerm SmtTerm::applyFunction(const std::vector<SmtTerm>& arguments) const {
  auto& ls{*getLocalStatePtr()};
  std::vector<const SmtTerm*> tmp{};
  tmp.reserve(arguments.size() + 1);
  tmp.push_back(this);
  for (const auto& i : arguments) {
    tmp.push_back(&i);
  }

  return SmtTerm{ls.mkTerm(cvc5::Kind::APPLY_UF, tmp).toString()};
}

[[nodiscard]] SmtTerm SmtTerm::mkIntArray(const std::string& name) {
  gGlobalState.addSymbol(name, "(declare-const " + name + " (Array Int Int))");
  return SmtTerm{name};
}

[[nodiscard]] SmtTerm SmtTerm::mkRealArray(const std::string& name) {
  gGlobalState.addSymbol(name, "(declare-const " + name + " (Array Int Real))");
  return SmtTerm{name};
}

[[nodiscard]]
SmtTerm SmtTerm::arrayStore(const SmtTerm& index, const SmtTerm& value) const {
  std::stringstream ss{};
  ss << "(store " << term_ << ' ' << index.term_ << ' ' << value.term_ << ')';
  return SmtTerm{ss.str(), false};
}

[[nodiscard]]
SmtTerm SmtTerm::arraySelect(const SmtTerm& index) const {
  auto& ls{*getLocalStatePtr()};

  return SmtTerm{
      ls.mkTerm(cvc5::Kind::SELECT, std::vector{this, &index}).toString()};
}

[[nodiscard]]
SmtTerm SmtTerm::toReal() const {
  auto& ls{*getLocalStatePtr()};

  return SmtTerm{ls.mkTerm(cvc5::Kind::TO_REAL, std::vector{this}).toString()};
}

[[nodiscard]]
SmtTerm SmtTerm::toInt() const {
  auto& ls{*getLocalStatePtr()};

  return SmtTerm{
      ls.mkTerm(cvc5::Kind::TO_INTEGER, std::vector{this}).toString()};
}

void SmtComparison::pushAssumption(const WithParity& assumption) {
  gGlobalState.pushAssumption(assumption);
}

void SmtComparison::popAssumption() {
  gGlobalState.popAssumption();
}

[[nodiscard]] SmtComparison SmtComparison::operator&(
    const SmtComparison& other) const {
  auto& ls{*getLocalStatePtr()};
  return SmtComparison{
      ls.mkTerm(cvc5::Kind::AND, std::vector{&term_, &other.term_}).toString()};
}

[[nodiscard]] SmtComparison SmtComparison::operator|(
    const SmtComparison& other) const {
  auto& ls{*getLocalStatePtr()};
  return SmtComparison{
      ls.mkTerm(cvc5::Kind::OR, std::vector{&term_, &other.term_}).toString()};
}

[[nodiscard]] SmtComparison SmtComparison::operator~() const {
  auto& ls{*getLocalStatePtr()};
  return SmtComparison{
      ls.mkTerm(cvc5::Kind::NOT, std::vector{&term_}).toString()};
}

[[nodiscard]] SmtComparison SmtComparison::mkForallInt(
    const SmtTerm& var, const SmtComparison& expr) {
  auto& ls{*getLocalStatePtr()};

  SmtTerm tmp{"(forall ((" + var.str() + " Int)) " + expr.str() + ")"};
  return SmtComparison{ls.simplify(tmp).toString()};
}

[[nodiscard]] SmtComparison SmtComparison::mkForallReal(
    const SmtTerm& var, const SmtComparison& expr) {
  auto& ls{*getLocalStatePtr()};

  SmtTerm tmp{"(forall ((" + var.str() + " Real)) " + expr.str() + ")"};
  return SmtComparison{ls.simplify(tmp).toString()};
}

}  // namespace ddsolve::expression
