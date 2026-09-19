/*
 * pages.h - 기기가 제공하는 두 개의 웹 페이지 (설정 포털 / 회의실 페이지)
 */
#pragma once
#include <Arduino.h>
#include <pgmspace.h>

// ---------------------------------------------------------------------------
// 1) Wi-Fi 설정 포털 (AP 모드에서 표시)
// ---------------------------------------------------------------------------
static const char SETUP_PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang="ko"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>회의실 카메라 설정</title>
<style>
:root{--bg:#0f172a;--card:#1e293b;--line:#334155;--fg:#e2e8f0;--mut:#94a3b8;--pri:#38bdf8;--ok:#22c55e;--warn:#f59e0b}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font-family:system-ui,-apple-system,"Malgun Gothic",sans-serif;padding:16px}
.wrap{max-width:520px;margin:0 auto}
h1{font-size:20px;margin:8px 0 4px}
.sub{color:var(--mut);font-size:13px;margin-bottom:16px}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:16px;margin-bottom:14px}
label{display:block;font-size:13px;color:var(--mut);margin:10px 0 4px}
input,select{width:100%;padding:11px 12px;border-radius:10px;border:1px solid var(--line);background:#0b1220;color:var(--fg);font-size:15px}
button{width:100%;padding:13px;border:0;border-radius:10px;background:var(--pri);color:#04293b;font-weight:700;font-size:15px;margin-top:14px;cursor:pointer}
button.ghost{background:transparent;color:var(--mut);border:1px solid var(--line);font-weight:500}
.row{display:flex;gap:8px;align-items:center}
.net{display:flex;justify-content:space-between;padding:10px 12px;border:1px solid var(--line);border-radius:10px;margin-bottom:6px;cursor:pointer;background:#0b1220}
.net:hover{border-color:var(--pri)}
.net small{color:var(--mut)}
.hide{display:none}
.big{font-size:26px;font-weight:800;color:var(--ok);word-break:break-all;text-align:center;margin:10px 0}
.big a{color:var(--ok)}
.state{text-align:center;padding:10px;color:var(--mut)}
.err{color:#f87171;font-size:13px;margin-top:8px}
.spin{display:inline-block;width:14px;height:14px;border:2px solid var(--line);border-top-color:var(--pri);border-radius:50%;animation:s 1s linear infinite;vertical-align:-2px}
@keyframes s{to{transform:rotate(360deg)}}
</style></head><body><div class="wrap">

<h1>회의실 카메라 설정</h1>
<div class="sub" id="dev">기기 정보 불러오는 중…</div>

<div id="form">
  <div class="card">
    <div class="row" style="justify-content:space-between">
      <strong style="font-size:15px">1. Wi-Fi 선택</strong>
      <button class="ghost" style="width:auto;margin:0;padding:7px 12px" onclick="scan()">다시 검색</button>
    </div>
    <div id="nets" style="margin-top:10px"><div class="state"><span class="spin"></span> 검색 중…</div></div>
    <label for="ssid">SSID</label>
    <input id="ssid" placeholder="접속할 공유기 이름">
    <label for="pass">비밀번호</label>
    <input id="pass" type="password" placeholder="비밀번호 (없으면 비워두세요)">
  </div>

  <div class="card">
    <strong style="font-size:15px">2. 회의실 정보</strong>
    <label for="roomId">회의실 ID (서버에서 구분하는 값, 영문/숫자)</label>
    <input id="roomId" placeholder="room-1">
    <label for="roomName">회의실 이름</label>
    <input id="roomName" placeholder="3층 대회의실">
    <label for="server">중앙 서버 주소 (없으면 비워두세요)</label>
    <input id="server" placeholder="http://192.168.0.10:3000">
    <label for="token">기기 인증 토큰</label>
    <input id="token" placeholder="서버의 DEVICE_TOKEN 과 동일하게">
    <button onclick="save()">저장하고 접속</button>
    <div class="err" id="err"></div>
  </div>
</div>

<div id="result" class="hide">
  <div class="card">
    <div id="rstate" class="state"><span class="spin"></span> 공유기에 접속 중입니다…</div>
    <div id="rip" class="hide">
      <div class="sub" style="text-align:center;margin:0">할당받은 IP 주소</div>
      <div class="big" id="ipval"></div>
      <div class="sub" style="text-align:center">같은 네트워크에 연결한 뒤 위 주소로 접속하면<br>회의실 화면과 예약 페이지를 볼 수 있습니다.</div>
      <div class="sub" style="text-align:center" id="mdns"></div>
    </div>
    <button class="ghost" onclick="location.reload()">설정 화면으로 돌아가기</button>
  </div>
</div>

<script>
const $=s=>document.querySelector(s);
async function j(u,o){const r=await fetch(u,o);return r.json()}

async function info(){
  try{
    const d=await j('/api/info');
    $('#dev').textContent='기기 '+d.ap+' · 펌웨어 '+d.fw+' · 카메라 '+(d.camera?'정상':'인식 안 됨');
    $('#roomId').value=d.roomId; $('#roomName').value=d.roomName;
    $('#server').value=d.server; $('#token').value=d.token; $('#ssid').value=d.ssid||'';
  }catch(e){}
}
async function scan(){
  $('#nets').innerHTML='<div class="state"><span class="spin"></span> 검색 중…</div>';
  try{
    const d=await j('/api/scan');
    if(!d.nets.length){$('#nets').innerHTML='<div class="state">검색된 네트워크가 없습니다.</div>';return}
    $('#nets').innerHTML=d.nets.map(n=>
      '<div class="net" onclick="pick(this)" data-s="'+n.ssid.replace(/"/g,'&quot;')+'">'+
      '<span>'+(n.lock?'🔒 ':'📶 ')+n.ssid+'</span><small>'+n.rssi+' dBm</small></div>').join('');
  }catch(e){$('#nets').innerHTML='<div class="state">검색 실패</div>'}
}
function pick(el){$('#ssid').value=el.dataset.s;$('#pass').focus()}

async function save(){
  $('#err').textContent='';
  const ssid=$('#ssid').value.trim();
  if(!ssid){$('#err').textContent='SSID를 입력하거나 목록에서 선택하세요.';return}
  const body=new URLSearchParams({ssid,pass:$('#pass').value,
    roomId:$('#roomId').value.trim()||'room-1',roomName:$('#roomName').value.trim()||'회의실',
    server:$('#server').value.trim(),token:$('#token').value.trim()});
  try{
    await fetch('/api/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});
  }catch(e){}
  $('#form').classList.add('hide'); $('#result').classList.remove('hide');
  poll();
}

async function poll(){
  try{
    const d=await j('/api/wifi/status');
    if(d.state==='connected'){
      $('#rstate').classList.add('hide');
      $('#rip').classList.remove('hide');
      $('#ipval').innerHTML='<a href="http://'+d.ip+'/">'+d.ip+'</a>';
      $('#mdns').textContent='mDNS 주소: http://'+d.host+'.local/';
      return;
    }
    if(d.state==='failed'){
      $('#rstate').innerHTML='접속에 실패했습니다. 비밀번호를 확인하고 다시 시도하세요.';
      setTimeout(()=>location.reload(),2500);
      return;
    }
  }catch(e){}
  setTimeout(poll,1200);
}
info();scan();
</script>
</div></body></html>)HTML";

// ---------------------------------------------------------------------------
// 2) 회의실 페이지 (STA 모드에서 할당받은 IP로 접속하면 보이는 화면)
// ---------------------------------------------------------------------------
static const char ROOM_PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang="ko"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>회의실 현황</title>
<style>
:root{--bg:#0f172a;--card:#1e293b;--line:#334155;--fg:#e2e8f0;--mut:#94a3b8;--pri:#38bdf8;--ok:#22c55e;--busy:#ef4444;--warn:#f59e0b}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font-family:system-ui,-apple-system,"Malgun Gothic",sans-serif;padding:14px}
.wrap{max-width:680px;margin:0 auto}
h1{font-size:21px;margin:4px 0}
.sub{color:var(--mut);font-size:13px}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:14px;margin:12px 0}
.badge{display:inline-flex;align-items:center;gap:6px;padding:6px 12px;border-radius:999px;font-weight:700;font-size:14px}
.b-busy{background:rgba(239,68,68,.15);color:var(--busy)}
.b-free{background:rgba(34,197,94,.15);color:var(--ok)}
.b-warn{background:rgba(245,158,11,.15);color:var(--warn)}
.dot{width:8px;height:8px;border-radius:50%;background:currentColor}
.shot{width:100%;border-radius:12px;background:#000;display:block;aspect-ratio:4/3;object-fit:contain}
.row{display:flex;gap:8px;flex-wrap:wrap}
.row>*{flex:1}
label{display:block;font-size:12px;color:var(--mut);margin:10px 0 4px}
input{width:100%;padding:10px;border-radius:9px;border:1px solid var(--line);background:#0b1220;color:var(--fg);font-size:15px}
button{padding:11px 14px;border:0;border-radius:9px;background:var(--pri);color:#04293b;font-weight:700;font-size:14px;cursor:pointer}
button.ghost{background:transparent;color:var(--mut);border:1px solid var(--line);font-weight:500}
button.danger{background:rgba(239,68,68,.15);color:#fca5a5}
ul{list-style:none;padding:0;margin:0}
li{display:flex;justify-content:space-between;align-items:center;gap:8px;padding:10px;border:1px solid var(--line);border-radius:10px;margin-bottom:8px;background:#0b1220}
li.now{border-color:var(--busy)}
li .t{font-variant-numeric:tabular-nums;font-weight:700}
li .m{color:var(--mut);font-size:12px}
.msg{font-size:13px;margin-top:8px;min-height:18px}
.msg.ok{color:var(--ok)} .msg.err{color:#f87171}
.kv{display:flex;justify-content:space-between;font-size:12px;color:var(--mut);padding:3px 0}
h2{font-size:15px;margin:0 0 10px}
</style></head><body><div class="wrap">

<div style="display:flex;justify-content:space-between;align-items:flex-end">
  <div><h1 id="rname">회의실</h1><div class="sub" id="rtime">-</div></div>
  <div id="rbadge"><span class="badge b-free"><span class="dot"></span>확인 중</span></div>
</div>

<div class="card">
  <img id="cam" class="shot" alt="회의실 화면">
  <div class="row" style="margin-top:10px">
    <button class="ghost" id="btnStream" onclick="toggleStream()">실시간 영상 켜기</button>
    <button class="ghost" onclick="shot()">사진 새로고침</button>
  </div>
  <div class="sub" id="camnote" style="margin-top:8px"></div>
</div>

<div class="card">
  <h2>지금 상태</h2>
  <div id="nowbox"></div>
</div>

<div class="card">
  <h2>오늘 예약</h2>
  <ul id="list"><li><span class="m">불러오는 중…</span></li></ul>
</div>

<div class="card">
  <h2>예약하기</h2>
  <div class="row">
    <div><label for="date">날짜</label><input id="date" type="date"></div>
  </div>
  <div class="row">
    <div><label for="start">시작</label><input id="start" type="time" step="300"></div>
    <div><label for="end">종료</label><input id="end" type="time" step="300"></div>
  </div>
  <label for="title">회의 제목</label>
  <input id="title" placeholder="예: 주간 팀 회의">
  <label for="user">예약자</label>
  <input id="user" placeholder="이름 또는 부서">
  <div class="row" style="margin-top:14px">
    <button onclick="book()">예약하기</button>
    <button class="ghost" onclick="quick()">지금 바로 사용</button>
  </div>
  <div class="msg" id="msg"></div>
</div>

<div class="card">
  <h2>기기 정보</h2>
  <div id="info"></div>
  <div class="row" style="margin-top:12px">
    <button class="ghost" onclick="dev('/api/restart','기기를 재시작할까요?')">재시작</button>
    <button class="danger" onclick="dev('/api/wifi/reset','Wi-Fi 설정을 지우고 설정 모드로 돌아갈까요?')">Wi-Fi 재설정</button>
  </div>
</div>

<script>
const $=s=>document.querySelector(s);
const host=location.hostname;
let streamOn=false, camPort=81;

function pad(n){return String(n).padStart(2,'0')}
function hm(m){return pad(Math.floor(m/60))+':'+pad(m%60)}

async function j(u,o){const r=await fetch(u,o);if(!r.ok)throw new Error(await r.text());return r.json()}

function shot(){
  if(streamOn)return;
  $('#cam').src='http://'+host+':'+camPort+'/capture?t='+Date.now();
}
function toggleStream(){
  streamOn=!streamOn;
  $('#btnStream').textContent=streamOn?'실시간 영상 끄기':'실시간 영상 켜기';
  if(streamOn)$('#cam').src='http://'+host+':'+camPort+'/stream';
  else{$('#cam').removeAttribute('src');shot();}
}

function badge(st){
  if(st.warmup)return '<span class="badge b-warn"><span class="dot"></span>센서 준비 중</span>';
  return st.occupied
    ? '<span class="badge b-busy"><span class="dot"></span>사용중</span>'
    : '<span class="badge b-free"><span class="dot"></span>비어있음</span>';
}

function nowBox(st){
  let h='';
  const cur=st.current, nx=st.next;
  if(cur){
    h+='<div><strong>'+esc(cur.title)+'</strong> · '+esc(cur.user)+'</div>'+
       '<div class="sub">'+hm(cur.start)+' ~ '+hm(cur.end)+' 예약 진행 중</div>';
  }else{
    h+='<div><strong>예약 없음</strong></div>';
  }
  if(st.occupied&&!cur)h+='<div class="sub" style="color:var(--warn);margin-top:6px">예약 없이 사용 중입니다.</div>';
  if(!st.occupied&&cur)h+='<div class="sub" style="color:var(--warn);margin-top:6px">예약 시간이지만 사람이 감지되지 않습니다.</div>';
  h+='<div class="sub" style="margin-top:6px">마지막 움직임: '+(st.lastMotion<0?'없음':agoText(st.lastMotion))+'</div>';
  if(nx)h+='<div class="sub">다음 예약: '+hm(nx.start)+' '+esc(nx.title)+'</div>';
  return h;
}
function agoText(s){
  if(s<60)return s+'초 전';
  if(s<3600)return Math.floor(s/60)+'분 전';
  return Math.floor(s/3600)+'시간 전';
}
function esc(s){return String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]))}

async function refresh(){
  try{
    const st=await j('/api/status');
    camPort=st.streamPort||81;
    $('#rname').textContent=st.roomName;
    $('#rtime').textContent=st.time+'  ·  '+st.roomId;
    $('#rbadge').innerHTML=badge(st);
    $('#nowbox').innerHTML=nowBox(st);
    $('#camnote').textContent=st.cameraReady?'':'카메라를 인식하지 못했습니다. 배선과 보드 설정을 확인하세요.';
    $('#info').innerHTML=[
      ['IP 주소',st.ip],['Wi-Fi',st.ssid+' ('+st.rssi+' dBm)'],
      ['중앙 서버',st.cloud.configured?(st.cloud.online?'연결됨':'연결 끊김')+' · '+esc(st.cloud.url):'사용 안 함'],
      ['마지막 동기화',st.cloud.configured?(st.cloud.lastSync<0?'없음':agoText(st.cloud.lastSync)):'-'],
      ['감지 횟수(부팅 후)',st.motionCount],
      ['동작 시간',Math.floor(st.uptime/3600)+'시간 '+Math.floor(st.uptime%3600/60)+'분'],
      ['펌웨어',st.fw]
    ].map(r=>'<div class="kv"><span>'+r[0]+'</span><span>'+r[1]+'</span></div>').join('');

    const list=st.reservations;
    $('#list').innerHTML=list.length?list.map(r=>{
      const cls=(st.nowMin>=r.start&&st.nowMin<r.end)?' class="now"':'';
      const state=st.nowMin>=r.end?'종료':(st.nowMin>=r.start?'진행중':'예정');
      return '<li'+cls+'><div><div class="t">'+hm(r.start)+' ~ '+hm(r.end)+'</div>'+
        '<div class="m">'+esc(r.title)+' · '+esc(r.user)+' · '+state+(r.synced?'':' · 동기화 대기')+'</div></div>'+
        '<button class="ghost" onclick="cancel(\''+r.id+'\')">취소</button></li>';
    }).join(''):'<li><span class="m">오늘 예약이 없습니다.</span></li>';

    if(!$('#date').value)$('#date').value=st.date;
  }catch(e){}
}

async function post(url,obj){
  const body=new URLSearchParams(obj);
  const r=await fetch(url,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});
  const t=await r.text();
  let d={};try{d=JSON.parse(t)}catch(e){}
  if(!r.ok||d.ok===false)throw new Error(d.error||'요청에 실패했습니다.');
  return d;
}
function say(t,ok){const m=$('#msg');m.textContent=t;m.className='msg '+(ok?'ok':'err');setTimeout(()=>{m.textContent=''},4000)}

async function book(){
  try{
    await post('/api/reservations',{date:$('#date').value,start:$('#start').value,end:$('#end').value,
      title:$('#title').value,user:$('#user').value});
    say('예약이 등록되었습니다.',true);
    $('#title').value='';
    refresh();
  }catch(e){say(e.message,false)}
}
async function quick(){
  try{ await post('/api/quickbook',{user:$('#user').value,title:$('#title').value});
    say('지금부터 사용 등록되었습니다.',true); refresh();
  }catch(e){say(e.message,false)}
}
async function cancel(id){
  if(!confirm('이 예약을 취소할까요?'))return;
  try{ await post('/api/reservations/cancel',{id}); say('예약을 취소했습니다.',true); refresh(); }
  catch(e){say(e.message,false)}
}
async function dev(url,q){
  if(!confirm(q))return;
  try{ await post(url,{}); say('명령을 보냈습니다. 잠시 후 다시 접속하세요.',true); }catch(e){say(e.message,false)}
}

refresh(); shot();
setInterval(refresh,5000);
setInterval(()=>{if(!streamOn)shot()},15000);
</script>
</div></body></html>)HTML";
