<script setup>
// Routing over a real street network, in the browser, with the actual
// extension -- INSTALL duckrouting FROM community against duckdb-wasm. Nothing
// here is precomputed: every line on screen came back from a SQL query run a
// few milliseconds earlier.
//
// Two things load lazily and separately. The network draws immediately
// (97 KB gzipped), because a map that appears instantly is the point. The
// query engine is ~36 MB and only starts when the reader asks for it, so
// nobody pays for it by scrolling past.
//
// What each mode queries and draws lives in routing-modes.js. This file owns
// the map, the clicks and the chrome, and knows nothing about any particular
// routing function.
import { onMounted, onUnmounted, ref, shallowRef, computed } from 'vue'
import { MODES, GROUPS } from './routing-modes.js'

const NETWORK = '/duckrouting/demo-network.geojson'

const mapEl = ref(null)
const map = shallowRef(null)
const db = shallowRef(null)
const conn = shallowRef(null)

const status = ref('loading the network…')
const engine = ref('cold') // cold | starting | ready | failed
const mode = ref('dijkstra')
const picked = ref([])
const radius = ref(600)
const k = ref(3)
const sql = ref('')
const result = ref('')
const busy = ref(false)

// Vertex id -> [lon, lat] and edge id -> feature, both built from the network
// once so that a click can be snapped to the nearest routable vertex and a
// mode can turn a list of ids straight into geometry.
const vertices = new Map()
const featureById = new Map()
let features = []

const current = computed(() => MODES[mode.value])
const grouped = computed(() => GROUPS.map(g => ({
  ...g,
  modes: Object.entries(MODES).filter(([, m]) => m.group === g.key)
})))
const legend = computed(() => (engine.value === 'ready' && result.value)
  ? current.value.legend ?? null
  : null)
const hint = computed(() => {
  if (engine.value !== 'ready') return ''
  if (busy.value) return 'Running…'
  const m = current.value
  if (m.needs === 0) return ''
  if (m.needs === 'many') {
    const short = m.minimum - picked.value.length
    return short > 0 ? m.hint : 'Click more stops to extend the tour.'
  }
  return picked.value.length < m.needs ? m.hint : ''
})

function nearestVertex(lngLat) {
  let best = null
  let bestDistance = Infinity
  for (const [id, [lon, lat]] of vertices) {
    // Squared degrees is not a distance, but over an area this small the
    // ranking it produces is the same one metres would give.
    const dx = lon - lngLat.lng
    const dy = lat - lngLat.lat
    const d = dx * dx + dy * dy
    if (d < bestDistance) { bestDistance = d; best = id }
  }
  return best
}

const EMPTY = { type: 'FeatureCollection', features: [] }

function draw(layer, list) {
  map.value.getSource(layer).setData(
    list && list.length ? { type: 'FeatureCollection', features: list } : EMPTY
  )
}

function paint(layer, spec, fallback) {
  const it = spec ?? fallback
  const prefix = layer === 'points' ? 'circle' : 'line'
  map.value.setPaintProperty(layer, `${prefix}-color`, it.color)
  map.value.setPaintProperty(layer, layer === 'points' ? 'circle-radius' : 'line-width',
    layer === 'points' ? it.radius : it.width)
  map.value.setPaintProperty(layer, `${prefix}-opacity`, it.opacity ?? 1)
}

async function run() {
  const m = current.value
  if (busy.value) return
  if (m.needs === 'many' ? picked.value.length < m.minimum
    : picked.value.length < m.needs) return
  busy.value = true
  try {
    const t0 = performance.now()
    const out = await m.run({
      query: async statement => (await conn.value.query(statement)).toArray(),
      stops: [...picked.value],
      radius: radius.value,
      k: k.value,
      featureById,
      vertices
    })
    const ms = Math.round(performance.now() - t0)
    sql.value = out.sql
    draw('lower', out.lower)
    draw('upper', out.upper)
    draw('points', out.points)
    paint('lower', out.paint?.lower, { color: '#000', width: 0, opacity: 0 })
    paint('upper', out.paint?.upper, { color: '#000', width: 0, opacity: 0 })
    paint('points', out.paint?.points, { color: '#000', radius: 0, opacity: 0 })
    result.value = `${out.summary} · ${ms} ms`
  } catch (error) {
    result.value = 'query failed: ' + (error?.message ?? error)
  } finally {
    busy.value = false
  }
}

async function startEngine() {
  if (engine.value !== 'cold') return
  engine.value = 'starting'
  try {
    status.value = 'downloading the query engine…'
    const duckdb = await import('@duckdb/duckdb-wasm')
    const bundle = await duckdb.selectBundle(duckdb.getJsDelivrBundles())
    const worker = new Worker(URL.createObjectURL(
      new Blob([`importScripts("${bundle.mainWorker}");`], { type: 'text/javascript' })
    ))
    const instance = new duckdb.AsyncDuckDB(
      new duckdb.ConsoleLogger(duckdb.LogLevel.ERROR), worker
    )
    await instance.instantiate(bundle.mainModule, bundle.pthreadWorker)
    db.value = instance
    conn.value = await instance.connect()

    status.value = 'installing duckrouting…'
    await conn.value.query('INSTALL duckrouting FROM community')
    await conn.value.query('LOAD duckrouting')

    status.value = 'loading the network into DuckDB…'
    const header = 'id,source,target,cost,reverse_cost,capacity,reverse_capacity,x1,y1,x2,y2'
    const body = features.map(f => {
      const p = f.properties
      return [p.id, p.source, p.target, p.cost, p.reverse_cost,
        p.capacity, p.reverse_capacity, p.x1, p.y1, p.x2, p.y2].join(',')
    })
    await db.value.registerFileText('edges.csv', header + '\n' + body.join('\n'))
    await conn.value.query(`CREATE TABLE edges AS SELECT * FROM read_csv('edges.csv')`)

    const version = (await conn.value.query('SELECT version() AS v')).get(0).v
    engine.value = 'ready'
    status.value = `DuckDB ${version} · duckrouting · ${features.length} edges loaded`
    if (current.value.needs === 0) run()
  } catch (error) {
    engine.value = 'failed'
    status.value = 'could not start: ' + (error?.message ?? error)
  }
}

function reset() {
  picked.value = []
  result.value = ''
  sql.value = ''
  for (const id of ['lower', 'upper', 'points', 'picked']) {
    map.value?.getSource(id)?.setData(EMPTY)
  }
}

function setMode(next) {
  mode.value = next
  reset()
  // A whole-network mode has nothing to wait for, so it runs on selection.
  if (engine.value === 'ready' && MODES[next].needs === 0) run()
}

function showPicked() {
  map.value.getSource('picked').setData({
    type: 'FeatureCollection',
    features: picked.value.map(v => ({
      type: 'Feature',
      geometry: { type: 'Point', coordinates: vertices.get(v) },
      properties: { id: v }
    }))
  })
}

onMounted(async () => {
  const maplibre = await import('maplibre-gl')
  await import('maplibre-gl/dist/maplibre-gl.css')

  // MapLibre parses GeoJSON in a worker and starts it with { type: 'module' },
  // and its shipped worker imports a sibling module -- so it has to be bundled
  // rather than copied, and emitted as ESM (see worker.format in config.mts).
  // Without this the worker fails to start and every vector layer silently
  // renders nothing, which looks exactly like an empty map.
  const { default: workerUrl } =
    await import('maplibre-gl/dist/maplibre-gl-worker.mjs?worker&url')
  maplibre.setWorkerUrl(workerUrl)

  const response = await fetch(NETWORK)
  const geojson = await response.json()
  features = geojson.features
  for (const f of features) {
    const p = f.properties
    featureById.set(p.id, f)
    if (!vertices.has(p.source)) vertices.set(p.source, [p.x1, p.y1])
    if (!vertices.has(p.target)) vertices.set(p.target, [p.x2, p.y2])
  }

  map.value = new maplibre.Map({
    container: mapEl.value,
    // No basemap. Every key-free raster provider either wants an API key now
    // (Carto stamps "API KEY REQUIRED" across the tiles) or asks not to be
    // used for this, and the network is legible as a map on its own -- it is
    // 1,912 Amsterdam streets. It also means the demo has no third-party
    // runtime dependency beyond the extension it is showing off.
    style: {
      version: 8,
      sources: {},
      layers: [
        { id: 'bg', type: 'background', paint: { 'background-color': '#f1f5f9' } }
      ]
    },
    center: [4.889, 52.372],
    zoom: 13.4,
    attributionControl: {
      compact: false,
      customAttribution:
        'Street data © <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a> contributors (ODbL)'
    }
  })

  map.value.on('load', () => {
    map.value.addSource('network', { type: 'geojson', data: geojson })
    for (const id of ['lower', 'upper', 'points', 'picked']) {
      map.value.addSource(id, { type: 'geojson', data: EMPTY })
    }
    map.value.addLayer({
      id: 'network', type: 'line', source: 'network',
      paint: { 'line-color': '#64748b', 'line-width': 1.2, 'line-opacity': 0.85 }
    })
    // Two line layers, named for their stacking rather than their meaning:
    // KSP wants its alternatives under the best route, maximum flow wants its
    // saturated edges over the corridor.
    for (const id of ['lower', 'upper']) {
      map.value.addLayer({
        id, type: 'line', source: id,
        paint: { 'line-color': '#000', 'line-width': 0, 'line-opacity': 0 }
      })
    }
    map.value.addLayer({
      id: 'points', type: 'circle', source: 'points',
      paint: { 'circle-color': '#000', 'circle-radius': 0, 'circle-opacity': 0 }
    })
    map.value.addLayer({
      id: 'picked', type: 'circle', source: 'picked',
      paint: {
        'circle-radius': 6, 'circle-color': '#111827',
        'circle-stroke-width': 2, 'circle-stroke-color': '#ffffff'
      }
    })
    status.value = `${features.length} edges · ${vertices.size} vertices · engine not started`
  })

  map.value.on('click', event => {
    const m = current.value
    if (engine.value !== 'ready' || busy.value || m.needs === 0) return
    const id = nearestVertex(event.lngLat)
    if (m.needs === 'many') {
      // The tour grows as stops are added, and re-solves on every click once
      // there are enough of them to be a tour at all.
      picked.value = [...picked.value, id]
    } else {
      picked.value = picked.value.length >= m.needs ? [id] : [...picked.value, id]
    }
    showPicked()
    const enough = m.needs === 'many'
      ? picked.value.length >= m.minimum
      : picked.value.length === m.needs
    if (enough) run()
    else { result.value = ''; sql.value = '' }
  })
})

onUnmounted(() => {
  conn.value?.close?.()
  db.value?.terminate?.()
  map.value?.remove?.()
})
</script>

<template>
  <div class="routing-demo">
    <div v-for="group in grouped" :key="group.key" class="bar">
      <span class="group">{{ group.label }}</span>
      <button
        v-for="[key, m] in group.modes" :key="key"
        :class="{ active: mode === key }"
        :disabled="engine !== 'ready'"
        @click="setMode(key)"
      >{{ m.label }}</button>

      <template v-if="current.slider && current.group === group.key && engine === 'ready'">
        <label>
          {{ current.slider.label }}
          <input
            type="range"
            :min="current.slider.min" :max="current.slider.max" :step="current.slider.step"
            :value="current.slider.key === 'radius' ? radius : k"
            @input="current.slider.key === 'radius'
              ? radius = Number($event.target.value)
              : k = Number($event.target.value)"
            @change="run"
          />
          {{ current.slider.key === 'radius' ? radius : k }}{{ current.slider.unit }}
        </label>
      </template>
      <button
        v-if="group.key === 'points' && engine === 'ready'"
        class="ghost" @click="reset"
      >clear</button>
    </div>

    <div class="map-wrap">
      <div class="map" ref="mapEl"></div>
      <div v-if="engine !== 'ready'" class="overlay">
        <button v-if="engine === 'cold'" class="start" @click="startEngine">
          Start query engine · ~36 MB
        </button>
        <p v-else-if="engine === 'starting'">{{ status }}</p>
        <p v-else class="failed">{{ status }}</p>
      </div>
    </div>

    <p v-if="legend" class="legend">
      <span v-for="([colour, text]) in legend" :key="text">
        <i class="swatch" :style="{ background: colour }"></i>{{ text }}
      </span>
    </p>

    <p class="status">
      <span>{{ status }}</span>
      <span v-if="hint" class="hint">{{ hint }}</span>
      <span v-if="result" class="result">{{ result }}</span>
    </p>

    <pre v-if="sql" class="sql">{{ sql }}</pre>
  </div>
</template>

<style scoped>
.routing-demo { position: relative; margin: 1.5rem 0; }
.map {
  height: 460px; border-radius: 8px; overflow: hidden;
  border: 1px solid var(--vp-c-divider);
}
.bar { display: flex; flex-wrap: wrap; gap: .5rem; align-items: center; margin-bottom: .5rem; }
.bar .group {
  font-size: .7rem; text-transform: uppercase; letter-spacing: .04em;
  color: var(--vp-c-text-3); width: 6.4rem;
}
.bar button {
  border: 1px solid var(--vp-c-divider); border-radius: 6px;
  padding: .3rem .7rem; font-size: .85rem; background: var(--vp-c-bg-soft);
  cursor: pointer;
}
.bar button:disabled { opacity: .45; cursor: not-allowed; }
.bar button.active { background: var(--vp-c-brand-1); color: #fff; border-color: var(--vp-c-brand-1); }
.bar .ghost { margin-left: auto; }
.bar label { font-size: .8rem; display: flex; align-items: center; gap: .4rem; }
.map-wrap { position: relative; }
.overlay {
  position: absolute; inset: 0;
  display: flex; align-items: center; justify-content: center;
  background: rgba(255, 255, 255, .72); border-radius: 8px; text-align: center;
}
.dark .overlay { background: rgba(0, 0, 0, .55); }
.start {
  border: 0; border-radius: 8px; padding: .7rem 1.2rem; font-size: .95rem;
  background: var(--vp-c-brand-1); color: #fff; cursor: pointer;
}
.failed { color: var(--vp-c-danger-1); max-width: 34rem; }
.status, .legend {
  display: flex; flex-wrap: wrap; gap: .75rem;
  font-size: .8rem; color: var(--vp-c-text-2); margin: .6rem 0 0;
}
.legend { gap: 1rem; }
.legend span { display: flex; align-items: center; gap: .4rem; }
.swatch { width: 1.1rem; height: 3px; border-radius: 2px; display: inline-block; }
.status .result { color: var(--vp-c-brand-1); font-weight: 600; }
.sql {
  font-size: .78rem; margin-top: .5rem; padding: .6rem .8rem;
  background: var(--vp-c-bg-soft); border-radius: 6px; overflow-x: auto;
  white-space: pre;
}
</style>
