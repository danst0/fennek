// =============================================================================
// webfm_html.h — die eingebettete Web-Oberfläche der SD-Dateiverwaltung.
//
// Eine einzelne Seite (PROGMEM), die per fetch() gegen die JSON-API aus
// webfm.cpp arbeitet (/api/list, /api/download, /api/upload, /api/delete,
// /api/mkdir). Bewusst ohne externe Abhängigkeiten — funktioniert offline.
// =============================================================================
#pragma once

#include <pgmspace.h>

const char kWebFmHtml[] PROGMEM = R"HTML(<!doctype html>
<html lang="de">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Fennek &mdash; SD-Karte</title>
<style>
 body{font-family:system-ui,sans-serif;margin:0;background:#f4f1ea;color:#222}
 header{background:#2b2620;color:#f4f1ea;padding:10px 16px}
 header h1{margin:0;font-size:18px}
 header small{opacity:.7;font-weight:normal}
 main{max-width:760px;margin:0 auto;padding:12px}
 #crumbs{margin:8px 0;font-size:14px}
 #crumbs a{color:#7a4a12;text-decoration:none}
 #crumbs a:hover{text-decoration:underline}
 .bar{display:flex;gap:8px;flex-wrap:wrap;align-items:center;margin:8px 0}
 button{background:#7a4a12;color:#fff;border:0;border-radius:6px;padding:6px 12px;cursor:pointer}
 button:hover{background:#935a1a}
 button.warn{background:#8a2a2a}
 table{width:100%;border-collapse:collapse;background:#fff;border-radius:8px;overflow:hidden}
 th,td{text-align:left;padding:7px 10px;border-bottom:1px solid #e4ded2;font-size:14px}
 th{background:#e9e2d4}
 td.sz{text-align:right;white-space:nowrap;color:#666}
 td.act{text-align:right;white-space:nowrap}
 a.dir{font-weight:bold;color:#7a4a12;text-decoration:none}
 a.file{color:#222;text-decoration:none}
 a.file:hover,a.dir:hover{text-decoration:underline}
 #msg{margin:8px 0;font-size:14px;color:#555;min-height:18px}
 progress{width:160px}
</style>
</head>
<body>
<header><h1>Fennek <small>&mdash; SD-Dateiverwaltung</small></h1></header>
<main>
<div id="crumbs"></div>
<div class="bar">
 <input type="file" id="up" multiple>
 <button onclick="upload()">Hochladen</button>
 <button onclick="mkdir()">Neuer Ordner</button>
 <progress id="prog" value="0" max="100" hidden></progress>
</div>
<div id="msg"></div>
<table>
 <thead><tr><th>Name</th><th style="text-align:right">Gr&ouml;&szlig;e</th><th></th></tr></thead>
 <tbody id="tbl"></tbody>
</table>
</main>
<script>
let cur = '/';
const enc = encodeURIComponent;
const $   = id => document.getElementById(id);

function fmtSize(n){
  if (n >= 1048576) return (n/1048576).toFixed(1) + ' MB';
  if (n >= 1024)    return (n/1024).toFixed(1) + ' KB';
  return n + ' B';
}

function crumbs(){
  const parts = cur.split('/').filter(Boolean);
  let html = '<a href="#" onclick="go(\'/\');return false">SD</a>';
  let p = '';
  for (const part of parts){
    p += '/' + part;
    html += ' / <a href="#" onclick="go(\'' + p.replace(/'/g,"\\'") + '\');return false">' + part + '</a>';
  }
  $('crumbs').innerHTML = html;
}

async function load(){
  $('msg').textContent = 'Lade ...';
  try {
    const r = await fetch('/api/list?path=' + enc(cur));
    const j = await r.json();
    if (!r.ok) throw new Error(j.err || r.status);
    j.entries.sort((a,b) => (b.d - a.d) || a.n.localeCompare(b.n));
    let rows = '';
    for (const e of j.entries){
      const full = (cur === '/' ? '' : cur) + '/' + e.n;
      const fq = full.replace(/'/g,"\\'");
      if (e.d){
        rows += '<tr><td><a class="dir" href="#" onclick="go(\'' + fq + '\');return false">&#128193; '
              + e.n + '</a></td><td class="sz"></td><td class="act">'
              + '<button class="warn" onclick="del(\'' + fq + '\')">L&ouml;schen</button></td></tr>';
      } else {
        rows += '<tr><td><a class="file" href="/api/download?path=' + enc(full) + '">' + e.n
              + '</a></td><td class="sz">' + fmtSize(e.s) + '</td><td class="act">'
              + '<button class="warn" onclick="del(\'' + fq + '\')">L&ouml;schen</button></td></tr>';
      }
    }
    $('tbl').innerHTML = rows || '<tr><td colspan="3">(leer)</td></tr>';
    $('msg').textContent = j.entries.length + ' Eintrag/Einträge';
  } catch (e){
    $('msg').textContent = 'Fehler: ' + e.message;
  }
  crumbs();
}

function go(p){ cur = p || '/'; load(); }

async function post(url){
  const r = await fetch(url, {method:'POST'});
  const j = await r.json().catch(() => ({}));
  if (!r.ok) throw new Error(j.err || r.status);
}

async function del(p){
  if (!confirm('Wirklich löschen?\n' + p)) return;
  try { await post('/api/delete?path=' + enc(p)); } catch (e){ alert('Fehler: ' + e.message); }
  load();
}

async function mkdir(){
  const name = prompt('Name des neuen Ordners:');
  if (!name) return;
  const full = (cur === '/' ? '' : cur) + '/' + name;
  try { await post('/api/mkdir?path=' + enc(full)); } catch (e){ alert('Fehler: ' + e.message); }
  load();
}

async function upload(){
  const files = $('up').files;
  if (!files.length){ alert('Erst Datei(en) auswählen.'); return; }
  const prog = $('prog');
  prog.hidden = false;
  for (let i = 0; i < files.length; i++){
    $('msg').textContent = 'Lade hoch (' + (i+1) + '/' + files.length + '): ' + files[i].name;
    prog.value = 0;
    // XHR statt fetch: liefert Upload-Fortschritt.
    await new Promise((res, rej) => {
      const fd = new FormData();
      fd.append('file', files[i]);
      const x = new XMLHttpRequest();
      x.open('POST', '/api/upload?path=' + enc(cur));
      x.upload.onprogress = e => { if (e.lengthComputable) prog.value = 100 * e.loaded / e.total; };
      x.onload  = () => {
        if (x.status === 200) return res();
        let why = 'HTTP ' + x.status;
        try { why = JSON.parse(x.responseText).err || why; } catch (_) {}
        rej(new Error(why));
      };
      x.onerror = () => rej(new Error('Netzwerkfehler'));
      x.send(fd);
    }).catch(e => alert('Upload-Fehler bei ' + files[i].name + ': ' + e.message));
  }
  prog.hidden = true;
  $('up').value = '';
  load();
}

load();
</script>
</body>
</html>
)HTML";
