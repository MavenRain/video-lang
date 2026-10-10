# TinyCC JSON realization

Paths below are relative to this host kit, or to the root of a generated
language. The template's `formers/FORMERS.md` gives the host-neutral laws.
The front end in `src/front/` is a fork of the tcc-wasm front end (`README.md`,
section Origin). Each status covers `langc check`, `langc eval` and
`langc build`. `test/gate.sh` checks every example, every eval line in
`test/eval/expect.txt` and every JSON golden in `test/json/`. `make check`
also runs `test/regressions.js` for type safety, complete type metadata,
binder scope and domain declarations.

| ID | Former | Status and evidence |
|---|---|---|
| F1 | Product | DONE: `pair`, `first`, `second`; `examples/formers.lang`, `examples/functions.lang` |
| F2 | Coproduct | DONE: `inl`, `inr`; `examples/formers.lang`, `examples/laws.lang` |
| F3 | Option | DONE: `none`, `some`; `examples/formers.lang`, `examples/monad.lang` |
| F4 | List | DONE: `nil`, `cons`; `examples/formers.lang`, `examples/inventory.lang` |
| F5 | Monad pure/map/bind | DONE over Option, Sum E and List: `examples/monad.lang`, `examples/laws.lang` |
| F6 | Algebra fold | DONE over Nat, List and each family: `examples/algebra.lang`, `examples/entries.lang` |
| F7 | Algebra unfold | DONE into List with a Nat step limit: `examples/algebra.lang` |
| F8 | Filter | DONE over Option and List: `examples/monad.lang`, `domain/domain.lang` |
| F9 | Non-dependent Pi | DONE: `examples/functions.lang` |
| F10 | Dependent Pi | DONE: `examples/functions.lang`, `examples/universes.lang` |
| F11 | Sigma | DONE: `pack`, `witness`, `payload`; `examples/sigma.lang` |
| F12 | Eq refl/symm/trans | DONE: `examples/equality.lang`, `examples/laws.lang` |
| F13 | Eq transport/cong | DONE: `examples/equality.lang` |
| F14 | Universes | DONE: `Type 0` and `Type 1`, without cumulativity; `examples/universes.lang`, `test/check/cumulative.lang`, `test/check/type2.lang` |
| F15 | Generic indexed family | DONE in `domain/` only (rule R1): `Lot` in `domain/domain.lang`, `examples/inventory.lang` |

JSON output of each former: `README.md`, section The JSON document.
Functions, types, families and equality proofs are not instances. Thus
F9, F10, F14 and the F12 and F13 proofs have no JSON value of their own.
A Sigma payload of `Eq` type is `null`.
