// scratch/test_animations.js
const fs = require('fs');
const path = require('path');
const assert = require('assert');

const htmlPath = path.resolve(__dirname, '../../map.html');
const content = fs.readFileSync(htmlPath, 'utf8');

// Check SVG paths and motion classes
assert.ok(content.includes('.flow-path {') || content.includes('flow-path'), "Must define SVG flow paths in CSS and JS");
assert.ok(content.includes('stroke-dasharray: 8 12'), "Must define dasharray for particle motion");
assert.ok(content.includes('@keyframes flowMotion'), "Must define keyframe animation for flow motion");

// Check scenario engine bindings
assert.ok(content.includes('switchScenario('), "Must define switchScenario function");
assert.ok(content.includes('applyScenario('), "Must define applyScenario function");
assert.ok(content.includes('data-scenario="overview"'), "Must have overview scenario button");
assert.ok(content.includes('data-scenario="boot"'), "Must have boot scenario button");
assert.ok(content.includes('data-scenario="telemetry"'), "Must have telemetry scenario button");
assert.ok(content.includes('data-scenario="staging_rollback"'), "Must have staging_rollback scenario button");

// Check pause/play motion toggle
assert.ok(content.includes('toggleMotion()'), "Must have toggleMotion function");

console.log("Animation and scenario test passed!");
