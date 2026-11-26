"use strict";
const backend = "sh /data/adb/modules/frame_policy_config/config.sh";
let counter = 0, savedHash = "", seed, sceneSeed, busy = false;
const editor = document.getElementById("editor"), message = document.getElementById("message");
const fasMode = document.getElementById("fasMode"), fasBase = document.getElementById("fasMarginBase");
const fasOverrides = document.getElementById("fasOverrides");
const fasDefaults = {powersave: 3, balance: 1, performance: 0.3, fast: 0};
let fasEditError = "";
function showFasSettings(settings) {
  const tuning = frameFasProtocol(settings);
  fasMode.value = tuning.mode;
  fasBase.placeholder = String(fasDefaults[tuning.mode]);
  const margin = settings.fasMarginFps;
  const advanced = margin && typeof margin === "object" && !Array.isArray(margin);
  fasBase.value = margin === undefined ? "" : String(advanced ? margin.base : margin);
  fasOverrides.value = JSON.stringify(advanced ? Object.fromEntries(Object.entries(margin).filter(([key]) => key !== "base")) : {}, null, 2);
}
function editFasSettings() {
  try {
    if (fasBase.validity?.badInput) throw new Error("FAS 基础余量必须为数值");
    const settings = JSON.parse(editor.value), overrides = JSON.parse(fasOverrides.value || "{}");
    if (!overrides || typeof overrides !== "object" || Array.isArray(overrides) || Object.hasOwn(overrides, "base"))
      throw new Error("按帧率覆盖必须是帧率到余量的对象，不包含 base");
    settings.fasMode = fasMode.value;
    if (fasBase.value.trim() === "" && !Object.keys(overrides).length) delete settings.fasMarginFps;
    else {
      const base = fasBase.value.trim() === "" ? fasDefaults[settings.fasMode] : Number(fasBase.value);
      settings.fasMarginFps = Object.keys(overrides).length ? {base, ...overrides} : base;
    }
    frameFasProtocol(settings);
    fasEditError = "";
    editor.value = JSON.stringify(settings, null, 2) + "\n";
    fasBase.placeholder = String(fasDefaults[settings.fasMode]);
    message.textContent = "FAS 配置已编辑，点击保存并应用后生效。"; message.dataset.error = "false";
  } catch (error) { fasEditError = error.message; message.textContent = error.message; message.dataset.error = "true"; }
}
for (const control of [fasMode, fasBase, fasOverrides]) control.addEventListener("change", editFasSettings);
editor.addEventListener("input", () => { try { showFasSettings(JSON.parse(editor.value)); fasEditError = ""; } catch (_) {} });
function execute(command) {
  return new Promise((resolve, reject) => {
    if (typeof window.ksu?.exec !== "function") return reject(new Error("请从 KernelSU 管理器打开。"));
    const name = `framePolicyReply${++counter}`;
    window[name] = (code, stdout, stderr) => {
      delete window[name];
      Number(code) === 0 ? resolve(stdout) : reject(new Error((stderr || stdout || `退出码 ${code}`).trim()));
    };
    try { window.ksu.exec(command, "{}", name); }
    catch (error) { delete window[name]; reject(error); }
  });
}
async function refresh() {
  const text = await execute(`${backend} read`), split = text.indexOf("\n");
  savedHash = text.slice(0, split).trim();
  if (!/^[a-f0-9]{64}$/.test(savedHash)) throw new Error("配置读取失败");
  editor.value = text.slice(split + 1);
  showFasSettings(JSON.parse(editor.value));
  fasEditError = "";
  document.getElementById("status").textContent = await execute(`${backend} status`);
}
async function run(task) {
  if (busy) return;
  busy = true; document.querySelectorAll("button").forEach(b => b.disabled = true);
  message.textContent = ""; message.dataset.error = "false";
  try { await task(); }
  catch (error) { message.textContent = error.message; message.dataset.error = "true"; }
  finally { busy = false; document.querySelectorAll("button").forEach(b => b.disabled = false); }
}
document.getElementById("save").onclick = () => run(async () => {
  const settings = JSON.parse(editor.value);
  if (fasEditError) throw new Error(fasEditError);
  const {payload} = frameSettingsProtocol(settings, seed, sceneSeed);
  let binary = "";
  for (let i = 0; i < payload.length; i += 8192) binary += String.fromCharCode(...payload.subarray(i, i + 8192));
  await execute(`printf %s '${btoa(binary)}' | base64 -d | ${backend} save ${savedHash}`);
  await refresh(); message.textContent = "已保存并完成应用；请查看上方实际运行状态。";
});
document.getElementById("refresh").onclick = () => run(refresh);
document.getElementById("rollback").onclick = () => run(async () => {
  await execute(`${backend} rollback`); await refresh(); message.textContent = "已恢复上次配置。";
});
run(async () => {
  seed = JSON.parse(await execute(`${backend} seed`));
  sceneSeed = JSON.parse(await execute(`${backend} scene-seed`));
  await refresh();
});
