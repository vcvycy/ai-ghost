// machine.js —— 单台机器的交互终端
// 用 ?id=7 拿 client id,800ms 轮询 /api/output 增量显示

const params = new URLSearchParams(location.search);
const cid = params.get("id") || location.pathname.split("/").pop();
document.getElementById("cid").textContent = cid;

let lastLen = 0;
let alive   = true;

function appendText(text, cls) {
  const out = document.getElementById("out");
  if (cls) {
    const span = document.createElement("span");
    span.className = cls;
    span.textContent = text;
    out.appendChild(span);
  } else {
    out.appendChild(document.createTextNode(text));
  }
  out.scrollTop = out.scrollHeight;
}

async function poll() {
  if (!alive) return;
  try {
    const r = await fetch("/api/output?client=" + cid, {cache: "no-store"});

    if (r.status === 404) {
      alive = false;
      document.getElementById("status").textContent = "○ offline";
      document.getElementById("status").style.color = "#f85149";
      appendText("\n[!] client disconnected (refresh page to reconnect)\n");
      return;
    }
    if (!r.ok) return;

    const buf = new Uint8Array(await r.arrayBuffer());
    if (buf.byteLength > lastLen) {
      const chunk = buf.subarray(lastLen);
      const text  = new TextDecoder().decode(chunk);
      const lines = text.split("\n");
      for (let i = 0; i < lines.length; i++) {
        if (i > 0) appendText("\n");
        if (/^\[\d{2}:\d{2}:\d{2}\] >> /.test(lines[i])) {
          appendText(lines[i], "sent");
        } else {
          appendText(lines[i]);
        }
      }
      lastLen = buf.byteLength;
      document.getElementById("status").textContent = "● online";
      document.getElementById("status").style.color = "#3fb950";
    }
  } catch (e) { /* 忽略轮询错误 */ }
}

setInterval(poll, 700);
poll();

// 元信息
fetch("/api/clients").then(r => r.json()).then(arr => {
  const c = arr.find(x => String(x.id) === String(cid));
  if (c) {
    document.getElementById("info").textContent =
      `${c.addr} · 上线 ${new Date(c.connected_at*1000).toLocaleTimeString()}`;
  }
});

async function send() {
  if (!alive) return;
  const input = document.getElementById("cmd");
  const cmd   = input.value;
  if (!cmd) return;
  input.value = "";
  input.disabled = true;
  try {
    await fetch("/api/exec?client=" + cid, {
      method: "POST",
      headers: {"Content-Type": "application/x-www-form-urlencoded"},
      body: "cmd=" + encodeURIComponent(cmd),
    });
  } catch (e) {
    appendText("\n[!] send failed: " + e + "\n");
  } finally {
    input.disabled = false;
    input.focus();
  }
  setTimeout(poll, 80);
}

document.getElementById("cmd").addEventListener("keydown", e => {
  if (e.key === "Enter") { e.preventDefault(); send(); }
  if (e.key === "l" && e.ctrlKey) {
    e.preventDefault();
    document.getElementById("out").textContent = "";
  }
});
document.getElementById("btn").addEventListener("click", send);
