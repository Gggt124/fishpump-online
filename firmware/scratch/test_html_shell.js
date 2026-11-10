// scratch/test_html_shell.js
const fs = require('fs');
const path = require('path');
const assert = require('assert');

const htmlPath = path.resolve(__dirname, '../../map.html');
assert.ok(fs.existsSync(htmlPath), "map.html must exist at repository root: " + htmlPath);

const content = fs.readFileSync(htmlPath, 'utf8');

// Check self-contained criteria (no external scripts, links, fonts or images loaded over web)
assert.ok(!content.match(/<script[^>]+src=["']https?:/i), "Must not load external scripts over HTTP/HTTPS");
assert.ok(!content.match(/<link[^>]+href=["']https?:/i), "Must not load external stylesheets over HTTP/HTTPS");
assert.ok(!content.match(/url\(["']?https?:/i), "Must not load external fonts or images over HTTP/HTTPS");

// Check core DOM elements
assert.ok(content.includes('id="scenario-bar"'), "Must contain scenario switcher");
assert.ok(content.includes('id="architecture-svg"'), "Must contain main SVG canvas");
assert.ok(content.includes('id="inspector-drawer"'), "Must contain inspector drawer");

// Check all essential components are present in data model
['app_main', 'telemetry_hub', 'pump_control', 'cooling_control', 'nvs_store', 'web_server'].forEach(comp => {
  assert.ok(content.includes(`id: "${comp}"`), `Node definition for ${comp} must be present`);
});

console.log("HTML shell and SVG nodes test passed! File size: " + (content.length/1024).toFixed(1) + " KB");
