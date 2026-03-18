#pragma once
#include <pgmspace.h>

static const char PORTAL_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>DeLorean DMD</title><style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:ui-monospace,"Courier New",monospace;background:#040704;color:#9dff8a;padding:14px;max-width:420px;margin:0 auto;line-height:1.2;background-image:repeating-linear-gradient(180deg,rgba(120,255,120,.05),rgba(120,255,120,.05) 1px,transparent 1px,transparent 3px)}
h2{color:#c8ff86;margin-bottom:10px;font-size:1.1em;letter-spacing:.06em;text-transform:uppercase;text-shadow:0 0 8px rgba(140,255,120,.35)}
h3{color:#7cf8a0;margin:16px 0 6px;font-size:.92em;border-bottom:1px dashed #255a2f;padding-bottom:4px;letter-spacing:.04em}
label{display:block;margin:7px 0 2px;font-size:.8em;color:#7dd789}
input,select{width:100%;padding:8px;border:1px solid #2a6e37;border-radius:2px;background:#07120a;color:#c5ffb7;font-size:.9em}
input[type=range]{padding:3px 0;background:transparent;border:none}
button{padding:9px 12px;border:1px solid #2e8c43;border-radius:2px;cursor:pointer;font-size:.9em;margin-top:8px;width:100%;background:#0a2510;color:#c9ffb8}
.btn-scan{background:#0f3218}
.btn-conn{background:#11381d}
.btn-mqtt{background:#144121}
.btn-panel{background:#1b4e2a}
.btn-clock{background:#1b3f50}
button:active{opacity:.82}
.msg{margin-top:8px;padding:8px;border-radius:2px;background:#061108;border:1px solid #1f5d2c;font-size:.8em;min-height:1.5em}
.ok{color:#7dff9f}.err{color:#ff8b8b}
select{margin-top:4px}
.row{display:flex;align-items:center;gap:10px;margin-top:6px}
.row label{margin:0;flex-shrink:0;min-width:50px}
.row input[type=range]{flex:1}
.row span{min-width:32px;text-align:right;font-size:.9em}
.toggle{position:relative;width:50px;height:26px;flex-shrink:0}
.toggle input{opacity:0;width:0;height:0}
.toggle .sl{position:absolute;inset:0;background:#112315;border:1px solid #2a6e37;border-radius:13px;cursor:pointer;transition:.2s}
.toggle .sl::before{content:'';position:absolute;width:20px;height:20px;left:2px;bottom:2px;background:#3d7f4f;border-radius:50%;transition:.2s}
.toggle input:checked+.sl{background:#174f28}
.toggle input:checked+.sl::before{transform:translateX(24px)}
.spin::after{content:'';display:inline-block;width:11px;height:11px;border:2px solid #8dff8e;border-top-color:transparent;border-radius:50%;animation:sp .7s linear infinite;margin-left:6px;vertical-align:middle}
@keyframes sp{to{transform:rotate(360deg)}}
.btn-ota{background:#2d1f08;border-color:#8c6a1e}
.progress-bar{width:100%;background:#07120a;border:1px solid #2a6e37;border-radius:2px;margin-top:8px;height:18px;display:none}
.progress-bar div{height:100%;background:#2e8c43;border-radius:2px;width:0;transition:width .2s}
</style></head><body>
<h2>DeLorean DMD</h2>

<h3>WiFi</h3>
<button class="btn-scan" onclick="scan()">Scan Networks</button>
<select id="nets" style="display:none" onchange="document.getElementById('ssid').value=this.value"></select>
<label for="ssid">SSID</label>
<input id="ssid" autocomplete="off" placeholder="Network name">
<label for="pass">Password</label>
<input id="pass" type="password" placeholder="Password">
<button class="btn-conn" onclick="conn()">Connect</button>
<div id="wst" class="msg">Ready</div>

<h3>MQTT</h3>
<label for="ms">Server</label>
<input id="ms" placeholder="e.g. 192.168.1.100">
<label for="mp">Port</label>
<input id="mp" type="number" placeholder="1883" value="1883">
<label for="mu">Username (optional)</label>
<input id="mu" autocomplete="off" placeholder="Username">
<label for="mw">Password (optional)</label>
<input id="mw" type="password" placeholder="Password">
<label for="mc">Client ID</label>
<input id="mc" placeholder="delorean-dmd">
<label for="mt">Topic</label>
<input id="mt" placeholder="delorean-dmd">
<button class="btn-mqtt" onclick="mqttSave()">Save MQTT Settings</button>
<div id="mst" class="msg"></div>

<h3>Panel Control</h3>
<div class="row">
<label>Power</label>
<label class="toggle"><input type="checkbox" id="pon" onchange="panelCtl()"><span class="sl"></span></label>
</div>
<div class="row">
<label>Bright</label>
<input type="range" id="pbr" min="0" max="255" value="25" oninput="document.getElementById('bv').textContent=this.value" onchange="panelCtl()">
<span id="bv">25</span>
</div>
<button class="btn-panel" onclick="panelCtl()">Apply</button>
<div id="pst" class="msg"></div>

<h3>Clock Mode</h3>
<div class="row">
<label>Enable</label>
<label class="toggle"><input type="checkbox" id="cen"><span class="sl"></span></label>
</div>
<label for="ce">Show clock after N GIFs</label>
<input id="ce" type="number" min="1" value="5" placeholder="5">
<label for="ctz">Timezone (POSIX TZ)</label>
<input id="ctz" placeholder="UTC0" value="UTC0">
<button class="btn-clock" onclick="clockSave()">Save Clock Settings</button>
<div id="cst" class="msg"></div>

<h3>Firmware Update (OTA)</h3>
<label for="fwfile">Select .bin firmware file</label>
<input type="file" id="fwfile" accept=".bin" style="padding:6px 0">
<button class="btn-ota" onclick="otaUpload()">Upload Firmware</button>
<div class="progress-bar" id="otapb"><div id="otafill"></div></div>
<div id="ost" class="msg"></div>

<script>
var W=document.getElementById('wst'),M=document.getElementById('mst'),P=document.getElementById('pst'),C=document.getElementById('cst');
function scan(){
W.className='msg';W.innerHTML='Scanning<span class="spin"></span>';
fetch('/scan').then(r=>r.json()).then(d=>{
var s=document.getElementById('nets');s.innerHTML='<option value="">-- select --</option>';
d.sort((a,b)=>b.r-a.r);
d.forEach(n=>{var o=document.createElement('option');o.value=n.s;o.textContent=n.s+' ('+n.r+'dBm'+(n.e?', open':'')+')';s.appendChild(o)});
s.style.display='block';W.textContent='Found '+d.length+' network(s)';
}).catch(()=>{W.className='msg err';W.textContent='Scan failed'});}

function conn(){
var ssid=document.getElementById('ssid').value,pass=document.getElementById('pass').value;
if(!ssid){W.className='msg err';W.textContent='Enter an SSID';return;}
W.className='msg';W.innerHTML='Connecting<span class="spin"></span>';
fetch('/connect',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
body:'ssid='+encodeURIComponent(ssid)+'&pass='+encodeURIComponent(pass)})
.then(r=>r.json()).then(d=>{
if(d.ok){W.className='msg ok';W.textContent='Connected! IP: '+d.ip;}
else{W.className='msg err';W.textContent=d.msg||'Failed';}
}).catch(()=>{W.className='msg err';W.textContent='Request failed'});}

function mqttSave(){
var b='server='+encodeURIComponent(document.getElementById('ms').value)
+'&port='+encodeURIComponent(document.getElementById('mp').value)
+'&user='+encodeURIComponent(document.getElementById('mu').value)
+'&pass='+encodeURIComponent(document.getElementById('mw').value)
+'&client='+encodeURIComponent(document.getElementById('mc').value)
+'&topic='+encodeURIComponent(document.getElementById('mt').value);
M.className='msg';M.innerHTML='Saving<span class="spin"></span>';
fetch('/mqtt',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b})
.then(r=>r.json()).then(d=>{
M.className='msg '+(d.ok?'ok':'err');M.textContent=d.msg;
}).catch(()=>{M.className='msg err';M.textContent='Request failed'});}

function panelCtl(){
var on=document.getElementById('pon').checked?1:0;
var br=document.getElementById('pbr').value;
fetch('/panel',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
body:'on='+on+'&brightness='+br})
.then(r=>r.json()).then(d=>{
P.className='msg '+(d.ok?'ok':'err');P.textContent=d.msg||'';
}).catch(()=>{P.className='msg err';P.textContent='Request failed';});}

function clockSave(){
var en=document.getElementById('cen').checked?1:0;
var ev=document.getElementById('ce').value||'5';
var tz=document.getElementById('ctz').value||'UTC0';
C.className='msg';C.innerHTML='Saving<span class="spin"></span>';
fetch('/clock',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
body:'enabled='+en+'&every='+encodeURIComponent(ev)+'&tz='+encodeURIComponent(tz)})
.then(r=>r.json()).then(d=>{
C.className='msg '+(d.ok?'ok':'err');C.textContent=d.msg||'';
}).catch(()=>{C.className='msg err';C.textContent='Request failed';});}

function otaUpload(){
var f=document.getElementById('fwfile').files[0];
var O=document.getElementById('ost'),pb=document.getElementById('otapb'),fill=document.getElementById('otafill');
if(!f){O.className='msg err';O.textContent='Select a .bin file first';return;}
var fd=new FormData();fd.append('firmware',f);
var xhr=new XMLHttpRequest();
xhr.open('POST','/update');
xhr.upload.onprogress=function(e){
if(e.lengthComputable){var p=Math.round(e.loaded/e.total*100);fill.style.width=p+'%';O.textContent='Uploading… '+p+'%';}};
xhr.onloadstart=function(){pb.style.display='block';O.className='msg';O.innerHTML='Starting upload<span class="spin"></span>';};
xhr.onload=function(){
try{var d=JSON.parse(xhr.responseText);O.className='msg '+(d.ok?'ok':'err');O.textContent=d.msg||'';}
catch(e){O.className='msg err';O.textContent='Unexpected response';}};
xhr.onerror=function(){O.className='msg err';O.textContent='Upload failed';};
xhr.send(fd);}

fetch('/status').then(r=>r.json()).then(d=>{
if(d.connected)W.innerHTML='<span class="ok">Connected to '+d.ssid+' &mdash; IP: '+d.ip+'</span>';
else W.textContent=d.ap?'AP mode \u2014 not connected to WiFi':'Not connected';
if(d.mqtt){var m=d.mqtt;document.getElementById('ms').value=m.server||'';
document.getElementById('mp').value=m.port||1883;document.getElementById('mu').value=m.user||'';
document.getElementById('mc').value=m.client||'';document.getElementById('mt').value=m.topic||'';
if(m.connected)M.innerHTML='<span class="ok">MQTT connected</span>';
else if(m.server)M.innerHTML='<span class="err">MQTT not connected</span>';}
document.getElementById('pon').checked=d.panel_on;
document.getElementById('pbr').value=d.brightness;document.getElementById('bv').textContent=d.brightness;
if(d.clock){
document.getElementById('cen').checked=d.clock.enabled;
document.getElementById('ce').value=d.clock.every||5;
document.getElementById('ctz').value=d.clock.tz||'UTC0';
C.className='msg '+(d.clock.synced?'ok':'err');
C.textContent=d.clock.synced?'Clock synced (NTP OK)':'Clock not synced yet';
}
}).catch(()=>{});
</script></body></html>)rawliteral";
