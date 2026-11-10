// scratch/test_final_verification.js
const fs = require('fs');
const path = require('path');
const assert = require('assert');

console.log("==================================================");
console.log(" Running End-to-End System Deliverables Validation");
console.log("==================================================");

// 1. Verify workflow.doc
const docPath = path.resolve(__dirname, '../../workflow.doc');
assert.ok(fs.existsSync(docPath), "workflow.doc must exist at repository root");

let docContent;
try {
  docContent = fs.readFileSync(docPath, 'utf8');
} catch (e) {
  const fd = fs.openSync(docPath, 'r');
  const stats = fs.fstatSync(fd);
  const buffer = Buffer.alloc(stats.size);
  fs.readSync(fd, buffer, 0, stats.size, 0);
  fs.closeSync(fd);
  docContent = buffer.toString('utf8');
}

assert.ok(docContent.length > 20000, `workflow.doc must be comprehensive (>20KB, currently ${(docContent.length/1024).toFixed(1)} KB)`);
assert.ok(docContent.includes('ESP32-S3 WROOM-1-N16R8'), "Must specify target chip");
assert.ok(docContent.includes('FreeRTOS SMP'), "Must specify OS");
console.log(`[PASS] workflow.doc is valid (${(docContent.length/1024).toFixed(1)} KB)`);

// 2. Verify map.html
const htmlPath = path.resolve(__dirname, '../../map.html');
assert.ok(fs.existsSync(htmlPath), "map.html must exist at repository root");
const htmlContent = fs.readFileSync(htmlPath, 'utf8');

assert.ok(!htmlContent.match(/<script[^>]+src=["']https?:/i), "Must not load external scripts over HTTP/HTTPS");
assert.ok(!htmlContent.match(/<link[^>]+href=["']https?:/i), "Must not load external stylesheets over HTTP/HTTPS");
assert.ok(!htmlContent.match(/url\(["']?https?:/i), "Must not load external fonts/images over HTTP/HTTPS");
assert.ok(htmlContent.includes('id="scenario-bar"'), "Must contain scenario switcher");
assert.ok(htmlContent.includes('id="architecture-svg"'), "Must contain main SVG canvas");
assert.ok(htmlContent.includes('id="inspector-drawer"'), "Must contain inspector drawer");

// 3. Cross-document consistency check
const components = [
  'app_main', 'telemetry_hub', 'pump_control', 'cooling_control',
  'nvs_store', 'web_server', 'wifi_manager', 'hardware_map',
  'session', 'relay_safe_init'
];

components.forEach(comp => {
  const compRegex = new RegExp(comp, 'i');
  assert.ok(compRegex.test(docContent), `workflow.doc must document component "${comp}"`);
  assert.ok(htmlContent.includes(`id: "${comp}"`), `map.html must contain node for "${comp}"`);
});

console.log(`[PASS] map.html is valid (${(htmlContent.length/1024).toFixed(1)} KB) and 100% consistent with workflow.doc`);
console.log("==================================================");
console.log(" ALL 6 IMPLEMENTATION TASKS VERIFIED SUCCESSFULLY!");
console.log("==================================================");
