// scratch/test_map_data.js
const assert = require('assert');
const { SYSTEM_MAP_DATA } = require('./map_data.js');

// 1. Verify all 4 layers exist
assert.strictEqual(SYSTEM_MAP_DATA.layers.length, 4, "Must have 4 layers");

// 2. Verify essential components exist
const requiredComponents = [
  'app_main', 'telemetry_hub', 'pump_control', 'cooling_control',
  'nvs_store', 'web_server', 'wifi_manager', 'hardware_map',
  'session', 'relay_safe_init', 'hardware_relays', 'hardware_sensors'
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
console.log("Architecture data integrity test passed successfully! Total components: " + Object.keys(SYSTEM_MAP_DATA.components).length + ", Connections: " + SYSTEM_MAP_DATA.connections.length);
