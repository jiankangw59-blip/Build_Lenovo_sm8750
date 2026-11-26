"use strict";
/* Offline migration through the production encoder. Never contacts a device. */
const fs = require("node:fs"), path = require("node:path"), crypto = require("node:crypto");
const {frameSettingsProtocol} = require("./webroot/protocol.js");
const options = Object.create(null), allowed = new Set(["--settings", "--output", "--seed", "--scene-seed"]);
for (let i = 2; i < process.argv.length; i += 2) {
  const key = process.argv[i], value = process.argv[i + 1];
  if (!allowed.has(key) || !value || value.startsWith("--") || Object.hasOwn(options, key))
    throw new Error("usage: node encode_settings.js --settings PRIVATE.json --output PACKET.bin [--seed SEED.json] [--scene-seed SCENE.json]");
  options[key] = value;
}
if (!options["--settings"] || !options["--output"])
  throw new Error("--settings and --output are required");
const read = file => JSON.parse(fs.readFileSync(file, "utf8"));
const input = path.resolve(options["--settings"]), output = path.resolve(options["--output"]);
if (input === output) throw new Error("Output must not replace the private settings input");
const seed = read(options["--seed"] || path.join(__dirname, "defaults/seed.json"));
const scene = read(options["--scene-seed"] || path.join(__dirname, "defaults/scene-seed.json"));
const packet = frameSettingsProtocol(read(input), seed, scene);
fs.writeFileSync(output, packet.payload, {flag: "wx", mode: 0o600});
console.log(JSON.stringify({output, bytes: packet.payload.length,
  sha256: crypto.createHash("sha256").update(packet.payload).digest("hex"),
  fasMode: packet.settings.fasMode, sceneMode: packet.settings.sceneMode,
  cpu: packet.settings.cpu, gpu: packet.settings.gpu, device_modified: false}));
