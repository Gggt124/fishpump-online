# FishPump ESP32-S3 Firmware Architecture & Data Flow Map Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Deliver a complete two-part system architecture package:
1. **Master Architecture & System Workflow Document (`workflow.doc`):** Definitive reference document recording complete system architecture, FreeRTOS tasks, component interactions, state machines, data flow pipelines, NVS storage schemas, and safety workflows.
2. **Interactive Visual Architecture & Data Flow Map (`map.html`):** Standalone, interactive HTML/SVG visualization with particle motion animations showing real-time data flows, scenarios, and component inspection.

**Architecture:** 
- `workflow.doc`: Clean, Microsoft Word-compatible structured document with rich typography, summary tables, component matrices, state machine specifications, data flow diagrams, and C API references.
- `map.html`: Single-file web application containing inline semantic HTML5, modern CSS3 (custom properties, responsive glassmorphism dark theme, particle motion animations), vector SVG canvas (dynamic path rendering, glow filters, layer visual bands), and modular vanilla JavaScript (scenario engine, inspector drawer controller, focus highlighter). Zero external dependencies.

**Tech Stack:** Microsoft Word HTML/Document Format, HTML5, Embedded SVG, CSS3, Vanilla ES6+ JavaScript.

## Global Constraints

- **File Paths:**
  - `workflow.doc` at repository root: `c:\Users\Copter\OneDrive\Desktop\fishpump-online\workflow.doc`
  - `map.html` at repository root: `c:\Users\Copter\OneDrive\Desktop\fishpump-online\map.html`
- **Zero External Dependencies:** No external fonts, icons, CSS frameworks, or JS scripts loaded over HTTP/HTTPS. Must work completely offline.
- **Language Convention:** Explanations and descriptions in Thai (ภาษาไทย); C symbols, FreeRTOS types, and technical acronyms in English.
- **Strict Architecture Accuracy:** Must accurately reflect the real C codebase in `firmware/` (`app_main.c`, `telemetry_hub`, `pump_control`, `cooling_control`, `nvs_store`, `web_server`, `wifi_manager`, `hardware_map`, `session`, `relay_safe_init`).

---

### Task 1: Generate Master Reference Document (`workflow.doc`)

**Files:**
- Create: `c:\Users\Copter\OneDrive\Desktop\fishpump-online\workflow.doc`
- Test: `scratch/test_workflow_doc.js`

**Interfaces:**
- Produces: `workflow.doc` containing:
  - Chapter 1: System Overview, Hardware Architecture & Pin Allocation
  - Chapter 2: FreeRTOS Task Matrix & Core CPU Allocation (Core 0/1, Priorities, Stack Sizes)
  - Chapter 3: State Machines & Control Logic (Pump Alternation, Wash Cycle, Cooling Thermostat)
  - Chapter 4: In-Memory Pub/Sub Telemetry Hub & SSE Real-Time Streaming
  - Chapter 5: NVS Storage Architecture & Zero-Brick Staging Rollback Safety
  - Chapter 6: Network Subsystem, Captive Portal & Web REST Gateway
  - Chapter 7: Hardware Recovery Operations & Crash Diagnostics
  - Chapter 8: Cross-Reference with `map.html` and Standard Operating Procedures

- [ ] **Step 1: Write test verifying `workflow.doc` completeness and structure**

```javascript
// scratch/test_workflow_doc.js
const fs = require('fs');
const path = require('path');
const assert = require('assert');

const docPath = path.resolve(__dirname, '../workflow.doc');
assert.ok(fs.existsSync(docPath), "workflow.doc must exist at repository root");

const content = fs.readFileSync(docPath, 'utf8');

// Check major chapters and components
const requiredSections = [
  'System Overview', 'Hardware Architecture', 'FreeRTOS Task Matrix',
  'pump_control', 'cooling_control', 'telemetry_hub', 'nvs_store',
  'web_server', 'wifi_manager', 'hardware_map', 'relay_safe_init',
  'Zero-Brick Staging', 'Server-Sent Events'
];
requiredSections.forEach(section => {
  assert.ok(content.includes(section), `workflow.doc must contain section "${section}"`);
});

// Check file size (must be comprehensive, > 15 KB)
assert.ok(content.length > 15000, `workflow.doc must be comprehensive (>15KB, currently ${content.length})`);
console.log("workflow.doc validation test passed!");
```

- [ ] **Step 2: Run test to verify it fails**

Run: `node scratch/test_workflow_doc.js`  
Expected: FAIL (`workflow.doc` not found)

- [ ] **Step 3: Implement `workflow.doc`**

Construct the comprehensive, cleanly formatted Word-compatible document covering all 8 chapters, tables, and architectural details in Thai with English technical terms.

- [ ] **Step 4: Run test to verify it passes**

Run: `node scratch/test_workflow_doc.js`  
Expected: PASS ("workflow.doc validation test passed!")

- [ ] **Step 5: Commit**

```bash
git add workflow.doc scratch/test_workflow_doc.js
git commit -m "docs: generate master architecture and system workflow reference document (workflow.doc)"
```

---

### Task 2: Comprehensive Architecture Data Dictionary for `map.html`

**Files:**
- Create: `scratch/map_data.js`
- Test: `scratch/test_map_data.js`

**Interfaces:**
- Consumes: Specifications defined in `workflow.doc`.
- Produces: `SYSTEM_MAP_DATA` object containing `layers`, `components`, `connections`, and `scenarios`.

- [ ] **Step 1: Write test for architecture data integrity**

```javascript
// scratch/test_map_data.js
const assert = require('assert');
const { SYSTEM_MAP_DATA } = require('./map_data.js');

assert.strictEqual(SYSTEM_MAP_DATA.layers.length, 4, "Must have 4 layers");
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
assert.strictEqual(SYSTEM_MAP_DATA.scenarios.length, 4, "Must define 4 scenarios");
console.log("Architecture data integrity test passed successfully!");
```

- [ ] **Step 2: Run test to verify it fails**

Run: `node scratch/test_map_data.js`  
Expected: FAIL

- [ ] **Step 3: Implement `scratch/map_data.js`**

Implement complete dictionary with detailed Thai descriptions, C signatures, FreeRTOS specs, and connections.

- [ ] **Step 4: Run test to verify it passes**

Run: `node scratch/test_map_data.js`  
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add scratch/map_data.js scratch/test_map_data.js
git commit -m "feat: define firmware architecture data dictionary and verification test"
```

---

### Task 3: Standalone Shell, Modern Dark Theme & SVG Layout System

**Files:**
- Create: `c:\Users\Copter\OneDrive\Desktop\fishpump-online\map.html`
- Test: `scratch/test_html_shell.js`

**Interfaces:**
- Consumes: `SYSTEM_MAP_DATA` from Task 2.
- Produces: Base `map.html` featuring responsive header, SVG canvas, layer bands, and component nodes.

- [ ] **Step 1: Write verification script for HTML layout and SVG nodes**

```javascript
// scratch/test_html_shell.js
const fs = require('fs');
const path = require('path');
const assert = require('assert');

const htmlPath = path.resolve(__dirname, '../map.html');
assert.ok(fs.existsSync(htmlPath), "map.html must exist at repository root");
const content = fs.readFileSync(htmlPath, 'utf8');

assert.ok(!content.includes('http://') && !content.includes('https://'), "Must have zero external web requests");
assert.ok(content.includes('id="scenario-bar"'), "Must contain scenario switcher");
assert.ok(content.includes('id="architecture-svg"'), "Must contain main SVG canvas");
assert.ok(content.includes('id="inspector-drawer"'), "Must contain inspector drawer");
console.log("HTML shell and SVG nodes test passed!");
```

- [ ] **Step 2: Run test to verify it fails**

Run: `node scratch/test_html_shell.js`  
Expected: FAIL

- [ ] **Step 3: Implement base `map.html`**

Construct `map.html` with SVG Canvas, layer rects, component node cards, and embedded CSS.

- [ ] **Step 4: Run test to verify it passes**

Run: `node scratch/test_html_shell.js`  
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add map.html scratch/test_html_shell.js
git commit -m "feat: scaffold standalone map.html with SVG canvas and dark theme"
```

---

### Task 4: Animated SVG Paths, Particle Motion & Dynamic Scenario Engine

**Files:**
- Modify: `c:\Users\Copter\OneDrive\Desktop\fishpump-online\map.html`
- Test: `scratch/test_animations.js`

**Interfaces:**
- Produces: SVG bezier paths, particle motion keyframe animations, and 4-scenario dynamic switcher.

- [ ] **Step 1: Write test verifying SVG connection paths and scenario bindings**
- [ ] **Step 2: Run test to verify it fails**
- [ ] **Step 3: Implement SVG paths, particle animations, and scenario engine**
- [ ] **Step 4: Run test to verify it passes**
- [ ] **Step 5: Commit**

```bash
git add map.html scratch/test_animations.js
git commit -m "feat: implement animated SVG flow paths and scenario switching engine"
```

---

### Task 5: Interactive Component Inspector Drawer & Focus Mode

**Files:**
- Modify: `c:\Users\Copter\OneDrive\Desktop\fishpump-online\map.html`
- Test: `scratch/test_inspector.js`

**Interfaces:**
- Produces: Slide-over drawer, Focus Mode highlighting, detailed C API and RTOS tables.

- [ ] **Step 1: Write test verifying inspector drawer functionality and focus mode**
- [ ] **Step 2: Run test to verify it fails**
- [ ] **Step 3: Implement inspector drawer, drawer rendering, and focus mode**
- [ ] **Step 4: Run test to verify it passes**
- [ ] **Step 5: Commit**

```bash
git add map.html scratch/test_inspector.js
git commit -m "feat: add interactive component inspector drawer and focus mode"
```

---

### Task 6: Final Verification, Cross-Document Consistency & Delivery

**Files:**
- Review: `c:\Users\Copter\OneDrive\Desktop\fishpump-online\workflow.doc`
- Review: `c:\Users\Copter\OneDrive\Desktop\fishpump-online\map.html`

- [ ] **Step 1: Run comprehensive verification script comparing workflow.doc and map.html**
- [ ] **Step 2: Commit final deliverable**

```bash
git add workflow.doc map.html
git commit -m "feat: complete master workflow document and interactive architecture map"
```
