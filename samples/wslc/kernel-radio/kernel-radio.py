# Copyright (C) Microsoft Corporation. All rights reserved.
import json
import os
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import psutil


PAGE = r'''<!doctype html>
<html lang="en">
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Kernel Radio / WSLC</title>
<style>
:root{color-scheme:dark;font-family:Consolas,monospace;background:#080e13;color:#c9d8df}
body{max-width:1060px;margin:5vh auto;padding:24px}
h1{font-size:42px;letter-spacing:-2px;margin:12px 0;color:#e8fff3}
.green{color:#69f0ac}.muted{color:#8aa2ae} .label{font-size:12px;letter-spacing:2px;text-transform:uppercase}
.route{display:flex;align-items:center;gap:18px;margin:32px 0}
.node{border:1px solid #264239;padding:18px;flex:1;background:#0d191b}
.node strong{display:block;color:#69f0ac;font-size:23px;margin-top:8px}
.grid{display:grid;grid-template-columns:repeat(3,1fr);gap:14px}
.card{background:#101b22;border:1px solid #21343f;padding:22px}
.value{font-size:29px;color:#e8fff3;margin-top:12px}
canvas{display:block;width:100%;height:190px}
.scope{margin-top:16px;padding:22px;background:#0b171d;border:1px solid #21343f}
button{font:inherit;background:#69f0ac;color:#07130c;border:0;padding:15px 20px;cursor:pointer;font-weight:bold}
button:disabled{background:#456656;cursor:default}
.controls{display:flex;gap:20px;align-items:center;margin:24px 0}
pre{white-space:pre-wrap;line-height:1.9;background:#101b22;padding:20px;border-left:3px solid #69f0ac}
.error{color:#ff997f}footer{margin-top:24px;font-size:12px;color:#8aa2ae}
@media(max-width:650px){.grid{grid-template-columns:1fr}.route{gap:6px}.node{padding:10px}.node strong{font-size:16px}h1{font-size:32px}}
</style>
<div class="label green">WSLC / live Linux telemetry</div>
<h1>Kernel Radio<span class="green">.</span></h1>
<div class="muted">A Windows click. A Linux process. A real CPU heartbeat.</div>
<div class="route">
  <div class="node"><span class="label">Windows / browser</span><strong id="host"></strong></div>
  <span class="green">&rarr;</span>
  <div class="node"><span class="label">WSLC / published port</span><strong>TCP forward</strong></div>
  <span class="green">&rarr;</span>
  <div class="node"><span class="label">Linux / container</span><strong id="linux">:8000</strong></div>
</div>
<div class="grid">
  <div class="card"><div class="label muted">Pulse worker / one CPU</div><div class="value" id="cpu">0.0%</div></div>
  <div class="card"><div class="label muted">Container processes</div><div class="value" id="processes">--</div></div>
  <div class="card"><div class="label muted">WSL VM memory in use</div><div class="value" id="memory">--</div></div>
</div>
<div class="scope">
  <div class="label muted">Actual worker CPU / psutil reading Linux /proc</div>
  <canvas id="scope" width="1000" height="190"></canvas>
</div>
<div class="controls"><button id="pulse">SEND CPU PULSE / 4s</button><span id="status" class="muted">Connecting...</span></div>
<pre id="receipt">Waiting for Linux...</pre>
<footer>No simulated metrics. Python + psutil. Pulse is limited to one worker for four seconds.</footer>
<script>
const $ = id => document.getElementById(id);
const samples = Array(100).fill(0);
const trace = 'click-' + crypto.randomUUID().slice(0,8);
let polling = false;
$('host').textContent = location.host;
function draw(){
  const c=$('scope'), x=c.getContext('2d'), w=c.width, h=c.height;
  x.clearRect(0,0,w,h); x.strokeStyle='#1c333d'; x.lineWidth=1;
  for(let y=10;y<h;y+=40){x.beginPath();x.moveTo(0,y);x.lineTo(w,y);x.stroke();}
  x.beginPath();
  samples.forEach((v,i)=>{const a=i*w/(samples.length-1),b=h-10-Math.min(v,110)/110*(h-20);i?x.lineTo(a,b):x.moveTo(a,b);});
  x.strokeStyle='#69f0ac';x.lineWidth=3;x.stroke();
}
async function update(){
  if(polling)return;
  polling=true;
  try{
    const r=await fetch('/api',{cache:'no-store',headers:{'X-Lab-Trace':trace}});
    if(!r.ok)throw Error('HTTP '+r.status);
    const d=await r.json();
    $('cpu').textContent=d.worker_cpu.toFixed(1)+'%';
    $('processes').textContent=d.processes;
    $('memory').textContent=d.memory_used_mib+' MiB';
    $('linux').textContent=':'+d.port;
    $('status').className='muted';
    $('status').textContent=d.active?'Linux worker PID '+d.worker_pid+' is burning one CPU':'Idle / ready for a pulse';
    $('pulse').disabled=d.active;
    $('receipt').textContent=
      '$ uname -r\n'+d.kernel+'\n\n'+
      'hostname      '+d.hostname+'\n'+
      'server PID    '+d.server_pid+'\n'+
      'Linux socket  '+d.local+'\n'+
      'Linux peer    '+d.peer+'\n'+
      'request tag   '+d.trace+'\n'+
      'last pulse    '+(d.last_pulse||'none');
    samples.push(d.worker_cpu);samples.shift();draw();
  }catch(e){
    $('status').className='error';$('status').textContent='Connection lost: '+e.message;
    $('pulse').disabled=true;
  }finally{polling=false;}
}
$('pulse').onclick=async()=>{
  $('pulse').disabled=true;
  try{
    const r=await fetch('/pulse',{method:'POST',headers:{'X-Lab-Trace':trace}});
    if(!r.ok)throw Error('HTTP '+r.status);
    await update();
  }catch(e){$('status').className='error';$('status').textContent='Pulse failed: '+e.message;}
};
draw();update();setInterval(update,500);
</script>
</html>'''

lock = threading.Lock()
worker = None
previous_cpu = 0
previous_time = 0
last_pulse = ''


class Handler(BaseHTTPRequestHandler):
    def reply(self, status, body, content_type='application/json'):
        data = body.encode()
        self.send_response(status)
        self.send_header('Content-Type', content_type)
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        global previous_cpu, previous_time
        if self.path == '/':
            return self.reply(200, PAGE, 'text/html; charset=utf-8')
        if self.path != '/api':
            return self.reply(404, '{"error":"not found"}')
        with lock:
            active = worker is not None and worker.poll() is None
            cpu = 0.0
            now = time.monotonic()
            if active:
                try:
                    times = psutil.Process(worker.pid).cpu_times()
                    elapsed_cpu = times.user + times.system
                    cpu = 100 * (elapsed_cpu - previous_cpu) / (now - previous_time)
                    previous_cpu, previous_time = elapsed_cpu, now
                except psutil.NoSuchProcess:
                    active = False
            mem = psutil.virtual_memory()
            used = (mem.total - mem.available) // (1024 * 1024)
            data = dict(
                worker_cpu=round(cpu, 1), active=active,
                worker_pid=worker.pid if active else None,
                server_pid=os.getpid(), hostname=os.uname().nodename,
                kernel=os.uname().release, port=self.server.server_port,
                local=f'{self.connection.getsockname()[0]}:{self.connection.getsockname()[1]}',
                peer=f'{self.client_address[0]}:{self.client_address[1]}',
                processes=len(psutil.pids()),
                memory_used_mib=used, trace=self.headers.get('X-Lab-Trace', ''),
                last_pulse=last_pulse,
            )
        self.reply(200, json.dumps(data))

    def do_POST(self):
        global worker, previous_cpu, previous_time, last_pulse
        if self.path != '/pulse':
            return self.reply(404, '{"error":"not found"}')
        origin = self.headers.get('Origin')
        if origin and origin != 'http://' + self.headers.get('Host', ''):
            return self.reply(403, '{"error":"cross-origin pulse rejected"}')
        with lock:
            if worker is not None and worker.poll() is None:
                return self.reply(409, '{"error":"pulse already active"}')
            worker = subprocess.Popen([
                sys.executable, '-c',
                'import time\nend=time.monotonic()+4\nwhile time.monotonic()<end:\n pass\n',
            ])
            previous_cpu, previous_time = 0, time.monotonic()
            last_pulse = self.headers.get('X-Lab-Trace', 'untagged')[:80]
            print(f'PULSE trace={last_pulse!r} Linux PID={worker.pid} duration=4s', flush=True)
        self.reply(202, '{"status":"pulse started"}')

    def log_message(self, fmt, *args):
        if self.path != '/api':
            super().log_message(fmt, *args)


print('Kernel Radio listening on 0.0.0.0:8000', flush=True)
ThreadingHTTPServer(('0.0.0.0', 8000), Handler).serve_forever()
