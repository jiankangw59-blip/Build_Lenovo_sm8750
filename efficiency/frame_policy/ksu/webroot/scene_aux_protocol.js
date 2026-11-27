"use strict";

/* Fixed typed records, never scripts or caller-selected paths. */
function frameSceneAuxProtocol(settings, seed) {
  const size = 26584, header = 56, optionsAt = 168, records = 216, rulesAt = 17624;
  const table = new Uint8Array(size), view = new DataView(table.buffer);
  const u32 = (at, value) => view.setUint32(at, value, true);
  u32(0, 1);
  for (let i = 0; i < 28; i++) view.setInt32(header + i * 4, -1, true);
  const enabled = settings.sceneAux ?? false;
  if (typeof enabled !== "boolean") throw new Error("sceneAux 必须为布尔值");
  if (!enabled) return {table, required: false, profileCount: 0};
  if (!seed || seed.abi !== 1 || seed.tableSize !== size || seed.cpuSize !== 136 || seed.profileSize !== 544 ||
      !Array.isArray(seed.cpuIds) || !seed.cpuIds.length || seed.cpuIds.length > 4 ||
      seed.cpuIds.some(v => !Number.isInteger(v) || v < 0 || v > 4095) || new Set(seed.cpuIds).size !== seed.cpuIds.length)
    throw new Error("Scene 辅助参数库版本或拓扑不匹配");
  const modes = ["powersave", "balance", "performance", "fast"];
  const mode = settings.sceneMode;
  if (!modes.includes(mode)) throw new Error("Scene 模式无效");
  const apps = settings.sceneApps ?? {};
  if (!apps || typeof apps !== "object" || Array.isArray(apps)) throw new Error("Scene 应用表无效");
  const own = (object, key) => object && Object.hasOwn(object, key);
  function indices(selectedMode, category) {
    if (selectedMode === "ignore" || selectedMode === "igoned") return seed.ignored;
    if (!modes.includes(selectedMode) || !own(seed.selectors, selectedMode)) throw new Error("Scene 辅助模式不存在");
    category = own(seed.categoryAliases, category) ? seed.categoryAliases[category] : category;
    if (!own(seed.selectors[selectedMode], category)) throw new Error("Scene 辅助类别不存在");
    return seed.selectors[selectedMode][category];
  }
  const names = Object.keys(apps).sort();
  if (names.length > 64) throw new Error("Scene 辅助应用超过64项");
  const chosen = [], packed = new Map();
  function select(values) {
    if (!Array.isArray(values) || values.length !== 3) throw new Error("辅助参数缺少三状态");
    return values.map(index => {
      if (!Number.isInteger(index) || index < 0 || index >= seed.profiles.length) throw new Error("辅助索引越界");
      if (!packed.has(index)) {
        if (chosen.length === 32) throw new Error("辅助参数超过32组");
        packed.set(index, chosen.length); chosen.push(index);
      }
      return packed.get(index);
    });
  }
  const defaults = select(indices(mode, "normal"));
  const rules = names.map(name => {
    if (!/^[A-Za-z0-9_]+(?:\.[A-Za-z0-9_]+)+$/.test(name) || name.length > 127) throw new Error("辅助包名无效");
    const raw = apps[name], item = typeof raw === "string" ? {mode: raw} : raw;
    if (!item || typeof item !== "object" || Array.isArray(item) || Object.keys(item).some(k => k !== "mode" && k !== "category"))
      throw new Error("辅助应用设置无效");
    return {name, profiles: select(indices(item.mode ?? mode, item.category ?? "normal"))};
  });
  [1, names.length, seed.cpuIds.length, chosen.length, ...seed.cpuIds, ...Array(4 - seed.cpuIds.length).fill(0),
    ...defaults, 0].forEach((value, i) => u32(i * 4, value));
  const integer = (value, min, max) => Number.isInteger(value) && value >= min && value <= max;
  const optionKeys = ["source", "statSource", "gestureBoost", "gestureDurationMs", "launcherBoost", "perfNotify",
    "perfNotifyInApps", "perfUsePresets", "kswapdBoost", "panelPeakHz", "gpuLock"];
  const options = seed.options && {...seed.options};
  if (!options || Object.keys(options).some(k => k !== "validMask" && !optionKeys.includes(k)) ||
      !Object.hasOwn(options, "validMask") || optionKeys.some(k => !Object.hasOwn(options, k)) ||
      !integer(options.validMask, 0, 2047)) throw new Error("Invalid typed Scene global options");
  if (settings.sceneGpuLock !== undefined) {
    if (typeof settings.sceneGpuLock !== "boolean") throw new Error("sceneGpuLock must be boolean");
    options.gpuLock = settings.sceneGpuLock ? 1 : 0;
    options.validMask |= 1024;
  }
  const optionMax = [2, 1, 1, 60000, 1, 1, 1, 1, 1, 1000, 1];
  optionKeys.forEach((key, index) => {
    if (!integer(options[key], 0, optionMax[index]) || !(options.validMask & (1 << index)) && options[key] !== 0 ||
        key === "panelPeakHz" && (options.validMask & (1 << index)) && options[key] === 0)
      throw new Error("Scene global option is out of range or unmarked");
  });
  [options.validMask, ...optionKeys.map(key => options[key])].forEach((v, i) => u32(optionsAt + i * 4, v));
  chosen.forEach((source, slot) => {
    const cpus = seed.profiles[source].cpus;
    if (!Array.isArray(cpus) || cpus.length !== seed.cpuIds.length) throw new Error("辅助CPU记录数量无效");
    let cluster = null;
    cpus.forEach((cpu, index) => {
      const keys = ["validMask", "upUs", "downUs", "hispeedKHz", "waltBoost", "coreEnable", "minCpus", "maxCpus",
        "excludesMask", "prefer", "limiterClusterCount", "limiterClustersPacked", "feedbackMask",
        "perfectKHz", "smoothness", "mtMilliPercent", "additive"];
      if (!cpu || Object.keys(cpu).some(k => !keys.includes(k)) || keys.some(k => !Object.hasOwn(cpu, k)))
        throw new Error("辅助CPU字段无效");
      const m = cpu.validMask, f = cpu.feedbackMask;
      if (!integer(m, 0, 31) || !integer(f, 0, 127) || !integer(cpu.upUs, 0, 1000000) ||
          !integer(cpu.downUs, 0, 1000000) || !integer(cpu.hispeedKHz, 0, 10000000) ||
          !integer(cpu.waltBoost, -100, 100) || !integer(cpu.coreEnable, 0, 1) ||
          !integer(cpu.minCpus, 0, 64) || !integer(cpu.maxCpus, 0, 64) ||
          cpu.minCpus > cpu.maxCpus || !integer(cpu.prefer, 0, 3) ||
          !integer(cpu.limiterClusterCount, 0, seed.cpuIds.length) || !integer(cpu.limiterClustersPacked, 0, 0xffffffff) ||
          !integer(cpu.perfectKHz, 0, 10000000) || !integer(cpu.smoothness, 0, 32) ||
          !integer(cpu.mtMilliPercent, 0, 100000) || !Array.isArray(cpu.additive) || cpu.additive.length > 8)
        throw new Error("辅助数值超出范围");
      for (const [bit, values] of [[1, [cpu.upUs]], [2, [cpu.downUs]], [4, [cpu.hispeedKHz]], [8, [cpu.waltBoost]],
                                  [16, [cpu.coreEnable, cpu.minCpus, cpu.maxCpus]]])
        if (!(m & bit) && values.some(v => v !== 0)) throw new Error("未管理的辅助字段必须为0");
      if (typeof cpu.excludesMask !== "string" || !/^0x[0-9a-f]{1,16}$/i.test(cpu.excludesMask)) throw new Error("排除CPU掩码无效");
      const excluded = BigInt(cpu.excludesMask);
      if (!(f & 1) && excluded !== 0n || !(f & 2) && cpu.prefer !== 0 ||
          !(f & 4) && (cpu.limiterClusterCount || cpu.limiterClustersPacked) ||
          !(f & 8) && cpu.perfectKHz || !(f & 16) && cpu.smoothness || !(f & 32) && cpu.mtMilliPercent ||
          !(f & 64) && cpu.additive.length) throw new Error("未标记反馈字段必须为0");
      cpu.additive.forEach((step, i) => {
        if (!Array.isArray(step) || step.length !== 2 || !integer(step[0], 0, 10000000) ||
            !integer(step[1], -10000000, 10000000) || (i === 0 ? step[0] !== 0 : step[0] <= cpu.additive[i - 1][0]))
          throw new Error("Invalid original additive kHz steps");
      });
      let encoded = cpu.limiterClustersPacked, seen = new Set();
      for (let i = 0; i < 4; i++) {
        const id = encoded % 256; encoded = Math.floor(encoded / 256);
        if (i < cpu.limiterClusterCount) {
          if (id >= seed.cpuIds.length || seen.has(id)) throw new Error("cluster索引重复或越界");
          seen.add(id);
        } else if (id !== 0) throw new Error("未使用cluster字节必须为0");
      }
      const signature = `${f & 4}:${cpu.limiterClusterCount}:${cpu.limiterClustersPacked}`;
      if (cluster !== null && signature !== cluster) throw new Error("全局cluster元数据必须一致");
      cluster = signature;
      const at = records + slot * 544 + index * 136;
      [m, cpu.upUs, cpu.downUs, cpu.hispeedKHz].forEach((v, i) => u32(at + i * 4, v));
      view.setInt32(at + 16, cpu.waltBoost, true);
      [cpu.coreEnable, cpu.minCpus, cpu.maxCpus].forEach((v, i) => u32(at + 20 + i * 4, v));
      view.setBigUint64(at + 32, excluded, true);
      [cpu.prefer, cpu.limiterClusterCount, cpu.limiterClustersPacked, f].forEach((v, i) => u32(at + 40 + i * 4, v));
      [cpu.perfectKHz, cpu.smoothness, cpu.mtMilliPercent, cpu.additive.length].forEach((v, i) => u32(at + 56 + i * 4, v));
      cpu.additive.forEach((step, i) => { u32(at + 72 + i * 8, step[0]); view.setInt32(at + 76 + i * 8, step[1], true); });
    });
  });
  rules.forEach((rule, index) => {
    const at = rulesAt + index * 140;
    table.set(new TextEncoder().encode(rule.name), at);
    rule.profiles.forEach((value, i) => u32(at + 128 + i * 4, value));
  });
  return {table, required: true, profileCount: chosen.length, selectedLibraryProfiles: chosen, rules, defaults};
}
if (typeof module !== "undefined") module.exports = {frameSceneAuxProtocol};
