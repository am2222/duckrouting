import { defineConfig } from 'vitepress'

// Each page under functions/ documents one family of routing functions, the way
// pgRouting groups them. Add a family by dropping in a new page and listing it
// in the sidebar below.
export default defineConfig({
  title: 'duckrouting',
  description: 'Graph routing for DuckDB, built on the Boost Graph Library',
  base: '/duckrouting/',
  lastUpdated: true,
  cleanUrls: true,

  head: [['link', { rel: 'icon', type: 'image/svg+xml', href: '/duckrouting/icon.svg' }]],

  // Repo docs that are not part of the site.
  srcExclude: ['UPDATING.md'],

  vite: {
    // MapLibre starts its worker with { type: 'module' }, so the worker chunk
    // has to be ESM. Vite's default is iife, which loads as a classic script
    // and fails. See the setWorkerUrl call in RoutingDemo.vue.
    worker: { format: 'es' }
  },

  themeConfig: {
    logo: '/icon.svg',

    nav: [
      { text: 'Guide', link: '/guide/getting-started' },
      { text: 'Demo', link: '/demo' },
      { text: 'Functions', link: '/functions/dijkstra' },
      { text: 'pgRouting catalog', link: '/pgrouting-function-catalog' }
    ],

    sidebar: [
      {
        text: 'Guide',
        items: [
          { text: 'Getting started', link: '/guide/getting-started' },
          { text: 'The edges query', link: '/guide/edges-query' },
          { text: 'Live demo', link: '/demo' }
        ]
      },
      {
        text: 'Functions',
        items: [
          { text: 'Dijkstra family', link: '/functions/dijkstra' },
          { text: 'A*', link: '/functions/astar' },
          { text: 'K shortest paths', link: '/functions/ksp' },
          { text: 'All pairs', link: '/functions/all-pairs' },
          { text: 'Components', link: '/functions/components' },
          { text: 'Spanning trees', link: '/functions/spanning-trees' },
          { text: 'Traversal & ordering', link: '/functions/traversal' },
          { text: 'Graph analysis', link: '/functions/analysis' },
          { text: 'Maximum flow', link: '/functions/flow' },
          { text: 'Travelling salesman', link: '/functions/tsp' },
          { text: 'Contraction & points', link: '/functions/contraction' }
        ]
      },
      {
        text: 'Reference',
        items: [
          { text: 'pgRouting function catalog', link: '/pgrouting-function-catalog' }
        ]
      }
    ],

    socialLinks: [
      { icon: 'github', link: 'https://github.com/am2222/duckrouting' }
    ],

    search: { provider: 'local' },

    footer: {
      message: 'Built on the Boost Graph Library. Algorithms follow pgRouting semantics.'
    }
  }
})
