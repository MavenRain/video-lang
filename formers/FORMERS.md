# Type formers

This file specifies the type formers of video-lang. The specification does
not depend on a host. A program has these formers and no others. The core
types and core operations come from `design/DESIGN.md` (`SPEC.md` sections 5
and 6). They do not add a type former.

The forms below use the surface syntax of `langc`. `formers/tcc-media.md`
gives the host form of each former on the tcc-media host, its status and its
evidence. Section 5 of this file is the realization matrix.

## 1. Notation

- `A`, `B`, `C` and `E` are data types. `S` is a seed type. `I` is an index
  type. `P` is a type function into `Type 0`.
- `a : A` means that the term `a` has the type `A`.
- `t[x := a]` is the term `t` with `a` in place of each free `x`.
- `=` in a law is equality of closed values. tcc-media compares two closed
  values by their normal forms (`langc eval`). Two media programs are equal
  when their outputs give the same framemd5 lines (`SPEC.md` section 8).
- `Unit` is a type with one value, `unit`.
- A test is a function `p : A -> Option Unit`. The result `some unit` keeps
  the element. tcc-media uses `Flag`, a type with two values, in
  place of `Option Unit`, and `true` keeps (`examples/monad.lang:9`).

## 2. Programs and refusal rules

A program is a sequence of definitions:

```
def NAME : TYPE := TERM
```

A definition can refer only to the definitions before it, to the core types
and operations, and to the forms of this file. The compiler applies these
refusal rules. They keep a program inside the former layer.

- **R1. No new data type.** The compiler refuses `family` in a program
  (`REFUSE_DATA`, `src/front/check.c:1519`). Only `domain/` declares a family.
- **R2. No unproved fact.** The compiler refuses `axiom` (`REFUSE_AXIOM`,
  `src/front/check.c:1521`). The target protocol has no axiom
  (`probe/CAPABILITY.md` section 10).
- **R3. No general recursion.** The compiler refuses the host form for a
  recursive definition (`def rec`). Recursion comes only from F6 `fold` and
  F7 `unfold`. When F7 has the status HOST-LIMIT, recursion comes only from
  F6.
- **R4. No new core name.** The compiler refuses a definition whose name is
  a core type, a core operation, a prelude name or a reserved name.
- **R5. Host forms.** On tcc-media, the one host form that is not a
  former is `family`, and only `domain/` can use it (R1). `SPEC.md`
  section 2 gives the full refusal list.

The compiler writes the target. A program cannot write a part of the target
directly.

## 3. Formers

Each former has a formation rule, introduction forms, elimination forms and
laws. A realization must satisfy each law that this section gives. A law
that a host cannot satisfy makes the status PARTIAL or HOST-LIMIT.

### F1 Product

- Formation: `Prod A B : Type 0` when `A : Type 0` and `B : Type 0`.
- Introduction: `pair a b : Prod A B` when `a : A` and `b : B`.
- Elimination: `first p : A` and `second p : B` when `p : Prod A B`.
- Laws: `first (pair a b) = a`. `second (pair a b) = b`.
  `pair (first p) (second p) = p`.

### F2 Coproduct

- Formation: `Sum A B : Type 0` when `A : Type 0` and `B : Type 0`.
- Introduction: `inl a : Sum A B` when `a : A`. `inr b : Sum A B` when
  `b : B`.
- Elimination: `either f g s : C` when `f : A -> C`, `g : B -> C` and
  `s : Sum A B`.
- Laws: `either f g (inl a) = f a`. `either f g (inr b) = g b`.

### F3 Option

- Formation: `Option A : Type 0` when `A : Type 0`.
- Introduction: `none : Option A`. `some a : Option A` when `a : A`.
- Elimination: `option z f o : C` when `z : C`, `f : A -> C` and
  `o : Option A`. The F5 and F8 forms also consume an option.
- Laws: `option z f none = z`. `option z f (some a) = f a`.
- A host can define `Option A` as `Sum Unit A`. Then `none` is `inl unit`,
  `some a` is `inr a`, and `option z f o = either (fun u => z) f o`.

### F4 List

- Formation: `List A : Type 0` when `A : Type 0`.
- Introduction: `nil : List A`. `cons a l : List A` when `a : A` and
  `l : List A`.
- Elimination: F6 `fold`. The F5 and F8 forms also consume a list.
- Laws: the F6 laws for the carrier `List A`.

### F5 Monad

- Instances: `Option`, `Sum E` and `List`. The declared type of the form
  selects the instance. Write `F` for the instance.
- Forms: `pure a : F A` when `a : A`. `map f t : F B` when `f : A -> B` and
  `t : F A`. `bind f t : F B` when `f : A -> F B` and `t : F A`.
- Laws:
  - Left identity: `bind f (pure a) = f a`.
  - Right identity: `bind pure t = t`.
  - Associativity: `bind g (bind f t) = bind (fun x => bind g (f x)) t`.
  - Map: `map f t = bind (fun x => pure (f x)) t`.
- For `Sum E`, `pure` is `inr`, and `bind f (inl e) = inl e`. For `List`,
  `bind` maps each element to a list and joins the lists in order.

### F6 Algebra fold

- Carriers: `Nat`, `List A`, and each recursive core type that the host
  column lists.
- Form over `Nat`: `fold f z n : C` when `f : C -> C`, `z : C` and
  `n : Nat`. Laws: `fold f z 0 = z`. `fold f z (n + 1) = f (fold f z n)`.
- Form over `List A`: `fold f z l : C` when `f : A -> C -> C`, `z : C` and
  `l : List A`. Laws: `fold f z nil = z`.
  `fold f z (cons a l) = f a (fold f z l)`. Thus `fold` consumes the
  elements from the right.
- Form over a core type with several constructors: `fold` takes one function
  for each constructor that has fields and one value for each constant
  constructor. A function receives the folded results of the recursive
  fields.
- Uniqueness: `fold f z` is the only function `h` with `h nil = z` and
  `h (cons a l) = f a (h l)`. The same holds for each carrier: `fold` is the
  unique algebra map out of the carrier. Each fold terminates, because it
  consumes one constructor at each step.

### F7 Algebra unfold

- Form into `List E`: `unfold g n s : List E` when
  `g : S -> Option (Prod E S)`, `n : Nat` and `s : S`. `n` is the step
  limit.
- Laws: `unfold g 0 s = nil`. When `g s = none`,
  `unfold g (n + 1) s = nil`. When `g s = some (pair e r)`,
  `unfold g (n + 1) s = cons e (unfold g n r)`.
- Form into `Nat`: `g : S -> Option S`, and the result is the number of
  steps.
- Form into a core type with several constructors: `g` gives one leg of a
  sum for each constructor. A seed that is left after `n` steps becomes the
  constant constructor that the host column names.
- The step limit makes each unfold terminate. A host that has no measure for
  the step limit cannot realize F7. Its status is HOST-LIMIT.

### F8 Filterable filter

- Carriers: `Option`, `List`, and each sequence core type that the host
  column lists. `Sum E` is not a carrier.
- Form: `filter p t : F A` when `p : A -> Option Unit` (a test, section 1)
  and `t : F A`.
- Laws: `filter p none = none`. `filter p (some a) = some a` when `p a`
  keeps, and `none` when it does not. `filter p nil = nil`.
  `filter p (cons a l) = cons a (filter p l)` when `p a` keeps, and
  `filter p l` when it does not.
- Thus `filter` keeps exactly the elements whose test keeps them, in their
  order.

### F9 Pi, not dependent

- Formation: `(x : A) -> B : Type 0` when `A` and `B` are data types and `B`
  does not refer to `x`. A type parameter comes first:
  `(T : Type 0) -> (x : T) -> B` is in `Type 1`.
- Introduction: `fun (x : A) => t` when `t : B` with `x : A` in scope.
- Elimination: the application `f a` when `f : (x : A) -> B` and `a : A`.
  An application gives one data type for each type parameter first, then
  the values.
- Laws: `(fun (x : A) => t) a = t[x := a]`. A function is not an instance
  and has no encoding.
- Rule: `B` cannot refer to a value parameter. That form is F10.

### F10 Pi, dependent

- Formation: `(x : A) -> B` when `B` can refer to `x`.
- Introduction: `fun (x : A) => t` when `t : B` with `x : A` in scope.
- Elimination: `f a : B[x := a]` when `f : (x : A) -> B` and `a : A`.
- Laws: `(fun (x : A) => t) a = t[x := a]`.

### F11 Sigma

- Formation: `Sigma A B` when `B` can refer to a value of `A`. In binder
  form: `(x : A) * B`.
- Introduction: `pack a b : Sigma A B` when `a : A` and `b : B[x := a]`.
- Elimination: `witness s : A` and `payload s : B[x := witness s]` when
  `s : Sigma A B`.
- Laws: `witness (pack a b) = a`. `payload (pack a b) = b`.
  `pack (witness s) (payload s) = s`.
- A host that can build a Sigma value but cannot project it has the status
  PARTIAL. A record family that a match reads can replace a projected Sigma.
  The host file names each replacement.

### F12 Equality: refl, symm, trans

- Formation: `Eq A x y` when `A : Type 0`, `x : A` and `y : A`.
- Introduction: `refl x : Eq A x x`.
- Elimination: `symm e : Eq A y x` when `e : Eq A x y`.
  `trans d e : Eq A x z` when `d : Eq A x y` and `e : Eq A y z`.
- Rules: `Eq A x y` has an inhabitant only when `x = y`. A proof has no
  runtime content. It erases from the target, and a definition of an `Eq`
  type is not an instance.

### F13 Equality: transport and cong

- Elimination: `transport P e u : P y` when `e : Eq A x y` and `u : P x`.
  `cong f e : Eq B (f x) (f y)` when `f : A -> B` and `e : Eq A x y`.
- Laws: `transport P (refl x) u = u`. `cong f (refl x) = refl (f x)`.

### F14 Universes

- `Type 0` classifies the data types and the function types of data types.
- `Type 1` classifies `Type 0`, the function types with a type parameter,
  the type functions (for example `Option`), and each core family whose
  index is a type.
- There is no cumulativity. A type in `Type 0` is not a type in `Type 1`.
  There is no `Type 2` in a program.
- An instance is a top-level definition whose type is a data type. A
  definition of a type, of a function or of a proof is not an instance.

### F15 Indexed family

- Formation: `D i : Type 0` when `i : I` and `I` is a core data type. The
  index is a value, not a type, so F15 uses F10.
- Introduction: each constructor fixes the index or takes it as an argument:
  `mkD i x : D i`.
- Elimination: a match on `d : D i` gives the fields. Each field type uses
  the same `i`.
- Laws: a match on `mkD i x` gives `x`.
- Example: `Lot` in `domain/domain.lang:8`, a family over `size : Nat`.

## 4. Status words

| Word | Meaning |
|---|---|
| DONE | The host realizes the former and its laws. Tests or host checks cover it. |
| PARTIAL | The host realizes a restricted form. The cell names the restriction. |
| PLANNED | The host can realize the former, but the build does not have it yet. |
| HOST-LIMIT | The host cannot realize the former. The cell cites the probe fact. |

## 5. Realization matrix

The cell gives the status of each former on the tcc-media host.
`formers/tcc-media.md` gives the evidence for each cell and the
`probe/CAPABILITY.md` section that sets it. When a status changes, update
`formers/tcc-media.md` and this matrix in the same change.

| ID | Former | tcc-media |
|---|---|---|
| F1 | Product | DONE |
| F2 | Coproduct | DONE |
| F3 | Option | DONE |
| F4 | List | DONE |
| F5 | Monad | DONE over `Option`, `Sum E` and `List` |
| F6 | Algebra fold | DONE over `Nat`, `List` and each family |
| F7 | Algebra unfold | DONE into `List`, with a `Nat` step limit |
| F8 | Filterable filter | DONE over `Option` and `List` |
| F9 | Pi, not dependent | DONE |
| F10 | Pi, dependent | DONE |
| F11 | Sigma | DONE |
| F12 | Equality: refl, symm, trans | DONE |
| F13 | Equality: transport and cong | DONE |
| F14 | Universes | DONE: `Type 0` and `Type 1`, without cumulativity |
| F15 | Indexed family | DONE in `domain/` only (rule R1) |
