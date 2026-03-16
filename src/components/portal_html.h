#pragma once
#include <pgmspace.h>

static const char PORTAL_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>DeLorean DMD</title><style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:system-ui,sans-serif;background:#1a1a2e;color:#e0e0e0;padding:16px;max-width:420px;margin:0 auto}
h2{color:#0ff;margin-bottom:12px;font-size:1.2em}
h3{color:#8ab4f8;margin:18px 0 8px;font-size:1em;border-bottom:1px solid #333;padding-bottom:4px}
label{display:block;margin:8px 0 2px;font-size:.85em;color:#aaa}
input,select{width:100%;padding:8px;border:1px solid #333;border-radius:4px;background:#16213e;color:#e0e0e0;font-size:.95em}
input[type=range]{padding:4px 0;background:transparent;border:none}
button{padding:10px 16px;border:none;border-radius:4px;cursor:pointer;font-size:.95em;margin-top:8px;width:100%}
.btn-scan{background:#0d7377;color:#fff}
.btn-conn{background:#533483;color:#fff}
.btn-mqtt{background:#1a6b3c;color:#fff}
.btn-panel{background:#b45309;color:#fff}
.btn-notify{background:#5b21b6;color:#fff}
button:active{opacity:.7}
.msg{margin-top:8px;padding:8px;border-radius:4px;background:#16213e;font-size:.85em;min-height:1.5em}
.ok{color:#0f6}.err{color:#f55}
select{margin-top:4px}
.row{display:flex;align-items:center;gap:10px;margin-top:6px}
.row label{margin:0;flex-shrink:0;min-width:50px}
.row input[type=range]{flex:1}
.row span{min-width:32px;text-align:right;font-size:.9em}
.toggle{position:relative;width:50px;height:26px;flex-shrink:0}
.toggle input{opacity:0;width:0;height:0}
.toggle .sl{position:absolute;inset:0;background:#444;border-radius:13px;cursor:pointer;transition:.2s}
.toggle .sl::before{content:'';position:absolute;width:20px;height:20px;left:3px;bottom:3px;background:#ccc;border-radius:50%;transition:.2s}
.toggle input:checked+.sl{background:#0d7377}
.toggle input:checked+.sl::before{transform:translateX(24px)}
.spin::after{content:'';display:inline-block;width:12px;height:12px;border:2px solid #0ff;border-top-color:transparent;border-radius:50%;animation:sp .6s linear infinite;margin-left:6px;vertical-align:middle}
@keyframes sp{to{transform:rotate(360deg)}}
</style></head><body>
<h2>&#x1F527; DeLorean DMD</h2>

<h3>&#x1F4F6; WiFi</h3>
<button class="btn-scan" onclick="scan()">Scan Networks</button>
<select id="nets" style="display:none" onchange="document.getElementById('ssid').value=this.value"></select>
<label for="ssid">SSID</label>
<input id="ssid" autocomplete="off" placeholder="Network name">
<label for="pass">Password</label>
<input id="pass" type="password" placeholder="Password">
<button class="btn-conn" onclick="conn()">Connect</button>
<div id="wst" class="msg">Ready</div>

<h3>&#x1F4E1; MQTT</h3>
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

<h3>&#x1F4BB; Panel Control</h3>
<div class="row">
<label>Power</label>
<div class="toggle"><input type="checkbox" id="pon" onchange="panelCtl()"><span class="sl"></span></div>
</div>
<div class="row">
<label>Bright</label>
<input type="range" id="pbr" min="0" max="255" value="25" oninput="document.getElementById('bv').textContent=this.value" onchange="panelCtl()">
<span id="bv">25</span>
</div>
<button class="btn-panel" onclick="panelCtl()">Apply</button>
<div id="pst" class="msg"></div>

<h3>&#x1F4AC; Text Notification</h3>
<label for="nt">Message</label>
<input id="nt" placeholder="Hello World">
<div class="row">
<label>Color</label>
<input type="color" id="nc" value="#ffffff" style="width:50px;padding:2px;flex-shrink:0">
<label style="margin:0 6px 0 10px">Size</label>
<select id="ns" style="width:60px"><option value="1">1</option><option value="2">2</option><option value="3">3</option></select>
<label style="margin:0 6px 0 10px">Effect</label>
<select id="ne"><option value="">None</option><option value="rainbow">Rainbow</option></select>
</div>
<div class="row">
<label>Duration</label>
<input type="number" id="nd" value="5" style="width:90px" min="0" placeholder="loops / sec">
<span style="font-size:.8em;margin-left:8px;color:#bbb">wide=loops, short=seconds</span>
</div>
<button class="btn-notify" onclick="sendNotify()">Send Notification</button>
<div id="nst" class="msg"></div>

<script>
var W=document.getElementById('wst'),M=document.getElementById('mst'),P=document.getElementById('pst');
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

function sendNotify(){
var t=document.getElementById('nt').value;
var N=document.getElementById('nst');
if(!t){N.className='msg err';N.textContent='Enter a message';return;}
var c=document.getElementById('nc').value;
var s=document.getElementById('ns').value;
var e=document.getElementById('ne').value;
var d=document.getElementById('nd').value||'5';
N.className='msg';N.innerHTML='Sending<span class="spin"></span>';
fetch('/notify',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
body:'text='+encodeURIComponent(t)+'&color='+encodeURIComponent(c)+'&size='+s+'&effect='+encodeURIComponent(e)+'&duration='+d})
.then(r=>r.json()).then(d=>{N.className='msg '+(d.ok?'ok':'err');N.textContent=d.msg||'';
}).catch(()=>{N.className='msg err';N.textContent='Request failed';});}

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
}).catch(()=>{});
</script></body></html>)rawliteral";
