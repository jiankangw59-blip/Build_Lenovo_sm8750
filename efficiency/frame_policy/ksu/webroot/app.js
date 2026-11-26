"use strict";
const backend = "sh /data/adb/modules/frame_policy_config/config.sh";
let counter = 0, savedHash = "", seed, busy = false;
const editor = document.getElementById("editor"), message = document.getElementById("message");
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
  const {payload} = frameSettingsProtocol(settings, seed);
  let binary = "";
  for (let i = 0; i < payload.length; i += 8192) binary += String.fromCharCode(...payload.subarray(i, i + 8192));
  await execute(`printf %s '${btoa(binary)}' | base64 -d | ${backend} save ${savedHash}`);
  await refresh(); message.textContent = "已保存并完成应用；请查看上方实际运行状态。";
});
document.getElementById("refresh").onclick = () => run(refresh);
document.getElementById("rollback").onclick = () => run(async () => {
  await execute(`${backend} rollback`); await refresh(); message.textContent = "已恢复上次配置。";
});
run(async () => { seed = JSON.parse(await execute(`${backend} seed`)); await refresh(); });
