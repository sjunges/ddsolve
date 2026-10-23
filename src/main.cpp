/**
 * @file main.cpp
 * @brief Python bindings for the decision diagram solver.
 *
 * This file defines the nanobind module that exposes the core solver
 * functionality to Python.  It registers classes for LinearTerm, SmtTerm,
 * LinearComparison, SmtComparison, VariableOrder, XBDD, and XADD along with
 * their operators and factory methods.  Sylvan is initialised at module load
 * time and cleaned up via an atexit handler.
 *
 * @note The Doxygen-style comments in this file were generated with the help
 *       of AI (Qwen3.6-35B-A3B). They have been human-checked on a best-effort
 *       basis. No lines of logic in this file were written or modified by AI.
 */

#include <nanobind/make_iterator.h>
#include <nanobind/nanobind.h>
#include <nanobind/operators.h>
#include <nanobind/stl/shared_ptr.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include <charconv>
#include <cstdlib>
#include <memory>
#include <sylvan_obj.hpp>

#include "ArithmeticTypes.hpp"
#include "LinearTerm.hpp"
#include "SmtTerm.hpp"
#include "VariableOrder.hpp"
#include "XADD.hpp"
#include "XBDD.hpp"
#include "sylvan/SylvanInterface.hpp"
#include "sylvan/sylvan_ddsolve.h"

// auto mi() {
//   ddsolve::XADD<ddsolve::expression::SmtTerm> r1{};
//   ddsolve::XADD<ddsolve::expression::SmtTerm> r2{};
//
//   return r1 + r2;
// }

namespace {

/**
 * @brief RAII manager for Sylvan's Lace threading library.
 *
 * Resumes Lace on enter (acquiring a thread) and suspends it on exit
 * (releasing the thread).  This allows safe interaction with Sylvan
 * from Python code that may run without the GIL.
 */
struct LaceManager {
  /**
   * @brief Resume Lace, acquiring a thread for use.
   */
  static void enter() {
    lace_resume();
  }

  /**
   * @brief Suspend Lace, releasing the current thread.
   */
  static void exit() {
    lace_suspend();
  }
};

/**
 * @brief RAII manager for enabling/disabling model printing.
 *
 * Enables printing of solver models on enter and disables it on exit.
 */
struct PrintModelsManager {
  /**
   * @brief Enable model printing.
   */
  static void enter() {
    ddsolve::interface::printModels(true);
  }

  /**
   * @brief Disable model printing.
   */
  static void exit() {
    ddsolve::interface::printModels(false);
  }
};

/**
 * @brief Clean up Sylvan resources when the module is unloaded.
 *
 * Reports Sylvan statistics to stderr, then gracefully shuts down
 * ddsolve, Sylvan, and Lace.  Called via Python's atexit mechanism.
 */
void cleanup() noexcept {
  nanobind::gil_scoped_release guard{};

  sylvan::sylvan_stats_report(stderr);

  lace_resume();
  sylvan::sylvan_quit_ddsolve();
  sylvan::sylvan_quit();
  lace_stop();
}

/**
 * @brief Retrieve the value of an environment variable.
 * @param env_var  The name of the environment variable.
 * @return The value as a string view, or an empty string view if the
 *         variable is not set.
 */
std::string_view getEnvironmentVariable(const char* env_var) {
  const auto* env{std::getenv(env_var)};

  if (env != nullptr) {
    return env;
  }

  return {};
}

/**
 * @brief Retrieve an environment variable as a 64-bit integer.
 * @param env_var  The name of the environment variable.
 * @param default_  The value to return if the variable is unset or invalid.
 * @return The parsed integer value, or @p default_ on failure.
 */
// NOLINTNEXTLINE(google-runtime-int)
int64_t getIntegerEnvironmentVariable(const char* env_var, long default_) {
  constexpr auto kbase{10};
  const auto str{getEnvironmentVariable(env_var)};

  if (str.size() > 0) {
    int64_t rval{};
    const auto error{
        std::from_chars(str.data(), str.data() + str.size(), rval)};
    if (error.ec == std::errc{} && error.ptr == str.data() + str.size()) {
      return rval;
    }
  }

  return default_;
}

/**
 * @brief Number of cases of a diagram as an arbitrary-precision Python integer.
 * @tparam Diagram  The diagram type.
 * @param diagram  The diagram to count the cases of.
 * @return The value of Diagram::getCaseCount() as a Python int.
 */
template <typename Diagram>
nanobind::object caseCountToPython(const Diagram& diagram) {
  const auto digits{diagram.getCaseCount().get_str()};
  return nanobind::steal(PyLong_FromString(digits.c_str(), nullptr, 10));
}

}  // namespace

// The NB_MODULE macro is outside our controll, so no linting wanted
NB_MODULE(_ddsolve, m) {  // NOLINT
  constexpr size_t ksylvan_default_max_memory{10UL * 1024 * 1024 * 1024};
  const size_t sylvan_max_memory{
      static_cast<size_t>(getIntegerEnvironmentVariable(
          "SYLVAN_MAXMEM", ksylvan_default_max_memory))};

  //  Initial tablesize is 2^-sylvan_initial_ratio of sylvan_max_memory
  constexpr int ksylvan_default_initial_ratio{0};
  const int sylvan_initial_ratio{static_cast<int>(getIntegerEnvironmentVariable(
      "SYLVAN_INITIAL_RATIO", ksylvan_default_initial_ratio))};

  constexpr int ksylvan_default_table_ratio{0};
  const int sylvan_table_ratio{static_cast<int>(getIntegerEnvironmentVariable(
      "SYLVAN_TABLE_RATIO", ksylvan_default_table_ratio))};

  constexpr int klace_default_workers{1};
  const int klace_workers{static_cast<int>(
      getIntegerEnvironmentVariable("LACE_WORKERS", klace_default_workers))};

  lace_start(klace_workers, 0);
  sylvan::sylvan_set_limits(sylvan_max_memory, sylvan_table_ratio,
                            sylvan_initial_ratio);
  sylvan::sylvan_init_package();
  sylvan::sylvan_init_mtbdd();
  sylvan::sylvan_init_ddsolve();

  lace_suspend();

  const auto f = nanobind::cpp_function(&cleanup);

  auto atexit = nanobind::module_::import_("atexit");
  atexit.attr("register")(nanobind::cpp_function(&cleanup));

  // The lambdas are here because of a language defect that the
  // EWG determined to be breaking too
  // much code to change (EWG decision89)...

  m.def("sylvan_gc", []() { sylvan::sylvan_gc_RUN(); });
  m.def("sylvan_count_refs", []() {
    return sylvan::mtbdd_count_refs() + sylvan::mtbdd_count_protected();
  });

  nanobind::class_<LaceManager>(m, "LaceManager")
      .def(nanobind::init<>())
      .def_static("__enter__", &LaceManager::enter)
      .def_static("__exit__", [](nanobind::args&) { LaceManager::exit(); });

  nanobind::class_<PrintModelsManager>(m, "PrintModelsManager")
      .def(nanobind::init<>())
      .def_static("__enter__", &PrintModelsManager::enter)
      .def_static("__exit__",
                  [](nanobind::args&) { PrintModelsManager::exit(); });

  nanobind::class_<ddsolve::expression::Product>(m, "Product")
      .def(nanobind::init<ddsolve::expression::VariableId, int>())
      .def("__init__",
           [](ddsolve::expression::Product* product,
              const ddsolve::expression::VariableId& var,
              const std::string& str) {
             new (product) ddsolve::expression::Product(
                 var,
                 ddsolve::expression::LinearTerm::coefficientFromString(str));
           })
      .def("__repr__",
           [](const std::shared_ptr<const ddsolve::expression::Product>& prod) {
             return ddsolve::expression::LinearTerm{{*prod}}.str();
           })
      .def("get_variable",
           [](const std::shared_ptr<const ddsolve::expression::Product>& prod) {
             return prod->variable;
           })
      .def("get_coefficient",
           [](const std::shared_ptr<const ddsolve::expression::Product>& prod) {
             return prod->coefficent.get_str();
           });

  nanobind::enum_<ddsolve::expression::FinityType>(m, "FinityType")
      .value("kFinite", ddsolve::expression::FinityType::kFinite)
      .value("kNegativeInfinite",
             ddsolve::expression::FinityType::kNegativeInfinite)
      .value("kPositiveInfinite",
             ddsolve::expression::FinityType::kPositiveInfinite);

  nanobind::class_<ddsolve::expression::LinearTerm>(m, "LinearTerm")
      .def(nanobind::init<std::vector<ddsolve::expression::Product>>())
      .def(nanobind::init<ddsolve::expression::FinityType>())
      .def(nanobind::init<ddsolve::expression::LinearTerm>())
      .def("__repr__", &ddsolve::expression::LinearTerm::str)
      .def(
          "__iter__",
          [](const ddsolve::expression::LinearTerm& term) {
            return nanobind::make_iterator(
                nanobind::type<ddsolve::expression::LinearTerm>(),
                "ProductIterator", term.begin(), term.end());
          },
          nanobind::keep_alive<0, 1>())
      .def("scale",
           [](const ddsolve::expression::LinearTerm& term,
              const std::string& scalar) {
             return ddsolve::expression::LinearTerm::scalarFromString(scalar) *
                    term;
           })
      .def("substitute", &ddsolve::expression::LinearTerm::substitute)
      .def("hash", &ddsolve::expression::LinearTerm::hash)
      .def("is_finite", &ddsolve::expression::LinearTerm::isFinite)
      .def("get_finity", &ddsolve::expression::LinearTerm::getFinity)
      .def(nanobind::self < nanobind::self)
      .def(nanobind::self == nanobind::self)
      .def(nanobind::self + nanobind::self)
      .def(nanobind::self - nanobind::self);

  nanobind::enum_<ddsolve::expression::ComparisonType>(m, "ComparisonType")
      .value("kEquality", ddsolve::expression::ComparisonType::kEquality)
      .value("kInequality", ddsolve::expression::ComparisonType::kInequality)
      .value("kLessThan", ddsolve::expression::ComparisonType::kLessThan)
      .value("kLessThanOrEqual",
             ddsolve::expression::ComparisonType::kLessThanOrEqual)
      .value("kGreaterThan", ddsolve::expression::ComparisonType::kGreaterThan)
      .value("kGreaterThanOrEqual",
             ddsolve::expression::ComparisonType::kGreaterThanOrEqual);

  using ddsolve::expression::LinearComparison;
  using ddsolve::expression::LinearTerm;
  using LinearVariableOrder = ddsolve::VariableOrder<LinearTerm>;
  using LinearXBDD = ddsolve::XBDD<LinearTerm>;
  using LinearXADD = ddsolve::XADD<LinearTerm>;

  nanobind::class_<LinearComparison> pyLinearComparison{m, "LinearComparison"};
  pyLinearComparison
      /*
        .def_static("decide_real",
                    &LinearComparison::decideReal)
        .def_static("decide_uint",
                    &LinearComparison::decideUInt)
                    */
      .def(nanobind::init<ddsolve::expression::LinearTerm,
                          ddsolve::expression::ComparisonType>())
      .def(nanobind::init<ddsolve::expression::LinearTerm,
                          ddsolve::expression::LinearTerm,
                          ddsolve::expression::ComparisonType>())
      .def("__repr__", &LinearComparison::str)
      .def("substitute", &LinearComparison::substitute)
      .def("get_term", &LinearComparison::getTerm)
      .def("get_type", &LinearComparison::getType)
      .def(nanobind::self < nanobind::self)
      .def(nanobind::self == nanobind::self);

  nanobind::class_<LinearComparison::WithParity>(pyLinearComparison,
                                                 "WithParity")
      .def(nanobind::init<
           bool, ddsolve::expression::LinearTerm::ConstComparisonPointer>())
      .def_ro("parity", &LinearComparison::WithParity::parity)
      .def_ro("comparison", &LinearComparison::WithParity::comparison);

  nanobind::class_<LinearVariableOrder>(m, "VariableOrder")
      .def(nanobind::init<>())
      .def("get_variables", &LinearVariableOrder::getVariables)
      .def("get_comparisons", &LinearVariableOrder::getComparisons)
      .def("get_variable", &LinearVariableOrder::getVariable)
      .def("get_comparison", &LinearVariableOrder::getComparison);

  nanobind::class_<LinearXBDD>(m, "XBDD")
      .def(nanobind::init<>())
      .def(nanobind::init<
           ddsolve::VariableOrder<LinearTerm>::VariableOrderPointer>())
      .def("to_dot", &LinearXBDD::toDot)
      .def("get_true", &LinearXBDD::getTrue)
      .def("get_false", &LinearXBDD::getFalse)
      .def("obtain_repr", &LinearXBDD::obtainRepresentation)
      .def("get_then", &LinearXBDD::getThen)
      .def("get_else", &LinearXBDD::getElse)
      .def("get_comparison", &LinearXBDD::getTopVar)
      .def("is_leaf", &LinearXBDD::isLeaf)
      .def("is_true", &LinearXBDD::isTrue)
      .def("is_false", &LinearXBDD::isFalse)
      .def("with_order", &LinearXBDD::withOrder)
      .def("get_order", &LinearXBDD::getOrder)
      .def("substitute", &LinearXBDD::substitute)
      .def("get_support", static_cast<LinearXBDD (LinearXBDD::*)() const>(
                              &LinearXBDD::getSupport))
      .def("ite",
           static_cast<LinearXBDD (LinearXBDD::*)(
               const LinearXBDD&, const LinearXBDD&) const>(&LinearXBDD::ite))
      .def("ite",
           static_cast<LinearXADD (LinearXBDD::*)(
               const LinearXADD&, const LinearXADD&) const>(&LinearXBDD::ite))
      .def(nanobind::self == nanobind::self)
      .def(nanobind::self & nanobind::self)
      .def(nanobind::self | nanobind::self)
      .def(~nanobind::self);

  nanobind::class_<LinearXADD>(m, "XADD")
      .def(nanobind::init<>())
      .def(nanobind::init<
           ddsolve::VariableOrder<LinearTerm>::VariableOrderPointer>())
      .def("to_dot", &LinearXADD::toDot)
      .def("get_undefined", &LinearXADD::getUndefined)
      .def(
          "get_infty",
          [](const LinearXADD& dd) {
            return dd.getTerm(std::make_shared<ddsolve::expression::LinearTerm>(
                ddsolve::expression::FinityType::kPositiveInfinite));
          })
      .def(
          "get_neg_infty",
          [](const LinearXADD& dd) {
            return dd.getTerm(std::make_shared<ddsolve::expression::LinearTerm>(
                ddsolve::expression::FinityType::kPositiveInfinite));
          })
      .def("get_zero",
           [](const LinearXADD& dd) {
             return dd.getTerm(
                 std::make_shared<ddsolve::expression::LinearTerm>());
           })
      .def(
          "get_one",
          [](const LinearXADD& dd) {
            return dd.getTerm(std::make_shared<ddsolve::expression::LinearTerm>(
                std::vector<ddsolve::expression::Product>{
                    ddsolve::expression::Product{0, 1}}));
          })
      .def("get_term",
           static_cast<LinearXADD (LinearXADD::*)(
               const ddsolve::expression::LinearTerm::ConstTermPointer&) const>(
               &LinearXADD::getTerm))
      .def("get_term",
           static_cast<ddsolve::expression::LinearTerm::ConstTermPointer (
               LinearXADD::*)() const>(&LinearXADD::getTerm))
      .def("get_then", &LinearXADD::getThen)
      .def("get_else", &LinearXADD::getElse)
      .def("get_comparison", &LinearXADD::getTopVar)
      .def("is_leaf", &LinearXADD::isLeaf)
      .def("is_true", &LinearXADD::isTrue)
      .def("is_false", &LinearXADD::isFalse)
      .def("get_nodecount", &LinearXADD::getNodeCount)
      .def("get_case_count", &caseCountToPython<LinearXADD>)
      .def("with_order", &LinearXADD::withOrder)
      .def("get_order", &LinearXADD::getOrder)
      .def("substitute", &LinearXADD::substitute,
           nanobind::call_guard<nanobind::gil_scoped_release>{})
      .def("prune", &LinearXADD::prune,
           nanobind::call_guard<nanobind::gil_scoped_release>{})
      .def("prune_full", &LinearXADD::pruneFull,
           nanobind::call_guard<nanobind::gil_scoped_release>{})
      .def("max", &LinearXADD::max,
           nanobind::call_guard<nanobind::gil_scoped_release>{})
      .def("min", &LinearXADD::min,
           nanobind::call_guard<nanobind::gil_scoped_release>{})
      .def("get_support", &LinearXADD::getSupport)
      .def("get_prunes", &LinearXADD::getPrunes,
           nanobind::call_guard<nanobind::gil_scoped_release>{})
      .def("undefined_to_zero", &LinearXADD::undefinedToZero)
      .def(
          "scale",
          [](const LinearXADD& add, const std::string& scalar) {
            const auto scalar_ptr{
                std::make_shared<ddsolve::expression::LinearTerm::ScalarType>(
                    ddsolve::expression::LinearTerm::scalarFromString(scalar))};
            return scalar_ptr * add;
          })
      .def(LinearXBDD{} * nanobind::self,
           nanobind::call_guard<nanobind::gil_scoped_release>{})
      .def(nanobind::self == nanobind::self)
      .def(nanobind::self <= nanobind::self,
           nanobind::call_guard<nanobind::gil_scoped_release>{})
      .def(nanobind::self < nanobind::self,
           nanobind::call_guard<nanobind::gil_scoped_release>{})
      .def(nanobind::self + nanobind::self,
           nanobind::call_guard<nanobind::gil_scoped_release>{})
      .def(nanobind::self - nanobind::self,
           nanobind::call_guard<nanobind::gil_scoped_release>{});

  ////////////////////////////////////////////////////////////////////////
  using ddsolve::expression::SmtComparison;
  using ddsolve::expression::SmtTerm;
  using SmtVariableOrder = ddsolve::VariableOrder<SmtTerm>;
  using SmtXBDD = ddsolve::XBDD<SmtTerm>;
  using SmtXADD = ddsolve::XADD<SmtTerm>;

  nanobind::class_<SmtTerm>{m, "SmtTerm"}
      .def(nanobind::init<>())
      .def("__repr__", &ddsolve::expression::SmtTerm::str)
      .def_static("mk_int_var", &ddsolve::expression::SmtTerm::mkIntVar)
      .def_static("mk_real_var", &ddsolve::expression::SmtTerm::mkRealVar)
      .def_static("mk_real", &ddsolve::expression::SmtTerm::mkReal)
      .def_static("mk_int", &ddsolve::expression::SmtTerm::mkInt)
      .def_static("mk_int_func", &ddsolve::expression::SmtTerm::mkIntFunction)
      .def_static("mk_real_func", &ddsolve::expression::SmtTerm::mkRealFunction)
      .def_static("mk_int_array", &ddsolve::expression::SmtTerm::mkIntArray)
      .def_static("mk_real_array", &ddsolve::expression::SmtTerm::mkRealArray)
      .def("array_store", &ddsolve::expression::SmtTerm::arrayStore)
      .def("array_select", &ddsolve::expression::SmtTerm::arraySelect)
      .def("apply_func", &ddsolve::expression::SmtTerm::applyFunction)
      .def("to_real", &ddsolve::expression::SmtTerm::toReal)
      .def("to_int", &ddsolve::expression::SmtTerm::toInt)
      .def("substitute", &ddsolve::expression::SmtTerm::substitute)
      .def("hash", &ddsolve::expression::SmtTerm::hash)
      .def(nanobind::self + nanobind::self)
      .def(nanobind::self - nanobind::self)
      .def(nanobind::self * nanobind::self)
      .def(nanobind::self / nanobind::self)
      .def(nanobind::self == nanobind::self);

  nanobind::class_<SmtComparison> pySmtComparison{m, "SmtComparison"};
  pySmtComparison
      .def(nanobind::init<ddsolve::expression::SmtTerm,
                          ddsolve::expression::ComparisonType>())
      .def(nanobind::init<ddsolve::expression::SmtTerm,
                          ddsolve::expression::SmtTerm,
                          ddsolve::expression::ComparisonType>())
      .def_static("push_assumption", &SmtComparison::pushAssumption)
      .def_static("pop_assumption", &SmtComparison::popAssumption)
      .def_static("mk_forall_ints", &SmtComparison::mkForallInt)
      .def_static("mk_forall_reals", &SmtComparison::mkForallReal)
      .def("__repr__", &SmtComparison::str)
      .def("substitute", &SmtComparison::substitute)
      .def(nanobind::self < nanobind::self)
      .def(nanobind::self == nanobind::self)
      .def(nanobind::self & nanobind::self)
      .def(nanobind::self | nanobind::self)
      .def(~nanobind::self);

  nanobind::class_<SmtComparison::WithParity>(pySmtComparison, "WithParity")
      .def(nanobind::init<
           bool, ddsolve::expression::SmtTerm::ConstComparisonPointer>())
      .def_ro("parity", &SmtComparison::WithParity::parity)
      .def_ro("comparison", &SmtComparison::WithParity::comparison);

  nanobind::class_<SmtVariableOrder>(m, "SmtVariableOrder")
      .def(nanobind::init<>())
      .def("get_variables", &SmtVariableOrder::getVariables)
      .def("get_comparisons", &SmtVariableOrder::getComparisons)
      .def("get_variable", &SmtVariableOrder::getVariable)
      .def("get_comparison", &SmtVariableOrder::getComparison);

  nanobind::class_<SmtXBDD>(m, "SmtXBDD")
      .def(nanobind::init<>())
      .def(nanobind::init<
           ddsolve::VariableOrder<SmtTerm>::VariableOrderPointer>())
      .def("to_dot", &SmtXBDD::toDot)
      .def("get_true", &SmtXBDD::getTrue)
      .def("get_false", &SmtXBDD::getFalse)
      .def("obtain_repr", &SmtXBDD::obtainRepresentation)
      .def("get_then", &SmtXBDD::getThen)
      .def("get_else", &SmtXBDD::getElse)
      .def("get_comparison", &SmtXBDD::getTopVar)
      .def("is_leaf", &SmtXBDD::isLeaf)
      .def("is_true", &SmtXBDD::isTrue)
      .def("is_false", &SmtXBDD::isFalse)
      .def("with_order", &SmtXBDD::withOrder)
      .def("get_order", &SmtXBDD::getOrder)
      .def("substitute", &SmtXBDD::substitute)
      .def("get_support",
           static_cast<SmtXBDD (SmtXBDD::*)() const>(&SmtXBDD::getSupport))
      .def("ite", static_cast<SmtXBDD (SmtXBDD::*)(
                      const SmtXBDD&, const SmtXBDD&) const>(&SmtXBDD::ite))
      .def("ite", static_cast<SmtXADD (SmtXBDD::*)(
                      const SmtXADD&, const SmtXADD&) const>(&SmtXBDD::ite))
      .def(nanobind::self == nanobind::self)
      .def(nanobind::self & nanobind::self)
      .def(nanobind::self | nanobind::self)
      .def(~nanobind::self);

  nanobind::class_<SmtXADD>(m, "SmtXADD")
      .def(nanobind::init<>())
      .def(nanobind::init<
           ddsolve::VariableOrder<SmtTerm>::VariableOrderPointer>())
      .def("to_dot", &SmtXADD::toDot)
      .def("get_undefined", &SmtXADD::getUndefined)
      .def("get_term",
           static_cast<SmtXADD (SmtXADD::*)(
               const ddsolve::expression::SmtTerm::ConstTermPointer&) const>(
               &SmtXADD::getTerm))
      .def("get_term",
           static_cast<ddsolve::expression::SmtTerm::ConstTermPointer (
               SmtXADD::*)() const>(&SmtXADD::getTerm))
      .def("get_then", &SmtXADD::getThen)
      .def("get_else", &SmtXADD::getElse)
      .def("get_comparison", &SmtXADD::getTopVar)
      .def("is_leaf", &SmtXADD::isLeaf)
      .def("is_true", &SmtXADD::isTrue)
      .def("is_false", &SmtXADD::isFalse)
      .def("get_nodecount", &SmtXADD::getNodeCount)
      .def("get_case_count", &caseCountToPython<SmtXADD>)
      .def("with_order", &SmtXADD::withOrder)
      .def("get_order", &SmtXADD::getOrder)
      .def("substitute", &SmtXADD::substitute,
           nanobind::call_guard<nanobind::gil_scoped_release>{})
      .def("prune", &SmtXADD::prune,
           nanobind::call_guard<nanobind::gil_scoped_release>{})
      .def("prune_full", &SmtXADD::pruneFull,
           nanobind::call_guard<nanobind::gil_scoped_release>{})
      .def("max", &SmtXADD::max,
           nanobind::call_guard<nanobind::gil_scoped_release>{})
      .def("min", &SmtXADD::min,
           nanobind::call_guard<nanobind::gil_scoped_release>{})
      .def("get_support", &SmtXADD::getSupport)
      .def("get_prunes", &SmtXADD::getPrunes,
           nanobind::call_guard<nanobind::gil_scoped_release>{})
      .def("undefined_to_zero", &SmtXADD::undefinedToZero)
      .def("scale",
           [](const SmtXADD& add, const std::string& scalar) {
             const auto scalar_ptr{
                 std::make_shared<ddsolve::expression::SmtTerm::ScalarType>(
                     ddsolve::expression::SmtTerm::scalarFromString(scalar))};
             return scalar_ptr * add;
           })
      //.def(nanobind::self / nanobind::self,
      //     nanobind::call_guard<nanobind::gil_scoped_release>{})
      .def(nanobind::self * nanobind::self,
           nanobind::call_guard<nanobind::gil_scoped_release>{})
      .def(SmtXBDD{} * nanobind::self,
           nanobind::call_guard<nanobind::gil_scoped_release>{})
      .def(nanobind::self == nanobind::self)
      .def(nanobind::self <= nanobind::self,
           nanobind::call_guard<nanobind::gil_scoped_release>{})
      .def(nanobind::self < nanobind::self,
           nanobind::call_guard<nanobind::gil_scoped_release>{})
      .def(nanobind::self + nanobind::self,
           nanobind::call_guard<nanobind::gil_scoped_release>{})
      .def(nanobind::self - nanobind::self,
           nanobind::call_guard<nanobind::gil_scoped_release>{});
}
