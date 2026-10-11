# tcc-media realization

Paths below are relative to the root of this repository.
`formers/FORMERS.md` gives the host-neutral forms and laws. The front end in
`src/front/` comes from the lang-template host kits (`docs/host/README.md`,
section Origin). The media layer (`src/media.c`) adds the core types and
core operations of `SPEC.md` sections 5 and 6. It adds no type former.

Each status covers `langc check`, `langc eval` and `langc build PROG`.
`make check` runs `test/gate.sh`, which checks every example, every eval line
in `test/eval/expect.txt` and every JSON golden in `test/json/`. It also runs
`test/regressions.js` and `test/printer-regressions.js` (`Makefile:67-73`).

The last column names the `probe/CAPABILITY.md` section that sets the
status. Sections 2 (several files) and 6 (cost) apply to each former.

| ID | Former | Status and evidence | Section |
|---|---|---|---|
| F1 | Product | DONE: `pair`, `first`, `second`; `examples/formers.lang`, `examples/functions.lang` | 1, 5 |
| F2 | Coproduct | DONE: `inl`, `inr`; `examples/formers.lang`, `examples/laws.lang` | 1, 5 |
| F3 | Option | DONE: `none`, `some`; `examples/formers.lang`, `examples/monad.lang`. `intersect` gives an `Option Interval` | 1, 5 |
| F4 | List | DONE: `nil`, `cons`; `examples/formers.lang`, `examples/inventory.lang`. `SubtitleTrack` is a `List` (`domain/video.lang:12`) | 5, 7 |
| F5 | Monad pure/map/bind | DONE over Option, Sum E and List: `examples/monad.lang`, `examples/laws.lang` | 1, 5 |
| F6 | Algebra fold | DONE over Nat, List and each family: `examples/algebra.lang`, `examples/entries.lang` | 3, 4 |
| F7 | Algebra unfold | DONE into List with a Nat step limit: `examples/algebra.lang` | 3, 4 |
| F8 | Filter | DONE over Option and List, with a `Flag` test: `examples/monad.lang`, `domain/domain.lang` | 1, 5 |
| F9 | Non-dependent Pi | DONE: `examples/functions.lang`; `Filter` and `compose` (`domain/video.lang:8-11`) | 1 |
| F10 | Dependent Pi | DONE: `examples/functions.lang`, `examples/universes.lang` | 1 |
| F11 | Sigma | DONE: `pack`, `witness`, `payload`; `examples/sigma.lang` | 8 |
| F12 | Eq refl/symm/trans | DONE: `examples/equality.lang`, `examples/laws.lang` | 7, 9 |
| F13 | Eq transport/cong | DONE: `examples/equality.lang` | 9 |
| F14 | Universes | DONE: `Type 0` and `Type 1`, without cumulativity; `examples/universes.lang`, `test/check/cumulative.lang`, `test/check/type2.lang` | 1, 5 |
| F15 | Generic indexed family | DONE in `domain/` only (rule R1): `Lot` in `domain/domain.lang:8`, `examples/inventory.lang` | 7 |

JSON output of each former: `docs/host/README.md`, section The JSON
document. Functions, types, families and equality proofs are not instances.
Thus F9, F10, F14 and the F12 and F13 proofs have no JSON value of their
own. A Sigma payload of `Eq` type is `null`. A `Video` value comes only from
the input of a media entry, so it is not an instance. `langc build PROG MAIN
IN... -o OUT` writes the media file of the entry (`SPEC.md` section 7).
