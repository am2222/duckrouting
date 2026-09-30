# Extension updating 
When cloning this template, the target version of DuckDB should be the latest stable release of DuckDB. However, there 
will inevitably come a time when a new DuckDB is released and the extension repository needs updating. This process goes
as follows:

- Bump submodules
  - `./duckdb` should be set to latest tagged release
  - `./extension-ci-tools` should be set to updated branch corresponding to latest DuckDB release. So if you're building for DuckDB `v1.1.0` there will be a branch in `extension-ci-tools` named `v1.1.0` to which you should check out. 
- Bump versions in `./github/workflows`
  - `duckdb_version` input in `duckdb-stable-build` job in `MainDistributionPipeline.yml` should be set to latest tagged release
  - `duckdb_version` input in `duckdb-stable-deploy` job in `MainDistributionPipeline.yml` should be set to latest tagged release
  - the reusable workflow `duckdb/extension-ci-tools/.github/workflows/_extension_distribution.yml` for the `duckdb-stable-build` job should be set to latest tagged release

# API changes
DuckDB extensions built with this extension template are built against the internal C++ API of DuckDB. This API is not guaranteed to be stable.
What this means for extension development is that when updating your extensions DuckDB target version using the above steps, you may run into the fact that your extension no longer builds properly.

Currently, DuckDB does not (yet) provide a specific change log for these API changes, but it is generally not too hard to figure out what has changed.

For figuring out how and why the C++ API changed, we recommend using the following resources:
- DuckDB's [Release Notes](https://github.com/duckdb/duckdb/releases)
- DuckDB's history of [Core extension patches](https://github.com/duckdb/duckdb/commits/main/.github/patches/extensions)
- The git history of the relevant C++ Header file of the API that has changed
# Follow-up: when DuckDB 2.0 becomes the stable target

Written September 2026, while 2.0 was still the `v2.0-cyanoptera` branch that
the canary job in `MainDistributionPipeline.yml` builds against. Do this once
that branch is a tagged release and the steps above have moved the submodules
and workflows onto it.

## Why there is something to do

DuckDB 2.0 changed how table functions describe themselves
([duckdb/duckdb#26091](https://github.com/duckdb/duckdb/pull/26091)). A
`TableFunction` now carries a `FunctionSignature`, like scalar functions do,
and `duckdb_functions()` reads parameter names from that signature. The
`named_parameters` map that 1.5 used to declare options is gone; options are
the typed kwargs of a `**options` parameter on the signature.

`src/include/duckrouting/compat.hpp` papers over the difference so that the
same source builds against both versions: `AddNamedParameter`,
`NamedParameterNames` and `ArgumentTypes` each try the 1.5 spelling first and
fall back to the signature. That keeps every build green, but it leaves one
thing worse on 2.0 than on 1.5: **argument names**. Every table function here
is built from a list of types, which on 2.0 becomes positional-only parameters
auto-named `col0`, `col1`, ... The documented names in `src/function_docs.cpp`
are attached as a `FunctionDescription`, which 2.0 only consults when the
description and the signature disagree on how many parameters there are. They
never disagree, so on 2.0 `duckdb_functions()` reports `col0` for everything.
The last assertion in `test/sql/duckrouting.test` pins exactly that, with a
version split, so nobody mistakes it for a regression.

## What to change

1. **Name the parameters on the signature.** The cleanest place is
   `Documented(duckdb::TableFunctionSet)` in `src/function_docs.cpp`, which
   already walks every overload against its `FunctionDoc`. Rebuild the set:
   copy each `TableFunction`, replace its signature with one whose positional
   parameters carry `doc.parameters[i]` -- `FunctionSignature::AddParameter(
   name, type)` with the default `STANDARD` kind -- carry the existing typed
   kwargs across with `WithTypedKwargs("options", ...)`, and add the copy to a
   fresh set. Overloads in a 2.0 set are `shared_ptr<const TableFunction>`, so
   the rewrite has to happen before `AddFunction`, not on the stored overload.
   Once this is in, `NamePositionalArguments` and the
   `description.parameter_names` bookkeeping for table functions are redundant
   and can go.

   This is also a behaviour change worth a line in the changelog: `STANDARD`
   parameters can be passed by name, so
   `duckrouting_dijkstra(edges_sql => ..., start_vid => 5, end_vid => 7)`
   starts to work. Keep the names in `kFunctionDocs` in step with what the
   docs site says, since they become part of the SQL interface.

2. **Drop the version split.** In `test/sql/duckrouting.test`, the `colN`
   assertion becomes a plain `0` on every version, and its explanatory comment
   shrinks to a sentence. The `ELSE 301` count is there only to keep the test
   live while the names are ignored; it stops meaning anything once they are
   honoured. Run `SELECT function_name, parameters FROM duckdb_functions()
   WHERE function_name LIKE 'duckrouting_%'` and eyeball it once.

3. **Retire the 1.5 branches of `compat.hpp`, but only when 1.5 support is
   actually dropped.** The community-extensions repository builds an
   extension per DuckDB version, so there may be a stretch where both are
   shipped from one source tree. While that lasts, leave the ranked candidates
   alone. When 1.5 goes, each helper collapses to its signature form:
   `AddNamedParameter` keeps only the typed-kwargs candidate,
   `NamedParameterNames` the `GetTypedKwargs()` one, `ArgumentTypes` the
   `GetSignature()` one, and the `std::string` overloads of `NameMatches` and
   `NameText`, plus the `InfoName` and `Overload` pairs, lose their 1.5 half.
   At that point most of the file is a one-liner each and can be inlined at
   the call sites if that reads better.

4. **Re-check the binder behaviour changes** the DuckDB pull request lists,
   with the SQL suite as the judge:
   - a named argument now binds only if it casts implicitly, where 1.5 forced
     the cast after selection;
   - a scalar no longer binds to a `LIST` parameter on its own. Every vertex
     argument here has explicit `BIGINT` and `BIGINT[]` overloads, so this
     should be invisible, but it is the sort of thing that surfaces in a docs
     example rather than a test;
   - the literals `0` and `1` now cast to `BOOLEAN`, so `directed` accepts
     them.

## Verifying against the branch before it is released

The canary job is the source of truth, but a local build against the branch
is faster to iterate on. It needs a full DuckDB build, about ten minutes on a
laptop, into a build directory of its own so the stable build stays intact:

```shell
git clone --depth 1 --branch v2.0-cyanoptera https://github.com/duckdb/duckdb.git /tmp/duckdb-v2
make -n release | grep 'cmake '        # the flags the stable build uses
cmake -G Ninja <those flags> -DCMAKE_TOOLCHAIN_FILE=$VCPKG_TOOLCHAIN_PATH \
      -DCMAKE_BUILD_TYPE=Release -S /tmp/duckdb-v2 -B build/canary
cmake --build build/canary
./build/canary/test/unittest "test/sql/*"
```

Then the usual `make`, `make test` and `make format-check` against 1.5, since
a compat change has to hold on both sides.
