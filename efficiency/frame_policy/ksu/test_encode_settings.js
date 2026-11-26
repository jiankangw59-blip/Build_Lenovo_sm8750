"use strict";
const assert = require("node:assert/strict"), fs = require("node:fs"), os = require("node:os"), path = require("node:path");
const {spawnSync} = require("node:child_process");
const {test} = require("node:test");

test("private settings migration preserves GPU, Scene and every existing rule", () => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), "frame-private-migration-"));
  try {
    const settings = JSON.parse(fs.readFileSync(path.join(__dirname, "defaults/settings.json"), "utf8"));
    settings.enabled = settings.gpu = true;
    settings.sceneMode = "powersave";
    settings.sceneApps = {"com.example.private": {mode: "performance", category: "Camera"}};
    settings.games["com.example.private"] = [60, 120];
    delete settings.fasMode;
    const input = path.join(directory, "private.json"), output = path.join(directory, "packet.bin");
    const original = JSON.stringify(settings, null, 2) + "\n";
    fs.writeFileSync(input, original);
    const run = (target = output) => spawnSync(process.execPath, [path.join(__dirname, "encode_settings.js"),
      "--settings", input, "--output", target], {encoding: "utf8"});
    const completed = run();
    assert.equal(completed.status, 0, completed.stderr);
    const payload = fs.readFileSync(output), length = payload.readUInt32LE(4);
    assert.equal(payload.readUInt32LE(0), 0x46505332);
    assert.deepEqual(JSON.parse(payload.subarray(32, 32 + length).toString("utf8")), {...settings, fasMode: "balance"});
    assert.equal(payload.subarray(-80).readUInt32LE(4), 1);
    assert.equal(payload.subarray(-80).readUInt32LE(8), 1000);
    assert.equal(fs.readFileSync(input, "utf8"), original);
    assert.notEqual(run().status, 0, "Existing output must not be overwritten");
    assert.notEqual(run(input).status, 0, "Private input must not be overwritten");
  } finally {
    const target = fs.realpathSync(directory), temporary = fs.realpathSync(os.tmpdir());
    assert.equal(path.dirname(target), temporary, "Cleanup must stay in the intended temporary parent");
    assert.ok(path.basename(target).startsWith("frame-private-migration-"));
    fs.rmSync(target, {recursive: true, force: true});
  }
});
