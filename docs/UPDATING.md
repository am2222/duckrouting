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
# Keeping the docs demo in step with DuckDB

The demo in `docs/.vitepress/components/RoutingDemo.vue` runs
`INSTALL duckrouting FROM community` inside duckdb-wasm, so it loads the
community binary built for whichever DuckDB version the pinned
`@duckdb/duckdb-wasm` embeds. The community repo rebuilds only the current
stable DuckDB slot; older slots keep whatever was last built for them. After
a DuckDB bump in `MainDistributionPipeline.yml`, a new duckrouting release
lands only in the new slot and the demo, still on the old slot, reports
`Table Function ... does not exist` for anything added since.

When `duckdb_version` changes, bump `@duckdb/duckdb-wasm` in `docs/package.json`
to a build that embeds the same DuckDB. The right version is usually on the
`next` npm tag (`npm view @duckdb/duckdb-wasm dist-tags`); check with

    strings node_modules/@duckdb/duckdb-wasm/dist/duckdb-eh.wasm | grep -E '^v[0-9]+\.[0-9]+\.[0-9]+$' | sort -V | tail -1
