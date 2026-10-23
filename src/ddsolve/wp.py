"""Weakest precondition verifier for PGCL probabilistic programs."""

from lark import Tree, Transformer, v_args
from lark.visitors import Interpreter
from tqdm.auto import tqdm
from itertools import count

import ddsolve._ddsolve as ddsolve


class Verifier(Interpreter):
    """Weakest precondition verifier for PGCL probabilistic programs.

    Uses backward reasoning to compute post-expectations and verify
    probabilistic programs using wp, wlp, or conditional wp calculi.

    Attributes:
        zero: The zero XADD value.
        one: The one XADD value.
        infty: The infinity XADD value.
        verified: The verification result ("True", "False", or "Unknown").
        post_expectation: The current post-expectation during verification.
        calculus: The active calculus mode (e.g., 'wp', 'wlp', 'cwp').
        liberal: Whether this is a liberal calculus (ends with 'lp').
        angelic: Whether this is an angelic calculus (starts with 'a').
        conditional: Whether this is a conditional calculus (starts with 'c').
        runtime: Whether this is a runtime calculus (ends with 'ert').
        ticktime: Whether this is a tick-time calculus (ends with 'ett').
    """

    def __init__(self, order):
        """Initialize the verifier with zero, one, and infinity values.

        Args:
            order: The variable order for decision diagram construction.
        """
        xadd = ddsolve.XADD(order)
        self.zero = xadd.get_zero()
        self.one = xadd.get_one()
        self.infty = xadd.get_infty()
        self.verified = "Unknown"
        self.iterations = 0
        self.peak_nodecount = 0
        self.peak_case_count = 0

    def _prune(self, expectation):
        """Prune an expectation and record the largest size seen before pruning.

        Args:
            expectation: The XADD to prune.

        Returns:
            The pruned XADD.
        """
        nodecount = expectation.get_nodecount()
        if nodecount > self.peak_nodecount:
            self.peak_nodecount = nodecount
            self.peak_case_count = expectation.get_case_count()
        return expectation.prune_full()

    def _expectation(self, exp):
        """Extract the expectation XADD from an expression or expectation_pair.

        Args:
            exp: Either an XADD or a Lark tree node for an expectation_pair.

        Returns:
            The XADD representing the expectation.
        """
        if isinstance(exp, ddsolve.XADD):
            return exp
        else:
            assert exp.data == "expectation_pair"
            return exp.children[1 if self.liberal else 0]

    def verify(self, prog):
        """Verify the program by visiting its tree.

        Args:
            prog: The transformed parse tree of the program.

        Returns:
            The verification result string ("True", "False", or "Unknown").
        """
        self.visit(prog)
        return self.verified

    def _set_calculus(self, calculus):
        """Configure the verification calculus mode.

        Sets flags based on the calculus name:
            - liberal: ends with 'lp'
            - angelic: starts with 'a' (unless runtime/ticktime)
            - conditional: starts with 'c'
            - runtime: ends with 'ert'
            - ticktime: ends with 'ett'

        Args:
            calculus: The calculus annotation string (e.g., 'wp', 'wlp', 'cwp').
        """
        self.calculus = calculus
        self.liberal = calculus.endswith("lp")
        self.angelic = calculus.startswith("a")
        self.conditional = calculus.startswith("c")
        self.runtime = calculus.endswith("ert")
        self.ticktime = calculus.endswith("ett")
        if self.runtime or self.ticktime:
            self.angelic = not self.angelic

    def _conditional(self, tree):
        """Verify using conditional wp (cwp) by computing both wp and wlp.

        Runs wp and wlp verifications separately, then combines results
        to check the conditional precondition.

        Args:
            tree: The Lark parse tree for the program.

        Raises:
            ValueError: If nondeterministic choice is used in cwp mode.
        """
        if list(tree.find_data("nd_choice")):
            raise ValueError("Nondeterministic choice in cwp not supported.")
        wp = Verifier(self.zero.get_order())
        wlp = Verifier(self.zero.get_order())

        for i in tree.find_data("calculus_annotation"):
            i.children[0] = i.children[0].replace("cwp", "wp")
        wp.verify(tree)

        for i in tree.find_data("calculus_annotation"):
            i.children[0] = i.children[0].replace("wp", "wlp")
        wlp.verify(tree)

        for i in tree.find_data("calculus_annotation"):
            i.children[0] = i.children[0].replace("wlp", "cwp")

        self.post_expectation = (wp.post_expectation, wlp.post_expectation)
        pre_expectation_bound = tree.children[0].children[2].children
        comperator = tree.children[0].children[1]

        with ddsolve.PrintModelsManager() as m:
            match comperator:
                case "<":
                    self.verified = (
                        self.post_expectation[0] < pre_expectation_bound[0]
                        and self.post_expectation[1] > pre_expectation_bound[1]
                    )
                case "<=":
                    self.verified = (
                        self.post_expectation[0] <= pre_expectation_bound[0]
                        and self.post_expectation[1] >= pre_expectation_bound[1]
                    )
                case ">":
                    self.verified = (
                        self.post_expectation[0] > pre_expectation_bound[0]
                        and self.post_expectation[1] < pre_expectation_bound[1]
                    )
                case ">=":
                    self.verified = (
                        self.post_expectation[0] >= pre_expectation_bound[0]
                        and self.post_expectation[1] <= pre_expectation_bound[1]
                    )
                case "==":
                    self.verified = (
                        (not (self.post_expectation[0] < pre_expectation_bound[0]))
                        and (not (self.post_expectation[0] > pre_expectation_bound[0]))
                        and (not (self.post_expectation[1] < pre_expectation_bound[1]))
                        and (not (self.post_expectation[1] > pre_expectation_bound[1]))
                    )
                case "!=":
                    self.verified = (
                        (self.post_expectation[0] < pre_expectation_bound[0])
                        or (self.post_expectation[0] > pre_expectation_bound[0])
                    ) and (
                        (self.post_expectation[1] < pre_expectation_bound[1])
                        or (self.post_expectation[1] > pre_expectation_bound[1])
                    )

    def start(self, tree):
        """Execute the main verification logic.

        Sets up the calculus, visits the program statements, and
        compares the computed post-expectation against the pre-bound.

        Args:
            tree: The Lark parse tree for the program.
        """
        self._set_calculus(tree.children[0].children[0])

        if self.conditional:
            self._conditional(tree)
            return

        comperator = tree.children[0].children[1]
        pre_expectation_bound = self._expectation(tree.children[0].children[2])
        statements = tree.children[1]
        self.post_expectation = self._expectation(tree.children[-1].children[0])

        with ddsolve.LaceManager() as m:
            self.visit(statements)

            self.post_expectation = self._prune(self.post_expectation)

            with ddsolve.PrintModelsManager() as m:
                match comperator:
                    case "<":
                        self.verified = self.post_expectation < pre_expectation_bound
                    case "<=":
                        self.verified = self.post_expectation <= pre_expectation_bound
                    case ">":
                        self.verified = self.post_expectation > pre_expectation_bound
                    case ">=":
                        self.verified = self.post_expectation >= pre_expectation_bound
                    case "==":
                        self.verified = (
                            self.post_expectation <= pre_expectation_bound
                        ) and (self.post_expectation >= pre_expectation_bound)
                    case "!=":
                        self.verified = (
                            not self.post_expectation <= pre_expectation_bound
                        ) or (not self.post_expectation >= pre_expectation_bound)

        # if self.conditional:
        #    self.post_expectation, self.verified = self._conditional(tree)
        #    return

        # self.pre_expectation_bound = self._expectation(tree.children[1].children[0])
        # statements = tree.children[2]
        # self.post_expectation = self._expectation(tree.children[-1].children[0])

        # with ddsolve.LaceManager() as m:
        #    self.visit(statements)
        #    self.verified = self._compare_bound(self.pre_expectation_bound)

    def statements(self, tree):
        """Visit all statements in reverse order (backward reasoning).

        Args:
            tree: The Lark tree node containing statement children.
        """
        for i in reversed(tree.children):
            self.visit(i)

    def diverge(self, tree):
        """Handle divergent programs.

        Sets the post-expectation based on the calculus mode:
            - liberal: one (1)
            - runtime: infinity
            - otherwise: zero (0)

        Args:
            tree: The Lark tree node for the diverge statement.
        """
        if self.liberal:
            self.post_expectation = self.one
        elif self.runtime:
            self.post_expectation = self.one.get_infty()
        else:
            self.post_expectation = self.zero

    def prob_choice(self, tree):
        """Handle probabilistic choice (bernoulli distribution).

        Computes the weighted expectation of the two branches.

        Args:
            tree: The Lark tree node ([if_body, probability, else_body]).
        """
        assert len(tree.children) == 3
        p = tree.children[1]
        q = (p[1] - p[0], p[1])
        p = str(p[0]) + "/" + str(p[1])
        q = str(q[0]) + "/" + str(q[1])
        post_expectation = self.post_expectation
        self.visit(tree.children[0])
        left_branch = self.post_expectation
        self.post_expectation = post_expectation
        self.visit(tree.children[2])
        right_branch = self.post_expectation
        self.post_expectation = left_branch.scale(p) + right_branch.scale(q)
        if self.runtime:
            self.post_expectation = self.post_expectation + self.one

    def nd_choice(self, tree):
        """Handle nondeterministic choice.

        Takes the maximum (angelic) or minimum (demonic) of the two branches.

        Args:
            tree: The Lark tree node ([left_branch, right_branch]).
        """
        post_expectation = self.post_expectation
        self.visit(tree.children[0])
        left_branch = self.post_expectation
        self.post_expectation = post_expectation
        self.visit(tree.children[1])
        right_branch = self.post_expectation

        if self.angelic:
            self.post_expectation = left_branch.max(right_branch)
        else:
            self.post_expectation = left_branch.min(right_branch)
        if self.runtime:
            self.post_expectation = self.post_expectation + self.one

    def if_(self, tree):
        """Handle if-then-else constructs.

        Computes the expectation as the guarded if-then-else of the
        two branches.

        Args:
            tree: The Lark tree node ([guard, if_body]) or
                ([guard, if_body, else_body]).
        """
        c = tree.children
        if len(c) == 2:
            else_branch = self.post_expectation
            self.visit(c[1])
            self.post_expectation = c[0].ite(self.post_expectation, else_branch)
        if len(c) == 3:
            tmp = self.post_expectation
            self.visit(c[1])
            then_branch = self.post_expectation
            self.post_expectation = tmp
            self.visit(c[2])
            self.post_expectation = c[0].ite(then_branch, self.post_expectation)
        if self.runtime:
            self.post_expectation = self.post_expectation + self.one

    def assign(self, tree):
        """Handle variable assignment by substituting into post-expectation.

        Args:
            tree: The Lark tree node ([variable, expression]).
        """
        var = tree.children[0]
        term = tree.children[1]
        self.post_expectation = self.post_expectation.substitute(var, term)
        if self.runtime:
            self.post_expectation = self.post_expectation + self.one

    def sample(self, tree):
        """Handle probabilistic sampling (random assignment).

        Computes the expectation as the weighted sum over all
        distribution outcomes.

        Args:
            tree: The Lark tree node ([variable, distribution]).
        """
        var = tree.children[0]
        tmp = self.zero
        self.post_expectation = self._prune(self.post_expectation)
        for prob, value in tree.children[1]:
            tmp = tmp + self.post_expectation.substitute(var, value).scale(
                f"{prob[0]}/{prob[1]}"
            )
            tmp = self._prune(tmp)
        self.post_expectation = tmp
        if self.runtime:
            self.post_expectation = self.post_expectation + self.one

    def observe(self, tree):
        """Handle observe (sampling) by multiplying with the guard.

        Sets undefined values to zero after multiplication.

        Args:
            tree: The Lark tree node containing the observation guard.
        """
        self.post_expectation = (
            tree.children[0] * self.post_expectation
        ).undefined_to_zero()

    def tick(self, tree):
        """Handle tick (time cost) by incrementing the post-expectation.

        Only active when ticktime calculus is enabled.

        Args:
            tree: The Lark tree node containing the tick value.
        """
        if self.ticktime:
            self.post_expectation = self.post_expectation + self.one

    def _fixpoint(self, n, loop_guard, loop_body):
        """Verify a while loop by fixpoint iteration.

        Iteratively unrolls the loop body until the expectation stabilizes
        or the iteration bound is reached.

        Args:
            n: The iteration bound (None for unbounded).
            loop_guard: The loop guard expression.
            loop_body: The loop body statement.
        """
        generator = range(n) if isinstance(n,int) else count()
        generator = tqdm(generator, desc=f"{self.calculus} fixpoint iteration")
        initial_expectation = self.post_expectation

        last_expectation = None
        self.post_expectation = self.one if self.liberal else self.zero
        for i in generator:
            self.iterations += 1
            last_expectation = self.post_expectation
            self.visit(loop_body)
            self.post_expectation = loop_guard.ite(
                self.post_expectation, initial_expectation
            )
            self.post_expectation = self._prune(self.post_expectation)
            if self.runtime:
                self.post_expectation = self.post_expectation + self.one
            if self.post_expectation == last_expectation:
                break

    def _kinduction(self, n, m, invariant, loop_guard, loop_body):
        """Verify a while loop using k-induction.

        Iteratively unrolls the loop body (up to m times per iteration),
        checking if the invariant is preserved. Falls back to a single
        unroll if the bound is exceeded.

        Args:
            n: The iteration bound (None for unbounded).
            m: The unroll factor (0 = no extremum, 1 = normal, >1 = unrolled).
            invariant: The loop invariant expectation.
            loop_guard: The loop guard expression.
            loop_body: The loop body statement.
        """
        generator = range(n) if isinstance(n,int) else count()
        if m == 1:
            desc=f"{self.calculus} kinduction"
        else:
            desc=f"{self.calculus} unrolled kinduction"
        generator = tqdm(generator, desc=desc)
        initial_expectation = self.post_expectation

        # handle the case that we use unrolled kinduction
        # without taking the extemum
        take_extremum = True
        if m == 0:
            m = 1
            take_extremum = False

        self.post_expectation = invariant
        for i in generator:
            self.iterations += 1
            # unrolled kinduction
            for _ in range(m):
                self.visit(loop_body)
                self.post_expectation = loop_guard.ite(
                    self.post_expectation, initial_expectation
                )
                if self.runtime:
                    self.post_expectation = self.post_expectation + self.one
                self.post_expectation = self._prune(self.post_expectation)

            if self.liberal:
                if invariant <= self.post_expectation:
                    return
                if take_extremum:
                    self.post_expectation = self.post_expectation.max(invariant)
            else:
                if self.post_expectation <= invariant:
                    return
                if take_extremum:
                    self.post_expectation = self.post_expectation.min(invariant)

            self.post_expectation = self._prune(self.post_expectation)

        print("Warning: Could not prove the given invariant with kinduction!")
        self.visit(loop_body)
        self.post_expectation = loop_guard.ite(
            self.post_expectation, initial_expectation
        )
        if self.runtime:
            self.post_expectation = self.post_expectation + self.one
        self.post_expectation = self._prune(self.post_expectation)
        #self.post_expectation = self.zero if self.liberal else self.infty

    def while_(self, tree):
        """Handle while loop verification based on loop annotation type.

        Dispatches to _fixpoint, _kinduction, or other strategies
        based on the loop annotation.

        Args:
            tree: The Lark tree node ([loop_annotation, loop_guard, loop_body]).
        """
        [loop_annotation, loop_guard, loop_body] = tree.children
        match tree.children[0].data:
            case "loop_annotation_fixpoint":
                self._fixpoint(None, loop_guard, loop_body)
            case "loop_annotation_unroll":
                self._fixpoint(int(loop_annotation.children[0]), loop_guard, loop_body)
            case "loop_annotation_kinduction":
                [invariant, n, m] = loop_annotation.children
                invariant = self._expectation(invariant)
                if n:
                    n = int(n)
                if m:
                    m = int(m)
                else:
                    m = 1
                self._kinduction(n, m, invariant, loop_guard, loop_body)
