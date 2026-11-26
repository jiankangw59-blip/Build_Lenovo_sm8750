"use strict";
const assert = require("node:assert/strict");
const fs = require("node:fs"), path = require("node:path");
const {spawnSync} = require("node:child_process");
const {test} = require("node:test");
const {frameSceneProtocol} = require("./webroot/scene_protocol.js");
const seed = JSON.parse(fs.readFileSync(path.join(__dirname, "defaults/scene-seed.json"), "utf8"));
const encode = (sceneMode = "balance", sceneApps = {}, source = seed) =>
  frameSceneProtocol({sceneMode, sceneApps}, source);
const uint = (table, offset) => new DataView(table.buffer, table.byteOffset, table.byteLength).getUint32(offset, true);
const cpu = (result, profile, policy) => 48 + profile * 336 + 16 + policy * 80;

test("Scene defaults preserve the public imported powersave numeric profile", () => {
  const result = encode("powersave"), {table} = result;
  assert.equal(table.length, 14384);
  assert.deepEqual([0, 4, 8, 12].map(offset => uint(table, offset)), [1, 0, 2, 3]);
  assert.deepEqual([16, 20, 24, 28].map(offset => uint(table, offset)), [0, 6, 0, 0]);
  const offset = cpu(result, result.defaults[0], 0);
  assert.deepEqual([0, 4, 8, 16, 20].map(delta => uint(table, offset + delta)),
    [748800, 1996800, 3, 0, 150]);
  assert.equal(uint(table, 48 + result.defaults[2] * 336 + 4), 0, "screen off releases GPU percentage lease");
});

test("All four modes and normal/Camera/ELP categories fit without dropping profiles", () => {
  const apps = {};
  for (const mode of ["powersave", "balance", "performance", "fast"])
    for (const category of ["normal", "Camera", "ELP"])
      apps[`com.example.${mode}.${category}`] = {mode, category};
  apps["com.example.ignore"] = "ignore";
  const result = encode("balance", apps);
  assert.equal(result.rules.length, 13);
  assert.equal(result.profileCount, 10);
  assert.ok(result.profileCount <= 16);
  const camera = result.rules.find(rule => rule.name === "com.example.powersave.Camera").profiles[0];
  const camera0 = cpu(result, camera, 0), camera6 = cpu(result, camera, 1);
  assert.deepEqual([uint(result.table, camera0), uint(result.table, camera0 + 4),
    uint(result.table, camera0 + 8)], [2400000, 3532800, 0]);
  assert.deepEqual([uint(result.table, camera6), uint(result.table, camera6 + 4)], [3283200, 4320000]);
  const fast = result.rules.find(rule => rule.name === "com.example.fast.normal").profiles[0];
  assert.equal(uint(result.table, cpu(result, fast, 0) + 8), 0, "fast's static policy opens no PMU feedback");
  const ignored = result.rules.find(rule => rule.name === "com.example.ignore").profiles;
  assert.deepEqual(ignored, [ignored[0], ignored[0], ignored[0]]);
  assert.ok(result.table.slice(48 + ignored[0] * 336, 48 + (ignored[0] + 1) * 336).every(value => value === 0));
});

test("ELP category aliases and inherited package mode preserve exact selection", () => {
  const result = encode("performance", {
    "com.example.reader": {category: "Reader"},
    "com.example.elp": {category: "ELP", mode: "performance"},
    "com.example.ignored": "igoned",
  });
  assert.deepEqual(result.rules.find(r => r.name === "com.example.reader").profiles,
    result.rules.find(r => r.name === "com.example.elp").profiles);
  const name = new TextDecoder().decode(result.table.slice(5424, 5424 + 128)).split("\0")[0];
  assert.equal(name, "com.example.elp");
});

test("Rejects invalid names, modes, categories, settings and more than 64 packages", () => {
  for (const value of [null, [], "balance"]) assert.throws(() => frameSceneProtocol(value, seed));
  assert.throws(() => encode("speed"));
  for (const apps of [[], null, {"*": "balance"}, {"com.example": {category: "invented"}},
    {"com.example": {mode: "balance", unknown: 1}}, {"com.example": {mode: "oops"}},
    {"com.example": {category: 42}}, JSON.parse('{"__proto__":"balance"}')]) {
    assert.throws(() => encode("balance", apps));
  }
  const apps = Object.fromEntries(Array.from({length: 65}, (_, i) => [`com.example.p${i}`, "balance"]));
  assert.throws(() => encode("balance", apps), /64/);
  delete apps["com.example.p64"];
  assert.equal(encode("balance", apps).rules.length, 64);
});

test("Selected profile subset is packed on demand, with a strict sixteen-profile ceiling", () => {
  const source = structuredClone(seed);
  source.profiles = Array.from({length: 24}, (_, i) => {
    const profile = structuredClone(seed.profiles[seed.selectors.balance.normal[0]]);
    profile.cpus[0].kHz[0] += i;
    return profile;
  });
  for (let i = 0; i < 8; i++) source.selectors.balance[`class${i}`] = [i * 3, i * 3 + 1, i * 3 + 2];
  source.selectors.balance.normal = [0, 1, 2];
  assert.equal(encode("balance", {}, source).profileCount, 3, "unused library entries consume no table slots");
  const apps = Object.fromEntries(Array.from({length: 6}, (_, i) =>
    [`com.example.p${i}`, {category: `class${i}`} ]));
  assert.throws(() => encode("balance", apps, source), /16/);
});

test("Rejects corrupt numeric library without wrapping integer fields", () => {
  for (const mutate of [
    s => { s.cpuIds = [0, 0]; },
    s => { s.selectors.balance.normal = [999, 0, 0]; },
    s => { s.profiles[s.selectors.balance.normal[0]].gpuPercent = 201; },
    s => { s.profiles[s.selectors.balance.normal[0]].cpus[0].kHz = [0, 99999999]; },
    s => { s.profiles[s.selectors.balance.normal[0]].cpus[0].margins = [[1, 200]]; },
    s => { s.profiles[s.ignored[0]].gpuPercent = 105; },
  ]) {
    const source = structuredClone(seed); mutate(source);
    assert.throws(() => encode("balance", {"com.example.ignore": "ignore"}, source));
  }
});

test("Browser encoder matches independent Python Scene profile packing byte for byte", () => {
  const result = encode("balance", {"com.example.camera": {mode: "performance", category: "Camera"},
    "com.example.ignore": "ignore", "com.example.reader": {mode: "powersave", category: "Reader"}});
  const script = `import base64,json,struct,sys
sys.path.insert(0,sys.argv[1])
from compile_scene_policy import encode_profile
x=json.load(sys.stdin); s=x['seed']; profiles=[]
for i in x['selected']:
 p=s['profiles'][i]
 profiles.append(encode_profile({'enabled':False} if not p['enabled'] else {'enabled':True,
  'gpu_percent':p['gpuPercent'],'gpu_khz':p['gpuKHz'],
  'cpus':[{'khz':c['kHz'],'margins':c['margins']} for c in p['cpus']]},len(s['cpuIds'])))
ids=s['cpuIds']; defaults=x['defaults']; rules=x['rules']
b=struct.pack('<12I',1,len(rules),len(ids),len(profiles),*ids,*([0]*(4-len(ids))),*defaults,0)
b+=b''.join(profiles)+bytes((16-len(profiles))*336)
for r in rules:b+=struct.pack('<128s3I',r['name'].encode('ascii'),*r['profiles'])
b+=bytes((64-len(rules))*140)
print(base64.b64encode(b).decode('ascii'))`;
  const completed = spawnSync(process.env.PYTHON || "python", ["-c", script, path.dirname(__dirname)], {
    input: JSON.stringify({seed, selected: result.selectedLibraryProfiles, defaults: result.defaults, rules: result.rules}),
    encoding: "utf8", timeout: 10000,
  });
  assert.equal(completed.status, 0, completed.stderr || completed.error?.message);
  assert.deepEqual(Buffer.from(result.table), Buffer.from(completed.stdout.trim(), "base64"));
});
