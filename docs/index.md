---
layout: home

hero:
  name: duckrouting
  text: Graph routing for DuckDB
  tagline: Shortest paths, cost matrices and service areas over any table of edges, powered by the Boost Graph Library.
  actions:
    - theme: brand
      text: Getting started
      link: /guide/getting-started
    - theme: alt
      text: Function reference
      link: /functions/dijkstra

features:
  - title: pgRouting semantics
    details: Signatures, column names and edge conventions follow pgRouting, and the test suite checks results against pgRouting's own published output.
  - title: Boost Graph Library
    details: Routing runs on boost::adjacency_list and boost::dijkstra_shortest_paths, so the algorithms are the same ones pgRouting builds on.
  - title: Any table of edges
    details: Point a function at a query returning id, source, target and cost. No topology tables to build, no geometry required.
---
