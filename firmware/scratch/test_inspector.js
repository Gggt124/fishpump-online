// scratch/test_inspector.js
const fs = require('fs');
const path = require('path');
const assert = require('assert');

const htmlPath = path.resolve(__dirname, '../../map.html');
const content = fs.readFileSync(htmlPath, 'utf8');

// Check Inspector DOM structure
assert.ok(content.includes('id="inspector-title"'), "Must have inspector title element");
assert.ok(content.includes('id="inspector-rtos"'), "Must have RTOS context block");
assert.ok(content.includes('id="inspector-safety"'), "Must have safety badges block");
assert.ok(content.includes('id="inspector-inbound"'), "Must have inbound data block");
assert.ok(content.includes('id="inspector-outbound"'), "Must have outbound data block");
assert.ok(content.includes('id="inspector-apis"'), "Must have C API block");

// Check JS handler functions
assert.ok(content.includes('selectComponent('), "Must have selectComponent function");
assert.ok(content.includes('closeInspector()'), "Must have closeInspector function");
assert.ok(content.includes('node-selected'), "Must have node-selected CSS class for active selection");
assert.ok(content.includes('flow-highlighted'), "Must have flow-highlighted CSS class for focus mode");

console.log("Inspector drawer and focus mode test passed!");
