"use strict";
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const {frameSettingsProtocol, frameFasProtocol, frameFasProfiles} = require("./webroot/protocol.js");
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

const splitRates = settings();
splitRates.fps = [30, 60, 90, 120, 144, 165];
splitRates.fallbackFps = [30, 45, 60, 90, 120, 130, 144, 165];
splitRates.games["com.tencent.tmgp.sgame"] = [60, 120];
const splitPacket = frameSettingsProtocol(splitRates, seed, sceneSeed), splitTable = view(splitPacket.table);
assert.equal(view(splitPacket.config).getUint32(140, true), 6);
assert.equal(splitTable.getUint32(32 + 192, true), 8);
assert.deepEqual(Array.from({length: 8}, (_, i) => splitTable.getUint32(32 + 196 + i * 4, true)), splitRates.fallbackFps);
const inheritedName = Object.keys(seed.relations.rules).find(name => !Object.hasOwn(splitRates.games, name));
assert.ok(inheritedName, "Fixture must include a relation-only game");
const inherited = rules(splitPacket.table).get(inheritedName);
assert.equal(inherited.data.getUint32(inherited.start + 192, true), 8);
assert.deepEqual(Array.from({length: 8}, (_, i) => inherited.data.getUint32(inherited.start + 196 + i * 4, true)), splitRates.fallbackFps);
const explicitSplit = rules(splitPacket.table).get("com.tencent.tmgp.sgame");
assert.equal(explicitSplit.data.getUint32(explicitSplit.start + 192, true), 2);
assert.equal(explicitSplit.data.getUint32(explicitSplit.start + 196, true), 60);
assert.equal(explicitSplit.data.getUint32(explicitSplit.start + 200, true), 120);
const inheritedLegacy = settings();
inheritedLegacy.fps = [60, 120];
assert.equal(view(frameSettingsProtocol(inheritedLegacy, seed, sceneSeed).table).getUint32(32 + 192, true), 2);
const sceneSplit = settings();
sceneSplit.sceneMode = "powersave";
sceneSplit.sceneBaseMode = "balance";
const separateScene = frameSettingsProtocol(sceneSplit, seed, sceneSeed);
assert.deepEqual(separateScene.config, packet.config, "Advanced base must preserve the requested legacy configuration");
const powersaveScene = settings(); powersaveScene.sceneMode = "powersave";
assert.deepEqual(separateScene.scene, frameSettingsProtocol(powersaveScene, seed, sceneSeed).scene);
assert.equal(view(separateScene.payload).getUint32(28, true), 1, "Separated Scene/base requires native table support");
assert.equal(view(packet.payload).getUint32(28, true), 0, "Legacy defaults must not acquire a new requirement");

for (const modify of [
  x => { x.fps = [60, 60]; }, x => { x.fps = [130, 45]; },
  x => { x.fps = [241]; }, x => { x.fps = []; },
  x => { x.fallbackFps = []; }, x => { x.fallbackFps = null; },
  x => { x.fallbackFps = ""; }, x => { x.fallbackFps = [60, 60]; },
  x => { x.fallbackFps = [120, 60]; }, x => { x.fallbackFps = [241]; },
  x => { x.sceneMode = "missing"; }, x => { x.cpu = "yes"; },
  x => { x.sceneBaseMode = "missing"; }, x => { x.sceneBaseMode = ""; },
  x => { x.sceneBaseMode = null; },
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
const legacyMargin = {...settings(), fasMode: "fast", fasMarginFps: {base: 2.1, 165: 0}};
const legacyProfiles = frameFasProfiles(legacyMargin);
assert.deepEqual(legacyProfiles, {powersave: null, balance: null, performance: null, fast: {base: 2.1, 165: 0}});
assert.deepEqual(frameFasProfiles({fasMarginFps: 0}), {powersave: null, balance: 0, performance: null, fast: null});
legacyProfiles.fast.base = 9;
assert.equal(legacyMargin.fasMarginFps.base, 2.1, "Profiles must be safe for the UI to edit");
const legacyPacket = frameSettingsProtocol(legacyMargin, seed, sceneSeed);
assert.deepEqual(legacyPacket.settings, legacyMargin, "An unrelated save retains the legacy settings shape");
assert.deepEqual(legacyPacket.fas, frameFasProtocol({fasMode: "fast", fasMarginFps: {base: 2.1, 165: 0}}).blob);

const bank = {balance: 0, performance: null, fast: {base: 2.5, 120: 0, 165: 0.125}};
const normalizedBank = {powersave: null, ...bank};
const bankInput = {...settings(), fasMode: "fast", fasProfiles: bank};
const bankPacket = frameSettingsProtocol(bankInput, seed, sceneSeed);
assert.deepEqual(bankPacket.settings, {...bankInput, fasProfiles: normalizedBank});
assert.equal(Object.hasOwn(bank, "powersave"), false);
for (const key of ["config", "table", "scene", "aux"])
  assert.deepEqual(bankPacket[key], packet[key], `Profile bank cannot change ${key} bytes`);
const {payload: bankPayload} = bankPacket, bankView = view(bankPayload);
const bankJson = bankPayload.subarray(32, 32 + bankView.getUint32(4, true));
assert.deepEqual(JSON.parse(new TextDecoder().decode(bankJson)), bankPacket.settings);
assert.equal(bankView.getUint32(0, true), header.getUint32(0, true));
assert.equal(bankView.getUint32(20, true), 80);
const auxSeed = JSON.parse(fs.readFileSync(path.join(__dirname, "defaults/scene-aux-seed.json"), "utf8"));
const auxInput = {...settings(), sceneAux: true, sceneGpuLock: true,
  sceneApps: {"com.example.private": {mode: "performance", category: "Camera"}}};
const auxBefore = frameSettingsProtocol(auxInput, seed, sceneSeed, auxSeed);
const auxBank = frameSettingsProtocol({...auxInput, fasProfiles: bank}, seed, sceneSeed, auxSeed);
for (const key of ["config", "table", "scene", "aux"])
  assert.deepEqual(auxBank[key], auxBefore[key], `Profile bank cannot change FPS3 ${key} bytes`);
assert.deepEqual(auxBank.settings, {...auxBefore.settings, fasProfiles: normalizedBank});
assert.deepEqual(auxBank.payload.subarray(8, 40), auxBefore.payload.subarray(8, 40));
assert.equal(view(auxBank.payload).getUint32(0, true), 0x46505333);
for (const [mode, base, overrides] of [
  ["powersave", 3000, []], ["balance", 0, []], ["performance", 300, []],
  ["fast", 2500, [[120, 0], [165, 125]]],
]) {
  const tuned = frameFasProtocol({...bankInput, fasMode: mode});
  assert.equal(tuned.base, base);
  assert.deepEqual(tuned.overrides, overrides);
}
assert.equal(frameFasProtocol({fasProfiles: {balance: null}}).base, 1000);
for (const settings of [
  {fasProfiles: null}, {fasProfiles: false}, {fasProfiles: []},
  {fasProfiles: {unknown: null}}, {fasProfiles: JSON.parse('{"__proto__":null}')},
  {fasProfiles: {fast: false}}, {fasProfiles: {fast: "0"}}, {fasProfiles: {fast: {165: 2}}},
  {fasProfiles: {fast: {base: 0, 14: 1}}}, {fasProfiles: {fast: {base: Infinity}}},
  {fasProfiles: {fast: {base: 1, 60: 56.001}}}, {fasMode: "missing", fasProfiles: {}},
  {fasMarginFps: 0, fasProfiles: {}}, {fasMarginFps: null, fasProfiles: {balance: null}},
]) {
  assert.throws(() => frameFasProfiles(settings));
  assert.throws(() => frameFasProtocol(settings));
}
console.log("FAS protocol: four independent modes, legacy default, decimal margins, overrides and invalid ranges passed");

for (const value of [null, [], false, {unknown:null}, {fast:-1}, {fast:3.001},
  {fast:0.0001}, {fast:true}, {fast:"0"}, {fast:{}}, JSON.parse('{"__proto__":0}')])
  assert.throws(() => frameFasProtocol({fasUtilLimits:value}));
for (const mode of ["powersave", "balance", "performance", "fast"]) {
  for (const limit of [0, 0.001, 0.501, 3]) {
    const result = frameFasProtocol({fasMode:mode,fasUtilLimits:{[mode]:limit}});
    assert.equal(result.blob.length, 88);
    const view = new DataView(result.blob.buffer);
    assert.equal(view.getUint32(80,true), Math.round(limit*1000));
    assert.equal(view.getUint32(84,true), 0);
  }
}
assert.equal(frameFasProtocol({fasUtilLimits:{balance:null,fast:0}}).blob.length,80);
console.log("FAS adaptive limits: all banks validated, zero preserved, exact extended ABI and old default passed");

if (process.argv[2] === "--defaults") {
  fs.writeFileSync(path.join(__dirname, "defaults/config.bin"), packet.config);
  fs.writeFileSync(path.join(__dirname, "defaults/games.bin"), packet.table);
  fs.writeFileSync(path.join(__dirname, "defaults/scene.bin"), packet.scene);
  fs.writeFileSync(path.join(__dirname, "defaults/fas.bin"), packet.fas);
  fs.writeFileSync(path.join(__dirname, "defaults/aux.bin"), packet.aux);
  fs.writeFileSync(path.join(__dirname, "defaults/aux-required"), "0\n");
  fs.writeFileSync(path.join(__dirname, "defaults/scene-required"), "0\n");
}
