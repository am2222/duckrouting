# Testing this extension
This directory contains all the tests for this extension. The `sql` directory holds tests that are written as [SQLLogicTests](https://duckdb.org/dev/sqllogictest/intro.html). DuckDB aims to have most its tests in this format as SQL statements, so for the quack extension, this should probably be the goal too.

The root makefile contains targets to build and run all of these tests. To run the SQLLogicTests:
```bash
make test
```
or 
```bash
make test_debug
```

## Test data licence

The sample network and expected results in `sql/` are adapted from the [pgRouting](https://github.com/pgRouting/pgrouting) project (`tools/testers/sampledata.pg`, `docqueries/` and `pgtap/`), © pgRouting developers, licensed under [CC BY-SA 3.0](https://creativecommons.org/licenses/by-sa/3.0/). The rest of the repository is MIT licensed.
