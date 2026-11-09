# Proposed loading-v2 examples

All files are **synthetic design examples, not implemented Surge inputs**.
No customer data, measured vehicle defaults or verified profile records are here.
Current loading-v1 binaries must reject the required v2 declaration.

See [the C/JSON contract](../surge-loading-v2-api.md) and
[JSON Schema](../surge-loading-v2-proposed.schema.json). Request files contain a
complete ordinary native model plus the proposed `loading` object; the schema
root validates that object. `definitions/result` validates the result-loading
files. These are possible witnesses, not deterministic expected HGS placements.

- `capabilities.json` and `invalid-input.error.json`: proposed discovery and
  structured error shapes, not observations of current runtime support.
- `axles`: one 500 kg unit, longitudinal centre uncertainty +/-50 mm, rigid
  two-axle model. Includes empty/load/deliver reaction bounds with exact rational
  numerators stored as strings.
- `reshuffling`: A loaded below B, B delivered later. Stage B, deliver A, restore
  B at layer zero, then deliver B. Two extra moves, 60 extra service seconds and
  five extra native fee units. No axle claim: that capability is absent.
- `combined`: same handling sequence with declared axle geometry and seven
  exact reaction states. Checks one common arrangement, not separate alternatives.
- `combined.validate.json`: same model plus fixed plan/trip flags and the exact
  witness. It specifies future fixed-plan replay, without finding a substitute.

All travel coordinates are identical; ordinary service is 60 seconds per task.
The first reshuffled stop departs at 120 seconds and the second at 180 seconds.
The synthetic `a`/`b`/`c`/`d` profile hashes only illustrate identity fields; native
format validation does not authenticate them as real calibration documents.
Illustrative node counts are not measured solver performance or search-optimality
claims. Source/executable/scenario/tenant binding belongs in the later Cursus
certificate envelope and cannot be omitted for production approval.

Design verification performed while preparing the proposal:

- Draft-07 schema meta-validation and validation of all request/result examples.
- Negative shape cases for missing capabilities/facts, duplicate capability,
  unknown field, wrong null/type and missing frontier budget.
- C11 syntax checks of the proposed declarations and typed construction examples;
  C++17 syntax check of the proposed public declaration block.
- Independent exact rational/state/stack/inventory/handling/schedule replay of
  all three witnesses; fixed-plan witness/reference equality; document links.

These are documentation/fixture consistency checks. No proposed native functions
were linked/executed and no customer load validation is claimed. The authoritative
C-versus-JSON, full-plan/concurrency and performance suites in the contract must
be implemented and pass before the API is shipped.
