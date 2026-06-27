// index.js —— 机器列表,2s 轮询 /api/clients

function esc(s) {
  return String(s).replace(/[&<>"']/g, c => ({
    '&':'&amp;', '<':'&lt;', '>':'&gt;', '"':'&quot;', "'":'&#39;'
  })[c]);
}

async function refresh() {
  try {
    const r = await fetch("/api/clients");
    const arr = await r.json();
    document.getElementById("count").textContent = arr.length;

    const body  = document.getElementById("body");
    const empty = document.getElementById("empty");
    const tbl   = document.getElementById("tbl");

    if (!arr.length) {
      body.innerHTML = "";
      empty.style.display = "block";
      tbl.style.display   = "none";
      return;
    }
    empty.style.display = "none";
    tbl.style.display   = "";

    body.innerHTML = arr.map(c => `
      <tr>
        <td><code>${c.id}</code></td>
        <td>${esc(c.name)}</td>
        <td><code>${esc(c.addr)}</code></td>
        <td>${new Date(c.connected_at*1000).toLocaleTimeString()}</td>
        <td>${new Date(c.last_seen*1000).toLocaleTimeString()}</td>
        <td class="muted">${c.banner ? esc(c.banner) : "<i>pending…</i>"}</td>
        <td><a class="btn" href="/machine/${c.id}">终端 →</a></td>
      </tr>`).join("");
  } catch (e) {
    console.error("refresh err:", e);
  }
}

setInterval(refresh, 2000);
refresh();
