# FishPump ESP32-S3 Firmware Architecture & Data Flow Map Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Create a standalone, interactive, animated HTML visualization (`map.html` at repository root) that maps the FishPump ESP32-S3 firmware architecture, FreeRTOS tasks, component interconnections, in-memory telemetry pub/sub bus, and data flow scenarios.

**Architecture:** A single-file web application (`map.html`) containing inline semantic HTML5, modern CSS3 (custom properties, responsive glassmorphism dark theme, particle motion animations), vector SVG canvas (dynamic path rendering, glow filters, layer visual bands), and modular vanilla JavaScript (scenario engine, inspector drawer controller, focus highlighter). Zero external dependencies or CDNs.

**Tech Stack:** HTML5, Embedded SVG, CSS3 (Keyframe animations, Flexbox/Grid, CSS Variables), Vanilla ES6+ JavaScript.

## Global Constraints

- **File Path:** Must reside at the repository root: `c:\Users\Copter\OneDrive\Desktop\fishpump-online\map.html`
- **Zero External Dependencies:** No external fonts, icons, CSS frameworks, or JS scripts loaded over HTTP/HTTPS. Must work completely offline.
- **Language Convention:** Explanations and descriptions in Thai (ภาษาไทย); C symbols, FreeRTOS types, and technical acronyms in English.
- **Strict Architecture Accuracy:** Must accurately reflect the real C codebase in `firmware/` (`app_main.c`, `telemetry_hub`, `pump_control`, `cooling_control`, `nvs_store`, `web_server`, `wifi_manager`, `hardware_map`, `session`, `relay_safe_init`).

---

### Task 1: Comprehensive Architecture Data Dictionary & System Topology

**Files:**
- Create: `scratch/map_data.js` (used to build and test the comprehensive data structure before embedding)
- Test: `scratch/test_map_data.js`

**Interfaces:**
- Produces: `SYSTEM_MAP_DATA` object containing:
  - `layers`: Array of 4 layers (`hardware`, `tasks`, `bus_core`, `network_ui`) with titles, badges, and descriptions.
  - `components`: Object mapping each component key (`app_main`, `telemetry_hub`, `pump_control`, `cooling_control`, `nvs_store`, `web_server`, `wifi_manager`, `hardware_map`, `session`, `relay_safe_init`, `relays_hardware`, `sensors_hardware`, `browser_ui`) to its layer, Thai title, English title, execution context (Core, Priority, Stack), primary responsibilities, inbound/outbound streams, key C APIs, and safety flags.
  - `connections`: Array of connection links with `id`, `from`, `to`, `label`, `category` (`telemetry`, `control`, `nvs`, `network`, `safety`), `scenarios` array, and flow description.
  - `scenarios`: Array of 4 scenarios (`overview`, `boot`, `telemetry`, `staging_rollback`) with step-by-step descriptions and active connection IDs.

- [ ] **Step 1: Write test for architecture data integrity**

```javascript
// scratch/test_map_data.js
const assert = require('assert');
const { SYSTEM_MAP_DATA } = require('./map_data.js');

// 1. Verify all 4 layers exist
assert.strictEqual(SYSTEM_MAP_DATA.layers.length, 4, "Must have 4 layers");

// 2. Verify essential components exist
const requiredComponents = [
  'app_main', 'telemetry_hub', 'pump_control', 'cooling_control',
  'nvs_store', 'web_server', 'wifi_manager', 'hardware_map',
  'session', 'relay_safe_init'
];
requiredComponents.forEach(comp => {
  assert.ok(SYSTEM_MAP_DATA.components[comp], `Component ${comp} must be defined`);
  assert.ok(SYSTEM_MAP_DATA.components[comp].thaiName, `Component ${comp} must have Thai name`);
  assert.ok(SYSTEM_MAP_DATA.components[comp].desc, `Component ${comp} must have Thai description`);
});

// 3. Verify connections reference valid components
SYSTEM_MAP_DATA.connections.forEach(conn => {
  assert.ok(SYSTEM_MAP_DATA.components[conn.from], `Connection from "${conn.from}" is invalid`);
  assert.ok(SYSTEM_MAP_DATA.components[conn.to], `Connection to "${conn.to}" is invalid`);
  assert.ok(conn.category, `Connection must have a category`);
});

// 4. Verify 4 scenarios exist
assert.strictEqual(SYSTEM_MAP_DATA.scenarios.length, 4, "Must define 4 scenarios");
console.log("Architecture data integrity test passed successfully!");
```

- [ ] **Step 2: Run test to verify it fails**

Run: `node scratch/test_map_data.js`  
Expected: FAIL (Cannot find module `./map_data.js`)

- [ ] **Step 3: Implement `scratch/map_data.js`**

Implement complete dictionary with detailed Thai descriptions, C signatures, FreeRTOS specs, and connections matching `firmware/` codebase.

- [ ] **Step 4: Run test to verify it passes**

Run: `node scratch/test_map_data.js`  
Expected: PASS ("Architecture data integrity test passed successfully!")

- [ ] **Step 5: Commit**

```bash
git add scratch/map_data.js scratch/test_map_data.js
git commit -m "feat: define firmware architecture data dictionary and verification test"
```

---

### Task 2: Standalone Shell, Modern Dark Theme & SVG Layout System

**Files:**
- Create: `c:\Users\Copter\OneDrive\Desktop\fishpump-online\map.html`
- Test: `scratch/test_html_shell.js`

**Interfaces:**
- Consumes: `SYSTEM_MAP_DATA` from Task 1.
- Produces: Base `map.html` featuring:
  - Responsive header with system status indicators, scenario picker tabs, play/pause animation toggle, and reset view.
  - Interactive SVG canvas with 4 architectural layer bands (`#b0-hardware`, `#b1-tasks`, `#b2-bus`, `#b3-network`).
  - Coordinated component node cards rendered inside SVG `<g class="component-node">` with icons, titles, badges, and status pills.
  - Responsive styling with CSS variables, backdrop blur, and modern typography using system fonts.

- [ ] **Step 1: Write verification script for HTML layout and SVG nodes**

```javascript
// scratch/test_html_shell.js
const fs = require('fs');
const path = require('path');
const assert = require('assert');

const htmlPath = path.resolve(__dirname, '../map.html');
assert.ok(fs.existsSync(htmlPath), "map.html must exist at repository root");

const content = fs.readFileSync(htmlPath, 'utf8');

// Check self-contained criteria (no external http/https stylesheets or scripts)
assert.ok(!content.includes('http://') && !content.includes('https://'), "Must have zero external web requests");

// Check core DOM elements
assert.ok(content.includes('id="scenario-bar"'), "Must contain scenario switcher");
assert.ok(content.includes('id="architecture-svg"'), "Must contain main SVG canvas");
assert.ok(content.includes('id="inspector-drawer"'), "Must contain inspector drawer");

// Check all components are rendered as SVG groups
['app_main', 'telemetry_hub', 'pump_control', 'cooling_control', 'nvs_store', 'web_server'].forEach(comp => {
  assert.ok(content.includes(`data-component-id="${comp}"`), `Node for ${comp} must be present in SVG`);
});

console.log("HTML shell and SVG nodes test passed!");
```

- [ ] **Step 2: Run test to verify it fails**

Run: `node scratch/test_html_shell.js`  
Expected: FAIL (`map.html` not found)

- [ ] **Step 3: Implement base `map.html`**

Construct `map.html` with:
- Top header with Title, Status Chips (ESP32-S3, 240MHz, 16MB Flash, FreeRTOS SMP), and Scenario switch tabs.
- SVG Canvas (with defined viewBox, gradient definitions, filter glows, 4 layer background rects, and positioned component cards).
- CSS styles embedded in `<style>`.

- [ ] **Step 4: Run test to verify it passes**

Run: `node scratch/test_html_shell.js`  
Expected: PASS ("HTML shell and SVG nodes test passed!")

- [ ] **Step 5: Commit**

```bash
git add map.html scratch/test_html_shell.js
git commit -m "feat: scaffold standalone map.html with SVG canvas and dark theme"
```

---

### Task 3: Animated SVG Paths, Particle Motion & Dynamic Scenario Engine

**Files:**
- Modify: `c:\Users\Copter\OneDrive\Desktop\fishpump-online\map.html`
- Test: `scratch/test_animations.js`

**Interfaces:**
- Consumes: `connections` and `scenarios` from `SYSTEM_MAP_DATA`.
- Produces:
  - SVG `<path>` elements connecting source and destination nodes using smooth cubic/quadratic bezier curves.
  - Animated dash-arrays and glowing particles travelling along paths in the real data flow direction.
  - Scenario switcher JavaScript engine: switching tabs toggles classes, updates step-by-step guidance banner, dims inactive paths, and speeds up/highlights active scenario paths.
  - Animation control (Play/Pause, speed toggle).

- [ ] **Step 1: Write test verifying SVG connection paths and scenario bindings**

```javascript
// scratch/test_animations.js
const fs = require('fs');
const path = require('path');
const assert = require('assert');

const content = fs.readFileSync(path.resolve(__dirname, '../map.html'), 'utf8');

// Check SVG paths
assert.ok(content.includes('class="flow-path'), "Must have SVG flow paths");
assert.ok(content.includes('stroke-dasharray'), "Must define dasharray for particle motion");

// Check scenario engine bindings
assert.ok(content.includes('switchScenario('), "Must define switchScenario function");
assert.ok(content.includes('boot'), "Must support boot scenario");
assert.ok(content.includes('telemetry'), "Must support telemetry scenario");
assert.ok(content.includes('staging_rollback'), "Must support staging rollback scenario");

console.log("Animation and scenario test passed!");
```

- [ ] **Step 2: Run test to verify it fails**

Run: `node scratch/test_animations.js`  
Expected: FAIL

- [ ] **Step 3: Implement SVG paths, particle animations, and scenario engine**

Add SVG path generation, CSS animations for `.flow-particle` and `.flow-dash`, and the JavaScript event handlers for scenario buttons.

- [ ] **Step 4: Run test to verify it passes**

Run: `node scratch/test_animations.js`  
Expected: PASS ("Animation and scenario test passed!")

- [ ] **Step 5: Commit**

```bash
git add map.html scratch/test_animations.js
git commit -m "feat: implement animated SVG flow paths and scenario switching engine"
```

---

### Task 4: Interactive Component Inspector Drawer & Focus Mode

**Files:**
- Modify: `c:\Users\Copter\OneDrive\Desktop\fishpump-online\map.html`
- Test: `scratch/test_inspector.js`

**Interfaces:**
- Consumes: Node click events, component data dictionary.
- Produces:
  - Slide-over drawer sliding in from the right edge with smooth CSS transition.
  - Inbound and Outbound relationship list with clickable links to jump to connected components.
  - Focus Mode: dims all unrelated components and highlights only connected inbound/outbound arrows.
  - Detailed sections in Thai for Role, FreeRTOS Specs, C API list, and Safety features.
  - Close button (`ESC` key support or overlay click).

- [ ] **Step 1: Write test verifying inspector drawer functionality and focus mode**

```javascript
// scratch/test_inspector.js
const fs = require('fs');
const path = require('path');
const assert = require('assert');

const content = fs.readFileSync(path.resolve(__dirname, '../map.html'), 'utf8');

// Check Inspector DOM structure
assert.ok(content.includes('id="inspector-title"'), "Must have inspector title element");
assert.ok(content.includes('id="inspector-rtos"'), "Must have RTOS context block");
assert.ok(content.includes('id="inspector-apis"'), "Must have C API block");
assert.ok(content.includes('id="inspector-inbound"'), "Must have inbound data block");
assert.ok(content.includes('id="inspector-outbound"'), "Must have outbound data block");
assert.ok(content.includes('openInspector('), "Must have openInspector function");
assert.ok(content.includes('closeInspector('), "Must have closeInspector function");

console.log("Inspector drawer test passed!");
```

- [ ] **Step 2: Run test to verify it fails**

Run: `node scratch/test_inspector.js`  
Expected: FAIL

- [ ] **Step 3: Implement inspector drawer, drawer rendering, and focus mode**

Add DOM elements, drawer animation styles, click handlers on SVG nodes, keyboard listeners, and breadcrumb navigation.

- [ ] **Step 4: Run test to verify it passes**

Run: `node scratch/test_inspector.js`  
Expected: PASS ("Inspector drawer test passed!")

- [ ] **Step 5: Commit**

```bash
git add map.html scratch/test_inspector.js
git commit -m "feat: add interactive component inspector drawer and focus mode"
```

---

### Task 5: End-to-End Verification, Polish & Delivery

**Files:**
- Review: `c:\Users\Copter\OneDrive\Desktop\fishpump-online\map.html`
- Clean up: `scratch/` files if needed.

**Interfaces:**
- End-to-end verification of `map.html`:
  - Valid HTML5 syntax.
  - Zero external resources (fonts, scripts, images).
  - All 10+ components clickable with verified content.
  - Responsive design from 1024px to 4K displays.
  - Smooth animation performance (60 FPS CSS transforms/stroke-dashoffset).

- [ ] **Step 1: Execute full verification script**

Run: `node -e "const fs = require('fs'); const content = fs.readFileSync('../map.html', 'utf8'); console.log('File size: ' + (content.length/1024).toFixed(1) + ' KB');"`

- [ ] **Step 2: Commit final deliverable**

```bash
git add map.html
git commit -m "feat: complete interactive system architecture and data flow map (map.html)"
```
