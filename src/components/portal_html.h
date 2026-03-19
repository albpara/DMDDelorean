#pragma once
#include <pgmspace.h>

static const char PORTAL_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>DeLorean DMD</title>
<style>
@import url('https://fonts.googleapis.com/css2?family=Press+Start+2P&display=swap');
*{box-sizing:border-box;margin:0;padding:0}
body{background-color:#00B0EB;font-family:'Press Start 2P',cursive;color:#000;background-position:top center;background-repeat:repeat;background-size:75%;background-image:url(https://i.imgur.com/zwZ2RGM.png);padding:16px 14px 32px;max-width:520px;margin:0 auto;line-height:1.8}
header{text-align:center;padding:20px 16px 24px;margin-bottom:16px;background:rgba(255,255,255,.92);border:4px solid #000;box-shadow:6px 6px 0 rgba(0,0,0,.25)}
header h1{font-size:22px;color:#ffcc00;text-shadow:3px 3px #000;margin-bottom:12px}
header p{font-size:10px;color:#555}
details{background:#fff;border:4px solid #000;margin-bottom:12px;box-shadow:5px 5px 0 rgba(0,0,0,.25)}
summary{display:flex;align-items:center;justify-content:space-between;padding:14px 16px;cursor:pointer;background:#ffcc00;color:#000;list-style:none;-webkit-user-select:none;user-select:none;border-bottom:4px solid transparent;font-size:13px}
summary::-webkit-details-marker{display:none}
details[open]>summary{background:#ffd700;border-bottom:4px solid #000}
summary:hover{background:#ffd700}
.st{display:flex;align-items:center;gap:10px}
.badge{font-size:9px;padding:3px 8px;background:#33cc33;border:2px solid #009900;color:#fff;margin-left:8px;display:none;vertical-align:middle}
.badge.on{display:inline}
.ar{font-size:10px;color:#000;transition:transform .2s}
details[open]>summary .ar{transform:rotate(90deg)}
.body{padding:16px}
label{display:block;margin:14px 0 6px;font-size:12px;color:#ffcc00;text-shadow:1px 1px #000}
input,select{width:100%;padding:10px;border:4px solid #000;background:#fff;color:#000;font-size:12px;font-family:'Press Start 2P',cursive;outline:none}
input[type=range]{-webkit-appearance:none;appearance:none;padding:0;background:#ffcc00;border:4px solid #000;height:16px;cursor:pointer}
input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:20px;height:24px;background:#000;cursor:pointer}
input[type=range]::-moz-range-thumb{width:20px;height:24px;background:#000;border:none;cursor:pointer}
input[type=file]{padding:6px 0;background:transparent;border:none;font-size:11px;color:#000}
input:not([type=range]):not([type=file]):focus,select:focus{background:#e6e6e6}
select option{background:#fff}
button{font-family:'Press Start 2P',cursive;border:4px solid #000;font-size:14px;padding:12px 16px;text-transform:uppercase;cursor:pointer;position:relative;top:0;width:100%;margin-top:12px;background:#ffcc00;color:#000;box-shadow:0 6px #cc9900;transition:top .08s,box-shadow .08s}
button:hover{background:#ffd700;top:2px;box-shadow:0 4px #cc9900}
button:active{top:6px;box-shadow:0 0 #cc9900}
.msg{margin-top:12px;padding:10px 12px;border:4px solid #000;background:#fff;font-size:11px;min-height:2.8em;color:#000;word-break:break-word;line-height:1.8}
.ok{color:#155724}.err{color:#721c24}
.row{display:flex;align-items:center;gap:12px;margin-top:12px}
.row>label{margin:0;flex-shrink:0;min-width:60px;font-size:11px}
.row input[type=range]{flex:1}
.row span{min-width:32px;text-align:right;font-size:12px}
.tgl{position:relative;width:52px;height:28px;flex-shrink:0}
.tgl input{opacity:0;width:0;height:0}
.sl{position:absolute;inset:0;background:#ccc;border:3px solid #000;cursor:pointer;transition:.18s}
.sl::before{content:'';position:absolute;width:18px;height:18px;left:2px;bottom:2px;background:#fff;border:1px solid #888;transition:.18s}
.tgl input:checked+.sl{background:#33cc33;border-color:#009900}
.tgl input:checked+.sl::before{transform:translateX(24px)}
.spin::after{content:'';display:inline-block;width:12px;height:12px;border:3px solid #000;border-top-color:transparent;border-radius:50%;animation:sp .7s linear infinite;margin-left:8px;vertical-align:middle}
@keyframes sp{to{transform:rotate(360deg)}}
.pbar{background:#000;border:4px solid #000;width:100%;height:28px;margin-top:12px;position:relative;overflow:hidden;display:none}
.pbar div{background:#ffcc00;height:100%;position:absolute;top:0;left:0;width:0;transition:width .3s}
</style></head><body>
<header>
<h1>DeLorean DMD</h1>
<p>ESP32 &#xb7; HUB75 128&#xd7;32</p>
</header>

<details id="s-panel" open>
<summary>
<span class="st">Panel</span>
<span class="ar">&#x25ba;</span>
</summary>
<div class="body">
<div class="row">
<label>Power</label>
<label class="tgl"><input type="checkbox" id="pon" onchange="panelCtl()"><span class="sl"></span></label>
</div>
<div class="row">
<label>Bright</label>
<input type="range" id="pbr" min="0" max="255" value="25" oninput="document.getElementById('bv').textContent=this.value" onchange="panelCtl()">
<span id="bv">25</span>
</div>
<div class="row">
<label>Safe</label>
<label class="tgl"><input type="checkbox" id="psafe" onchange="panelCtl()"><span class="sl"></span></label>
</div>
<div id="pst" class="msg"></div>
</div>
</details>

<details id="s-wifi">
<summary>
<span class="st">WiFi <span class="badge" id="wbadge">OK</span></span>
<span class="ar">&#x25ba;</span>
</summary>
<div class="body">
<button onclick="scan()">Scan Networks</button>
<select id="nets" style="display:none;margin-top:8px" onchange="document.getElementById('ssid').value=this.value"></select>
<label for="ssid">SSID</label>
<input id="ssid" autocomplete="off" placeholder="Network name">
<label for="pass">Password</label>
<input id="pass" type="password" placeholder="Password">
<button onclick="conn()">Connect</button>
<div id="wst" class="msg">Ready</div>
</div>
</details>

<details id="s-mqtt">
<summary>
<span class="st">MQTT <span class="badge" id="mbadge">OK</span></span>
<span class="ar">&#x25ba;</span>
</summary>
<div class="body">
<label for="ms">Server</label>
<input id="ms" placeholder="192.168.1.100">
<label for="mp">Port</label>
<input id="mp" type="number" placeholder="1883" value="1883">
<label for="mu">Username</label>
<input id="mu" autocomplete="off" placeholder="optional">
<label for="mw">Password</label>
<input id="mw" type="password" placeholder="optional">
<label for="mc">Client ID</label>
<input id="mc" placeholder="delorean-dmd">
<label for="mt">Topic</label>
<input id="mt" placeholder="delorean-dmd">
<button onclick="mqttSave()">Save MQTT</button>
<div id="mst" class="msg"></div>
</div>
</details>

<details id="s-clock">
<summary>
<span class="st">Clock <span class="badge" id="cbadge">ON</span></span>
<span class="ar">&#x25ba;</span>
</summary>
<div class="body">
<div class="row">
<label>Enable</label>
<label class="tgl"><input type="checkbox" id="cen"><span class="sl"></span></label>
</div>
<label for="ce">Show every N GIFs</label>
<input id="ce" type="number" min="1" value="5" placeholder="5">
<label for="ctz">Timezone (POSIX TZ)</label>
<input id="ctz" placeholder="UTC0" value="UTC0">
<button onclick="clockSave()">Save Clock</button>
<div id="cst" class="msg"></div>
</div>
</details>

<details id="s-ota">
<summary>
<span class="st">Firmware OTA</span>
<span class="ar">&#x25ba;</span>
</summary>
<div class="body">
<label for="fwfile">Select .bin file</label>
<input type="file" id="fwfile" accept=".bin">
<button onclick="otaUpload()">Upload Firmware</button>
<div class="pbar" id="otapb"><div id="otafill"></div></div>
<div id="ost" class="msg"></div>
</div>
</details>

<script>
var W=document.getElementById('wst'),M=document.getElementById('mst'),P=document.getElementById('pst'),C=document.getElementById('cst');
function scan(){
W.className='msg';W.innerHTML='Scanning<span class="spin"></span>';
fetch('/scan').then(r=>r.json()).then(d=>{
var s=document.getElementById('nets');s.innerHTML='<option value="">&#x2014; select &#x2014;</option>';
d.sort((a,b)=>b.r-a.r);
d.forEach(n=>{var o=document.createElement('option');o.value=n.s;o.textContent=n.s+' ('+n.r+'dBm'+(n.e?', open':'')+')';s.appendChild(o)});
s.style.display='block';W.textContent='Found '+d.length+' network(s)';
}).catch(()=>{W.className='msg err';W.textContent='Scan failed';});}

function conn(){
var ssid=document.getElementById('ssid').value,pass=document.getElementById('pass').value;
if(!ssid){W.className='msg err';W.textContent='Enter an SSID';return;}
W.className='msg';W.innerHTML='Connecting<span class="spin"></span>';
fetch('/connect',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
body:'ssid='+encodeURIComponent(ssid)+'&pass='+encodeURIComponent(pass)})
.then(r=>r.json()).then(d=>{
if(d.ok){W.className='msg ok';W.textContent='Connected! IP: '+d.ip;document.getElementById('wbadge').classList.add('on');}
else{W.className='msg err';W.textContent=d.msg||'Failed';}
}).catch(()=>{W.className='msg err';W.textContent='Request failed';});}

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
}).catch(()=>{M.className='msg err';M.textContent='Request failed';});}

function panelCtl(){
var on=document.getElementById('pon').checked?1:0,br=document.getElementById('pbr').value,safe=document.getElementById('psafe').checked?1:0;
fetch('/panel',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
body:'on='+on+'&brightness='+br+'&safe='+safe})
.then(r=>r.json()).then(d=>{
P.className='msg '+(d.ok?'ok':'err');P.textContent=d.msg||'';
}).catch(()=>{P.className='msg err';P.textContent='Request failed';});}

function clockSave(){
var en=document.getElementById('cen').checked?1:0,ev=document.getElementById('ce').value||'5',tz=document.getElementById('ctz').value||'UTC0';
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
if(e.lengthComputable){var p=Math.round(e.loaded/e.total*100);fill.style.width=p+'%';O.textContent='Uploading... '+p+'%';}};
xhr.onloadstart=function(){pb.style.display='block';O.className='msg';O.innerHTML='Starting upload<span class="spin"></span>';};
xhr.onload=function(){
try{var d=JSON.parse(xhr.responseText);O.className='msg '+(d.ok?'ok':'err');O.textContent=d.msg||'';}
catch(e){O.className='msg err';O.textContent='Unexpected response';}};
xhr.onerror=function(){O.className='msg err';O.textContent='Upload failed';};
xhr.send(fd);}

fetch('/status').then(r=>r.json()).then(d=>{
if(d.connected){W.innerHTML='<span class="ok">Connected: '+d.ssid+' \u2014 '+d.ip+'</span>';document.getElementById('wbadge').classList.add('on');}
else W.textContent=d.ap?'AP mode \u2014 not connected':'Not connected';
if(d.mqtt){var m=d.mqtt;document.getElementById('ms').value=m.server||'';
document.getElementById('mp').value=m.port||1883;document.getElementById('mu').value=m.user||'';
document.getElementById('mc').value=m.client||'';document.getElementById('mt').value=m.topic||'';
if(m.connected){M.innerHTML='<span class="ok">MQTT connected</span>';document.getElementById('mbadge').classList.add('on');}
else if(m.server)M.innerHTML='<span class="err">MQTT not connected</span>';}
document.getElementById('pon').checked=d.panel_on;
document.getElementById('pbr').value=d.brightness;document.getElementById('bv').textContent=d.brightness;
if(d.safe_brightness!==undefined)document.getElementById('psafe').checked=d.safe_brightness;
if(d.clock){
document.getElementById('cen').checked=d.clock.enabled;
document.getElementById('ce').value=d.clock.every||5;
document.getElementById('ctz').value=d.clock.tz||'UTC0';
C.className='msg '+(d.clock.synced?'ok':'err');
C.textContent=d.clock.synced?'Clock synced (NTP OK)':'Clock not synced yet';
if(d.clock.enabled)document.getElementById('cbadge').classList.add('on');}
}).catch(()=>{});
</script></body></html>)rawliteral";
