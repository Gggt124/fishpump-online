// scratch/test_workflow_doc.js
const fs = require('fs');
const path = require('path');
const assert = require('assert');

// Check in root directory
const docPath = path.resolve(__dirname, '../../workflow.doc');
assert.ok(fs.existsSync(docPath), "workflow.doc must exist at repository root: " + docPath);

// Read file synchronously with shared-read access
let content;
try {
  content = fs.readFileSync(docPath, 'utf8');
} catch (e) {
  // If file is locked for writing by Word, open with read-share flag
  const fd = fs.openSync(docPath, 'r');
  const stats = fs.fstatSync(fd);
  const buffer = Buffer.alloc(stats.size);
  fs.readSync(fd, buffer, 0, stats.size, 0);
  fs.closeSync(fd);
  content = buffer.toString('utf8');
}

// Check major chapters and components
const requiredSections = [
  'System Overview', 'Physical Pin Mapping', 'FreeRTOS Tasks',
  'pump_control', 'cooling_control', 'telemetry_hub', 'nvs_store',
  'web_server', 'wifi_manager', 'hardware_map', 'relay_safe_init',
  'Zero-Brick Staging', 'Server-Sent Events'
];
requiredSections.forEach(section => {
  assert.ok(content.includes(section), `workflow.doc must contain section "${section}"`);
});

// Check file size (must be comprehensive, > 15 KB)
assert.ok(content.length > 15000, `workflow.doc must be comprehensive (>15KB, currently ${content.length})`);
console.log("workflow.doc validation test passed! File size: " + (content.length/1024).toFixed(1) + " KB");
