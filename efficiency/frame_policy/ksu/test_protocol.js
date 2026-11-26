"use strict";
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const {frameSettingsProtocol, frameFasProtocol} = require("./webroot/protocol.js");
for (const name of ["enabled", "libgui.sha256", "seed.json", "settings.json"])
  assert.ok(!fs.readFileSync(path.join(__dirname, "defaults", name)).includes(13), `${name} must use LF`);
const seed = JSON.parse(fs.readFileSync(path.join(__dirname, "defaults/seed.json"), "utf8"));
const sceneSeed = JSON.parse(fs.readFileSync(path.join(__dirname, "defaults/scene-seed.json"), "utf8"));
const original = JSON.parse(fs.readFileSync(path.join(__dirname, "defaults/settings.json"), "utf8"));
const settings = () => structuredClone(original);
const packet = frameSettingsProtocol(settings(), seed, sceneSeed);
const view = bytes => new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
const cfg = view(packet.config), table = view(packet.table), header = view(packet.payload);
assert.equal(packet.config.length, 592);
assert.equal(packet.table.length, 7556);
assert.equal(cfg.getUint32(0, true), 4);
assert.equal(cfg.getUint32(4, true), 1);
assert.equal(cfg.getUint32(12, true), 165);
assert.equal(cfg.getUint32(140, true), 8);
assert.deepEqual(Array.from({length: 8}, (_, i) => cfg.getUint32(560 + i * 4, true)), original.fps);
assert.equal(header.getUint32(8, true), 592);
assert.equal(header.getUint32(12, true), 7556);
assert.equal(header.getUint32(20, true), 80);
assert.equal(header.getUint32(0, true), 0x46505332);
assert.equal(header.getUint32(16, true), 14384);
assert.equal(header.getUint32(24, true), 0);
assert.equal(header.getUint32(28, true), 0);
assert.deepEqual(packet.payload.subarray(-80), packet.fas);
assert.deepEqual(packet.payload.subarray(-14384 - 80, -80), packet.scene);
assert.deepEqual(JSON.parse(new TextDecoder().decode(packet.payload.subarray(32, 32 + header.getUint32(4, true)))), original);
assert.equal(table.getUint32(0, true), 1);
assert.equal(table.getUint32(8, true), 2);
assert.equal(table.getInt32(32 + 136, true), -150000);

function rules(bytes) {
  const data = view(bytes), result = new Map();
  for (let i = 1; i <= data.getUint32(4, true); i++) {
    const start = 32 + i * 228;
    const name = new TextDecoder().decode(bytes.subarray(start, start + 128)).split("\0")[0];
    result.set(name, {start, data});
  }
  return result;
}
const wz = rules(packet.table).get("com.tencent.tmgp.sgame");
assert.ok(wz);
assert.equal(wz.data.getUint32(wz.start + 128, true), 2);
assert.equal(wz.data.getUint32(wz.start + 132, true), 1);
assert.equal(wz.data.getInt32(wz.start + 136, true), 0);
assert.equal(wz.data.getInt32(wz.start + 140, true), 0);
assert.equal(wz.data.getUint32(wz.start + 192, true), 8);

const explicit = settings();
explicit.enabled = true; explicit.observe = true; explicit.games["com.tencent.tmgp.sgame"] = [45, 130];
const changed = frameSettingsProtocol(explicit, seed, sceneSeed), changedWz = rules(changed.table).get("com.tencent.tmgp.sgame");
assert.equal(view(changed.config).getUint32(4, true), 5);
assert.equal(view(changed.payload).getUint32(24, true), 1);
assert.equal(changedWz.data.getUint32(changedWz.start + 192, true), 2);
assert.equal(changedWz.data.getUint32(changedWz.start + 196, true), 45);
assert.equal(changedWz.data.getUint32(changedWz.start + 200, true), 130);

const prototypeNames = settings();
prototypeNames.games = JSON.parse('{"__proto__":[45],"constructor":[130]}');
const prototypes = rules(frameSettingsProtocol(prototypeNames, seed, sceneSeed).table);
assert.equal(prototypes.get("__proto__").data.getUint32(prototypes.get("__proto__").start + 196, true), 45);
assert.equal(prototypes.get("constructor").data.getUint32(prototypes.get("constructor").start + 196, true), 130);

const advanced = settings();
advanced.sceneMode = "fast";
advanced.sceneApps = {"com.oplus.camera": {mode: "performance", category: "Camera"}};
const advancedPacket = frameSettingsProtocol(advanced, seed, sceneSeed);
assert.equal(view(advancedPacket.payload).getUint32(28, true), 1);
assert.equal(view(advancedPacket.scene).getUint32(4, true), 1);

for (const modify of [
  x => { x.fps = [60, 60]; }, x => { x.fps = [130, 45]; },
  x => { x.fps = [241]; }, x => { x.fps = []; },
  x => { x.sceneMode = "missing"; }, x => { x.cpu = "yes"; },
  x => { x.gameCpuMHz[1][1] = 4321; }, x => { x.gameCpuMHz[0] = [2000, 1000]; },
  x => { x.games["invalid;name"] = [60]; }, x => { x.unrecognized = true; },
  x => { x.enabled = true; x.cpu = false; x.gpu = false; },
  x => { x.fasMode = "missing"; }, x => { x.fasMode = ""; },
  x => { x.fasMode = null; }, x => { x.fasMarginFps = ""; },
  x => { x.fasMarginFps = -1; }, x => { x.fasMarginFps = 30.001; },
  x => { x.fasMarginFps = NaN; }, x => { x.fasMarginFps = Infinity; },
  x => { x.fasMarginFps = 0.0001; }, x => { x.fasMarginFps = {}; },
  x => { x.fasMarginFps = {base: 1, "060": 1}; },
  x => { x.fasMarginFps = {base: 1, "60.0": 1}; },
  x => { x.fasMarginFps = {base: 1, "14": 1}; },
  x => { x.fasMarginFps = {base: 1, "240": 236.001}; },
  x => { x.fasMarginFps = {base: 1, ...Object.fromEntries(Array.from({length: 9}, (_, i) => [String(15 + i), 0]))}; },
]) {
  const input = settings(); modify(input);
  assert.throws(() => frameSettingsProtocol(input, seed, sceneSeed));
}
console.log("Configuration protocol: ABI, FPS overrides, CPU relations, prototype names and validation passed");

for (const [mode, index, base] of [["powersave", 0, 3000], ["balance", 1, 1000], ["performance", 2, 300], ["fast", 3, 0]]) {
  const input = settings(); input.fasMode = mode;
  const result = frameSettingsProtocol(input, seed, sceneSeed), fas = view(result.fas);
  assert.equal(result.fas.length, 80);
  assert.equal(fas.getUint32(0, true), 1);
  assert.equal(fas.getUint32(4, true), index);
  assert.equal(fas.getUint32(8, true), base);
  assert.deepEqual(result.config, packet.config, "FAS mode must not change Scene/config");
  assert.deepEqual(result.table, packet.table, "FAS mode must not change game rules");
  assert.deepEqual(result.scene, packet.scene, "FAS mode must not change Scene table");
}
const legacy = settings(); delete legacy.fasMode;
const upgraded = frameSettingsProtocol(legacy, seed, sceneSeed);
assert.equal(upgraded.settings.fasMode, "balance");
assert.deepEqual(upgraded.fas, packet.fas);
assert.equal(legacy.fasMode, undefined, "Encoding must not mutate legacy settings");
const custom = frameFasProtocol({fasMode: "fast", fasMarginFps: {base: 1.001, "165": 0.3, "60": 0}});
assert.equal(custom.base, 1001);
assert.deepEqual(custom.overrides, [[60, 0], [165, 300]]);
assert.equal(view(custom.blob).getUint32(4, true), 3);
assert.equal(frameFasProtocol({fasMarginFps: 0.001}).base, 1);
assert.equal(frameFasProtocol({fasMarginFps: {base: 30, "240": 236}}).overrides[0][1], 236000);
console.log("FAS protocol: four independent modes, legacy default, decimal margins, overrides and invalid ranges passed");

if (process.argv[2] === "--defaults") {
  fs.writeFileSync(path.join(__dirname, "defaults/config.bin"), packet.config);
  fs.writeFileSync(path.join(__dirname, "defaults/games.bin"), packet.table);
  fs.writeFileSync(path.join(__dirname, "defaults/scene.bin"), packet.scene);
  fs.writeFileSync(path.join(__dirname, "defaults/fas.bin"), packet.fas);
  fs.writeFileSync(path.join(__dirname, "defaults/scene-required"), "0\n");
}
