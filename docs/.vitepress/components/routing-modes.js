// One entry per thing the demo can show. Each mode owns its SQL and decides
// what to draw; RoutingDemo.vue owns the map, the clicks and the chrome, and
// knows nothing about any particular function.
//
// A mode returns up to three sets of features -- `lower`, `upper` and
// `points` -- plus the paint to draw them with. Two line layers rather than
// one because the orderings genuinely differ: KSP wants its alternatives
// *under* the best route, maximum flow wants its saturated edges *over* the
// corridor. Both are "secondary under/over primary", so the layers are named
// for their stacking rather than their meaning.

const EDGES = `'SELECT id, source, target, cost, reverse_cost FROM edges'`
const CAPACITY = `'SELECT id, source, target, capacity, reverse_capacity FROM edges'`

const CRIMSON = '#e11d48'
const AMBER = '#f59e0b'
const SKY = '#0ea5e9'
const VIOLET = '#7c3aed'
const RED = '#dc2626'
const GREEN = '#059669'
const INK = '#111827'

/** Features for a set of edge ids, in the order given. */
function edges(ids, ctx, extra) {
  const out = []
  for (const id of ids) {
    const feature = ctx.featureById.get(Number(id))
    if (!feature) continue
    out.push(extra
      ? { ...feature, properties: { ...feature.properties, ...extra(Number(id)) } }
      : feature)
  }
  return out
}

/** Point features for a set of vertex ids. */
function points(ids, ctx, properties) {
  const out = []
  for (const id of ids) {
    const at = ctx.vertices.get(Number(id))
    if (!at) continue
    out.push({
      type: 'Feature',
      geometry: { type: 'Point', coordinates: at },
      properties: { id: Number(id), ...(properties ? properties(Number(id)) : {}) }
    })
  }
  return out
}

const line = (color, width, opacity = 1) => ({ color, width, opacity })

// --- geometry for map matching --------------------------------------------
//
// Map matching needs, for every GPS fix, the nearby edges with the fraction
// along each where the fix projects and the distance to that point. With the
// spatial extension that is one call to duckrouting_find_close_edges, but
// loading spatial into the browser is another sizeable download for a single
// mode, so the projection is done here in a few lines and handed to DuckDB
// as a table. Everything the algorithm does then happens in SQL.
//
// Coordinates go through an equirectangular projection centred on the map, so
// distances come out in metres -- the same unit as `cost` -- which is what
// the transition probability compares them against.

const CENTRE = [4.889, 52.372]
const METRES_PER_DEG_LAT = 110574
const METRES_PER_DEG_LON = 111320 * Math.cos(CENTRE[1] * Math.PI / 180)

const project = ([lon, lat]) =>
  [(lon - CENTRE[0]) * METRES_PER_DEG_LON, (lat - CENTRE[1]) * METRES_PER_DEG_LAT]
const unproject = ([x, y]) =>
  [x / METRES_PER_DEG_LON + CENTRE[0], y / METRES_PER_DEG_LAT + CENTRE[1]]

// Projected polylines, computed once per feature.
const projected = new WeakMap()
function polyline(feature) {
  let coords = projected.get(feature)
  if (!coords) {
    coords = feature.geometry.coordinates.map(project)
    projected.set(feature, coords)
  }
  return coords
}

/** Closest point on a polyline to `p`: its distance, and the fraction of the
 *  polyline's length at which it lies. */
function closestOn(coords, p) {
  let best = { distance: Infinity, along: 0, point: coords[0] }
  let length = 0
  for (let i = 0; i + 1 < coords.length; i++) {
    const [ax, ay] = coords[i]
    const [bx, by] = coords[i + 1]
    const dx = bx - ax
    const dy = by - ay
    const len2 = dx * dx + dy * dy
    let t = len2 ? ((p[0] - ax) * dx + (p[1] - ay) * dy) / len2 : 0
    t = Math.max(0, Math.min(1, t))
    const point = [ax + t * dx, ay + t * dy]
    const distance = Math.hypot(p[0] - point[0], p[1] - point[1])
    if (distance < best.distance) best = { distance, along: length + t * Math.sqrt(len2), point }
    length += Math.sqrt(len2)
  }
  return { distance: best.distance, fraction: length ? best.along / length : 0 }
}

/** The point `fraction` of the way along a polyline, in lon/lat. */
function pointAt(coords, fraction) {
  const lengths = []
  let total = 0
  for (let i = 0; i + 1 < coords.length; i++) {
    const l = Math.hypot(coords[i + 1][0] - coords[i][0], coords[i + 1][1] - coords[i][1])
    lengths.push(l)
    total += l
  }
  let remaining = fraction * total
  for (let i = 0; i < lengths.length; i++) {
    if (remaining <= lengths[i] || i === lengths.length - 1) {
      const t = lengths[i] ? Math.min(1, remaining / lengths[i]) : 0
      return unproject([
        coords[i][0] + t * (coords[i + 1][0] - coords[i][0]),
        coords[i][1] + t * (coords[i + 1][1] - coords[i][1])
      ])
    }
    remaining -= lengths[i]
  }
  return unproject(coords[coords.length - 1])
}

/** The `k` nearest edges within `radius` metres of each fix, in the shape
 *  duckrouting_map_match wants. */
function candidates(fixes, ctx, radius, k) {
  const rows = []
  fixes.forEach((lngLat, i) => {
    const p = project(lngLat)
    const near = []
    for (const [id, feature] of ctx.featureById) {
      const hit = closestOn(polyline(feature), p)
      if (hit.distance <= radius) near.push({ edge_id: id, ...hit })
    }
    near.sort((a, b) => a.distance - b.distance || a.edge_id - b.edge_id)
    for (const c of near.slice(0, k)) {
      rows.push({ pid: i + 1, edge_id: c.edge_id, fraction: c.fraction, distance: c.distance, x: p[0], y: p[1] })
    }
  })
  return rows
}

export const MODES = {
  // --- pick points ---------------------------------------------------------

  dijkstra: {
    label: 'Shortest path',
    group: 'points',
    needs: 2,
    hint: 'Click a start and an end.',
    async run(ctx) {
      const sql = `SELECT edge, agg_cost FROM duckrouting_dijkstra(${EDGES}, ${ctx.stops[0]}, ${ctx.stops[1]})`
      const rows = await ctx.query(sql)
      const used = rows.map(r => Number(r.edge)).filter(e => e > 0)
      const total = rows.reduce((m, r) => Math.max(m, Number(r.agg_cost)), 0)
      return {
        sql,
        upper: edges(used, ctx),
        paint: { upper: line(CRIMSON, 4.5) },
        summary: used.length
          ? `${used.length} edges · ${Math.round(total)} m`
          : 'no route between those two points'
      }
    }
  },

  driving_distance: {
    label: 'Driving distance',
    group: 'points',
    needs: 1,
    hint: 'Click one point.',
    slider: { key: 'radius', min: 200, max: 2000, step: 100, unit: 'm', label: 'radius' },
    async run(ctx) {
      const sql = `SELECT edge, agg_cost FROM duckrouting_driving_distance(${EDGES}, ${ctx.stops[0]}, ${ctx.radius}.0)`
      const rows = await ctx.query(sql)
      const used = rows.map(r => Number(r.edge)).filter(e => e > 0)
      const far = rows.reduce((m, r) => Math.max(m, Number(r.agg_cost)), 0)
      return {
        sql,
        upper: edges(used, ctx),
        paint: { upper: line(SKY, 3, 0.8) },
        summary: `${rows.length} nodes within ${ctx.radius} m · furthest ${Math.round(far)} m`
      }
    }
  },

  ksp: {
    label: 'K alternatives',
    group: 'points',
    needs: 2,
    hint: 'Click a start and an end.',
    slider: { key: 'k', min: 2, max: 6, step: 1, unit: '', label: 'k' },
    legend: [[CRIMSON, 'cheapest route'], [AMBER, 'alternatives']],
    async run(ctx) {
      const sql = `SELECT path_id, edge, agg_cost FROM duckrouting_ksp(${EDGES}, ${ctx.stops[0]}, ${ctx.stops[1]}, ${ctx.k})`
      const rows = await ctx.query(sql)
      const byPath = new Map()
      for (const r of rows) {
        if (Number(r.edge) < 0) continue
        const id = Number(r.path_id)
        if (!byPath.has(id)) byPath.set(id, [])
        byPath.get(id).push(Number(r.edge))
      }
      const paths = [...byPath.entries()].sort((x, y) => x[0] - y[0])
      return {
        sql,
        lower: edges(paths.slice(1).flatMap(p => p[1]), ctx),
        upper: edges(paths.length ? paths[0][1] : [], ctx),
        paint: { lower: line(AMBER, 3, 0.85), upper: line(CRIMSON, 4.5) },
        summary: `${paths.length} route${paths.length === 1 ? '' : 's'}`
      }
    }
  },

  max_flow: {
    label: 'Maximum flow',
    group: 'points',
    needs: 2,
    hint: 'Click a source and a sink.',
    legend: [[VIOLET, 'carries flow — width is volume'], [RED, 'at capacity — the bottleneck']],
    async run(ctx) {
      // Edmonds-Karp rather than push-relabel. Both return the same maximum,
      // but push-relabel leaves circulation -- cycles that satisfy flow
      // conservation while carrying nothing between the two points. On this
      // network that is 474 edges in 16 disconnected components against
      // Edmonds-Karp's 57, only one of which even touches the source or sink.
      // Correct either way; only one of them is a picture.
      const sql = `SELECT edge, start_vid, end_vid, flow, residual_capacity FROM duckrouting_edmonds_karp(${CAPACITY}, ${ctx.stops[0]}, ${ctx.stops[1]})`
      const rows = await ctx.query(sql)
      const byId = new Map(rows.map(r => [Number(r.edge), r]))
      let low = Infinity
      let high = -Infinity
      for (const r of rows) {
        low = Math.min(low, Number(r.flow))
        high = Math.max(high, Number(r.flow))
      }
      const carrying = edges([...byId.keys()], ctx, id => ({
        flow: Number(byId.get(id).flow),
        saturated: Number(byId.get(id).residual_capacity) === 0
      }))
      const saturated = carrying.filter(f => f.properties.saturated)
      // The maximum flow is what leaves the source, not the sum over every
      // edge, which would count the same vehicles again at each hop.
      const source = ctx.stops[0]
      const total = rows.reduce((s, r) =>
        s + (Number(r.start_vid) === source ? Number(r.flow) : 0)
          - (Number(r.end_vid) === source ? Number(r.flow) : 0), 0)
      // Most pairs come back as one corridor at a single magnitude, and
      // interpolate() needs strictly ascending stops, so a flat result gets a
      // flat width rather than an exception.
      const scale = (min, max) => high > low
        ? ['interpolate', ['linear'], ['get', 'flow'], low, min, high, max]
        : (min + max) / 2
      return {
        sql,
        lower: carrying,
        upper: saturated,
        paint: {
          lower: { color: VIOLET, width: scale(2.5, 8), opacity: 0.85 },
          // Wider than whatever is underneath, or they vanish into it -- and
          // there are often only two or three, on short segments.
          upper: { color: RED, width: scale(6, 11.5), opacity: 1 }
        },
        summary: `${Math.round(total)} vehicles/h · ${rows.length} edges · ${saturated.length} at capacity`
      }
    }
  },

  tsp: {
    label: 'Travelling salesman',
    group: 'points',
    needs: 'many',
    minimum: 3,
    hint: 'Click three or more stops.',
    legend: [[CRIMSON, 'tour'], [INK, 'stops']],
    async run(ctx) {
      const stops = ctx.stops
      const list = `[${stops.join(', ')}]`
      // Three functions chained: a cost matrix over the stops, the tour order
      // over that matrix, then the via-router to turn an order back into
      // streets. Table-function arguments must be constant-foldable, so the
      // matrix is materialised rather than nested.
      const matrix = `CREATE OR REPLACE TABLE tsp_matrix AS SELECT * FROM duckrouting_dijkstra_cost_matrix(${EDGES}, ${list}, false)`
      const tour = `SELECT seq, node, agg_cost FROM duckrouting_tsp('SELECT start_vid, end_vid, agg_cost FROM tsp_matrix')`
      await ctx.query(matrix)
      const order = await ctx.query(tour)
      const visits = order.map(r => Number(r.node))
      const route = `SELECT edge FROM duckrouting_dijkstra_via(${EDGES}, [${visits.join(', ')}], false)`
      const rows = await ctx.query(route)
      const used = rows.map(r => Number(r.edge)).filter(e => e > 0)
      const total = order.reduce((m, r) => Math.max(m, Number(r.agg_cost)), 0)
      return {
        sql: [matrix + ';', tour + ';', route].join('\n'),
        upper: edges(used, ctx),
        points: points(stops, ctx),
        paint: { upper: line(CRIMSON, 4), points: { color: INK, radius: 6 } },
        summary: `${stops.length} stops · ${Math.round(total)} m · ` +
          `order ${visits.join(' → ')}`
      }
    }
  },

  map_match: {
    label: 'Map matching',
    group: 'points',
    needs: 'many',
    minimum: 2,
    raw: true,
    hint: 'Click along a route to drop GPS fixes — they need not be on a street.',
    more: 'Keep clicking to extend the trace.',
    slider: { key: 'gps_error', min: 5, max: 80, step: 5, unit: ' m', label: 'gps error' },
    legend: [[INK, 'GPS fixes — your clicks'], [SKY, 'where each fix was matched'], [CRIMSON, 'matched route']],
    async run(ctx) {
      const fixes = ctx.stops
      // Search a little beyond the noise level: a fix three standard
      // deviations off its street is still meant to be on it.
      const radius = Math.max(40, 3 * ctx.gps_error)
      const found = candidates(fixes, ctx, radius, 6)
      if (!found.length) {
        return { sql: '', summary: `no street within ${radius} m of any fix` }
      }
      const values = found.map(c =>
        `  (${c.pid}, ${c.edge_id}, ${c.fraction.toFixed(4)}, ${c.distance.toFixed(1)}, ${c.x.toFixed(1)}, ${c.y.toFixed(1)})`)
      const table = `CREATE OR REPLACE TABLE candidates AS SELECT * FROM (VALUES\n${values.join(',\n')}\n) AS t(pid, edge_id, fraction, distance, x, y)`
      const CANDIDATES = `'SELECT pid, edge_id, fraction, distance, x, y FROM candidates'`
      const matchSql = `SELECT pid, edge, fraction, ep, tp FROM duckrouting_map_match(${EDGES}, ${CANDIDATES}, ${ctx.gps_error}.0)`
      const pathSql = `SELECT path_seq, node, edge, cost, agg_cost FROM duckrouting_map_match_path(${EDGES}, ${CANDIDATES}, ${ctx.gps_error}.0)`
      await ctx.query(table)
      const [matched, path] = await Promise.all([ctx.query(matchSql), ctx.query(pathSql)])
      const sql = [table + ';', matchSql + ';', pathSql].join('\n')

      const used = path.map(r => Number(r.edge)).filter(e => e > 0)
      const total = path.reduce((m, r) => Math.max(m, Number(r.agg_cost)), 0)
      // Where each fix landed, and a tie from the fix to that point.
      const landed = []
      const ties = []
      for (const r of matched) {
        const feature = ctx.featureById.get(Number(r.edge))
        if (!feature) continue
        const at = pointAt(polyline(feature), Number(r.fraction))
        const fix = fixes[Number(r.pid) - 1]
        landed.push({ type: 'Feature', geometry: { type: 'Point', coordinates: at }, properties: { pid: Number(r.pid) } })
        ties.push({ type: 'Feature', geometry: { type: 'LineString', coordinates: [fix, at] }, properties: {} })
      }
      const unmatched = matched.length < fixes.length
      return {
        sql,
        lower: ties,
        upper: edges(used, ctx),
        points: landed,
        paint: { lower: line(SKY, 1.5, 0.9), upper: line(CRIMSON, 4.5), points: { color: SKY, radius: 4 } },
        summary: unmatched
          ? `unmatched — no route joins those ${fixes.length} fixes; try fixes closer together or a larger gps error`
          : `${fixes.length} fixes · ${used.length} edges · ${Math.round(total)} m`
      }
    }
  },

  // --- whole network -------------------------------------------------------

  spanning_tree: {
    label: 'Spanning tree',
    group: 'network',
    needs: 0,
    legend: [[GREEN, 'minimum spanning forest'], [AMBER, 'redundant — every street not needed to stay connected']],
    async run(ctx) {
      const sql = `SELECT edge, cost FROM duckrouting_kruskal(${EDGES})`
      const rows = await ctx.query(sql)
      const keep = new Set(rows.map(r => Number(r.edge)))
      const spare = [...ctx.featureById.keys()].filter(id => !keep.has(id))
      const total = rows.reduce((s, r) => s + Number(r.cost), 0)
      return {
        sql,
        lower: edges([...keep], ctx),
        upper: edges(spare, ctx),
        paint: { lower: line(GREEN, 1.6, 0.9), upper: line(AMBER, 3) },
        summary: `${keep.size} streets hold the network together (${(total / 1000).toFixed(1)} km) · ` +
          `${spare.length} are redundant`
      }
    }
  },

  one_way_traps: {
    label: 'One-way traps',
    group: 'network',
    needs: 0,
    legend: [[RED, 'cut off by one-way restrictions']],
    async run(ctx) {
      // Strongly connected means every vertex can reach every other *following
      // edge direction*. Anything outside the giant component is a pocket you
      // can drive into but not out of, or the reverse.
      const sql = `SELECT component, node FROM duckrouting_strong_components(${EDGES})`
      const rows = await ctx.query(sql)
      const size = new Map()
      const of = new Map()
      for (const r of rows) {
        const c = Number(r.component)
        of.set(Number(r.node), c)
        size.set(c, (size.get(c) ?? 0) + 1)
      }
      let giant = null
      for (const [c, n] of size) if (giant === null || n > size.get(giant)) giant = c
      const stranded = new Set([...of.entries()]
        .filter(([, c]) => c !== giant).map(([node]) => node))
      const cut = []
      for (const [id, f] of ctx.featureById) {
        if (stranded.has(f.properties.source) || stranded.has(f.properties.target)) cut.push(id)
      }
      return {
        sql,
        upper: edges(cut, ctx),
        points: points([...stranded], ctx),
        paint: { upper: line(RED, 3), points: { color: RED, radius: 3 } },
        summary: `${size.size} strongly connected components · ` +
          `${size.get(giant)} junctions in the main body · ` +
          `${stranded.size} stranded in ${size.size - 1} pockets`
      }
    }
  },

  critical_links: {
    label: 'Critical links',
    group: 'network',
    needs: 0,
    legend: [[RED, 'bridge — removing it splits the network'], [INK, 'articulation point']],
    async run(ctx) {
      const bridgeSql = `SELECT edge FROM duckrouting_bridges(${EDGES})`
      const articulationSql = `SELECT node FROM duckrouting_articulation_points(${EDGES})`
      const [bridges, articulations] = await Promise.all([
        ctx.query(bridgeSql), ctx.query(articulationSql)
      ])
      return {
        sql: [bridgeSql + ';', articulationSql].join('\n'),
        upper: edges(bridges.map(r => Number(r.edge)), ctx),
        points: points(articulations.map(r => Number(r.node)), ctx),
        paint: { upper: line(RED, 3.5), points: { color: INK, radius: 3.5 } },
        summary: `${bridges.length} bridges · ${articulations.length} articulation points`
      }
    }
  },

  centrality: {
    label: 'Centrality',
    group: 'network',
    needs: 0,
    legend: [[SKY, 'rarely on a shortest path'], [CRIMSON, 'often — a bottleneck junction']],
    async run(ctx) {
      // directed => true, and not only because a one-way network is the
      // honest way to ask this. The undirected variant is broken under
      // WebAssembly: on this network it returns inf or NaN for 714 of the
      // 1,484 junctions, reproducibly, where the same build run natively
      // returns none. The directed path is clean on both.
      const sql = `SELECT vid, centrality FROM duckrouting_betweenness_centrality(${EDGES}, true)`
      const rows = await ctx.query(sql)
      const score = new Map()
      let dropped = 0
      for (const r of rows) {
        const value = Number(r.centrality)
        // Never feed a non-finite value to the ramp below: interpolate() wants
        // strictly ascending stops and silently drops the whole layer if a
        // stop is NaN or infinite.
        if (Number.isFinite(value)) score.set(Number(r.vid), value)
        else dropped++
      }
      let peak = 0
      let busiest = null
      for (const [vid, value] of score) {
        if (value > peak) { peak = value; busiest = vid }
      }
      if (!(peak > 0)) {
        return { sql, summary: `${score.size} junctions · no centrality to show` }
      }
      return {
        sql,
        points: points([...score.keys()], ctx, id => ({ centrality: score.get(id) })),
        paint: {
          points: {
            // Straight to the peak rather than 0..1: real betweenness on a
            // street network is tiny almost everywhere, so a 0..1 ramp would
            // render every junction the same colour.
            color: ['interpolate', ['linear'], ['get', 'centrality'],
              0, SKY, peak / 2, AMBER, peak, CRIMSON],
            radius: ['interpolate', ['linear'], ['get', 'centrality'],
              0, 2, peak, 7]
          }
        },
        summary: `${score.size} junctions · busiest is ${busiest} at ${peak.toFixed(3)}` +
          (dropped ? ` · ${dropped} skipped` : '')
      }
    }
  },

  contraction: {
    label: 'Contraction',
    group: 'network',
    needs: 0,
    legend: [[VIOLET, 'absorbed into a shortcut']],
    async run(ctx) {
      // Dead ends get absorbed into their neighbour and degree-two chains
      // collapse into a single edge. The `v` rows name the survivors and what
      // each swallowed; the `e` rows are the shortcuts, which have no geometry
      // of their own because they stand in for a chain.
      const sql = `SELECT type, id, contracted_vertices FROM duckrouting_contraction(${EDGES}, false)`
      const rows = await ctx.query(sql)
      const absorbed = []
      let shortcuts = 0
      for (const r of rows) {
        const list = Array.from(r.contracted_vertices ?? [], Number)
        if (r.type === 'e') shortcuts++
        else absorbed.push(...list)
      }
      const touching = []
      const gone = new Set(absorbed)
      for (const [id, f] of ctx.featureById) {
        if (gone.has(f.properties.source) || gone.has(f.properties.target)) touching.push(id)
      }
      return {
        sql,
        lower: edges(touching, ctx),
        points: points(absorbed, ctx),
        paint: {
          lower: line(VIOLET, 2.5, 0.55),
          points: { color: VIOLET, radius: 3.5 }
        },
        summary: `${gone.size} junctions absorbed · ${shortcuts} shortcut edges · ` +
          `${ctx.vertices.size - gone.size} junctions left to route over`
      }
    }
  }
}

export const GROUPS = [
  { key: 'points', label: 'Pick points' },
  { key: 'network', label: 'Whole network' }
]
