// 网页资源：HTML/CSS/JS 全部集中在这里
// 说明：不能放在 .ino 里——Arduino builder 会用 ctags 扫描 .ino 的函数定义，
//       HTML 里的 "function xxx(" 会被误判成 C++ 函数并插入原型，把原始字符串截断。
//       放到 .h 里就不参与扫描了。
#pragma once

// ============================================================
//  网页 · 配网页（未联网时首页就是它）
// ============================================================
const char WIFI_HTML[] = R"HTML(
<!DOCTYPE html><html lang="zh-CN"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>小米闹钟 · 连接 WiFi</title>
<style>
:root{--acc:#ff6a00}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
body{margin:0;padding:16px 14px 40px;background:#f2f2f7;color:#1c1c1e;
 font:16px/1.55 -apple-system,BlinkMacSystemFont,"PingFang SC","Microsoft YaHei",sans-serif}
.wrap{max-width:520px;margin:0 auto}
h1{font-size:22px;margin:6px 6px 2px}
.sub{color:#8e8e93;font-size:13px;margin:0 6px 14px;line-height:1.6}
.card{margin-top:12px;border-radius:16px;background:#fff;box-shadow:0 1px 4px rgba(0,0,0,.06);overflow:hidden}
.net{display:flex;align-items:center;gap:12px;padding:14px 16px;border-bottom:1px solid #f0f0f3;cursor:pointer}
.net:last-child{border-bottom:0}
.net:active{background:#f7f7fa}
.net b{flex:1;font-weight:500;font-size:15px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.bars{color:#a1a1a6;font-size:12px}
.lock{color:#8e8e93;font-size:12px}
.row{display:flex;align-items:center;gap:10px;padding:12px 16px;border-bottom:1px solid #f0f0f3}
.row label{flex:0 0 44px;color:#8e8e93;font-size:14px}
input{flex:1;min-width:0;border:0;background:#f2f2f7;border-radius:10px;padding:10px 12px;font-size:15px;color:inherit}
button{border:0;border-radius:12px;font-size:15px;font-weight:500;background:#f2f2f7;color:#1c1c1e;padding:10px 14px}
button.p{background:var(--acc);color:#fff;display:block;width:calc(100% - 32px);margin:14px 16px;padding:12px}
.msg{margin:14px 6px;font-size:14px;line-height:1.7;color:#8e8e93;word-break:break-all}
.msg.err{color:#d13d00}
.msg.ok{color:#1a8f3c}
.spin{display:inline-block;width:12px;height:12px;border:2px solid #c7c7cc;border-top-color:#ff6a00;
 border-radius:50%;animation:s .8s linear infinite;vertical-align:-2px;margin-right:6px}
@keyframes s{to{transform:rotate(360deg)}}
</style></head><body><div class="wrap">
<h1>连接 WiFi</h1>
<p class="sub">ESP32 只支持 2.4GHz，5G 网络请在路由器上单独开一个 2.4G 频段</p>
<div class="card" id="list"><div class="net"><b>正在扫描…</b></div></div>
<div class="card">
<div class="row"><label>名称</label><input id="s" placeholder="WiFi 名称" autocomplete="off"></div>
<div class="row"><label>密码</label><input id="p" type="password" placeholder="WiFi 密码"><button onclick="tog()" id="eye">显示</button></div>
<button class="p" onclick="go()">连接</button>
</div>
<p class="msg" id="m"></p>
</div>
<script>
var $=function(i){return document.getElementById(i)};
function tog(){var p=$('p');p.type=(p.type==='password')?'text':'password';$('eye').textContent=(p.type==='password')?'显示':'隐藏'}
function esc(s){return String(s).replace(/[&<>"]/g,function(c){return {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]})}
var scanTries=0;
function scan(){
 fetch('/scan',{cache:'no-store'}).then(function(r){return r.json()}).then(function(d){
  // 首轮后台扫描还没出结果：短暂重试（通常 1~2 秒内就有），别显示"没扫到"误导
  if(!d.n.length && d.busy && scanTries<20){
   scanTries++;
   $('list').innerHTML='<div class="net"><b>正在扫描…</b></div>';
   setTimeout(scan,400);return;
  }
  if(!d.n.length){$('list').innerHTML='<div class="net"><b>没扫到网络，请确认路由器 2.4G 已开启</b></div>';return}
  var h='';
  d.n.forEach(function(x){
   var lv=['','弱','中','良','强'][x.bars];
   h+='<div class="net" data-s="'+esc(x.s)+'" onclick="pick(this)"><b>'+esc(x.s)+'</b>'+
      '<span class="bars">'+lv+'</span><span class="lock">'+(x.e?'需密码':'开放')+'</span></div>';
  });
  $('list').innerHTML=h;
 }).catch(function(){$('list').innerHTML='<div class="net"><b>扫描失败，请刷新</b></div>'});
}
function pick(el){$('s').value=el.getAttribute('data-s');$('p').focus()}
function go(){
 var s=$('s').value.replace(/^\s+|\s+$/g,'');
 if(!s){$('m').className='msg err';$('m').textContent='请先选择或输入 WiFi 名称';return}
 $('m').className='msg';
 $('m').innerHTML='<span class="spin"></span>正在连接，最多等 20 秒…';
 fetch('/connect?ssid='+encodeURIComponent(s)+'&pass='+encodeURIComponent($('p').value))
  .then(function(){poll(0)});
}
function poll(n){
 fetch('/netstatus').then(function(r){return r.json()}).then(function(d){
  if(d.st==1){ if(n<26) setTimeout(function(){poll(n+1)},800); }
  else if(d.st==2){
   $('m').className='msg ok';
   $('m').innerHTML='连接成功：<b>'+esc(d.ssid)+'</b><br>IP 地址：'+d.ip+
    '<br><br>热点马上自动关闭，手机会自动切回家里的 WiFi，不用手动切换；'+
    '稍后用浏览器打开 <a href="http://'+d.ip+'/">http://'+d.ip+'</a> 进入设置。';
  }
  else if(d.st==3){ $('m').className='msg err'; $('m').textContent=d.msg; }
  else { $('m').className='msg'; $('m').textContent='已提交，正在等待结果…'; }
 }).catch(function(){});
}
scan();
</script>
</body></html>
)HTML";

const char PAGE_HTML[] = R"HTML(
<!DOCTYPE html><html lang="zh-CN"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>小米闹钟</title>
<style>
:root{--acc:#ff6a00}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
body{margin:0;padding:16px 14px 40px;background:#f2f2f7;color:#1c1c1e;
 font:16px/1.55 -apple-system,BlinkMacSystemFont,"PingFang SC","Microsoft YaHei",sans-serif}
.wrap{max-width:520px;margin:0 auto}
/* 退出按钮：浮在橙色时间卡右上角，点一下清会话并跳回验证码页。
   图标是「门框 + 向外的箭头」。卡片右侧留 58px，避免时间/日期钻到按钮底下。 */
.clock{position:relative;padding:18px 58px 18px 20px;border-radius:18px;color:#fff;
 background:linear-gradient(135deg,#ff9a3d,#ff5e00);
 box-shadow:0 8px 22px rgba(255,94,0,.28)}
.logout{position:absolute;top:19px;right:25px;display:inline-block;width:17px;height:20px;opacity:.92}
.logout:active{opacity:.55}
.logout svg{width:17px;height:20px;display:block}
.clock .t{font-size:42px;font-weight:600;letter-spacing:1px;font-variant-numeric:tabular-nums;line-height:1.15}
.clock .d{margin-top:2px;font-size:13px;opacity:.9}
.card{margin-top:14px;padding:4px 16px 8px;border-radius:16px;background:#fff;box-shadow:0 1px 4px rgba(0,0,0,.06)}
h3{margin:14px 0 6px;font-size:13px;font-weight:600;color:#8e8e93;letter-spacing:.6px}
.row{display:flex;align-items:center;gap:12px;padding:11px 0;border-bottom:1px solid #f0f0f3}
.row:last-child{border-bottom:0}
.row>label{flex:0 0 auto;font-size:15px}
input[type=range]{flex:1 1 auto;min-width:0;height:28px;accent-color:var(--acc)}
input[type=time],select{border:0;background:#f2f2f7;border-radius:10px;padding:8px 12px;
 font-size:15px;color:inherit;margin-left:auto}
/* 整点时段：起止两个下拉挤在同一行，别让 select 的 margin-left:auto 把它们推散 */
.hrrow{gap:8px}
select.hr{margin-left:0;flex:0 0 auto;padding:8px 10px}
.to{color:#8e8e93;font-size:14px;flex:0 0 auto}
.val{flex:0 0 40px;text-align:right;font-size:14px;color:#8e8e93;font-variant-numeric:tabular-nums}
.sw{position:relative;flex:0 0 48px;width:48px;height:28px;margin-left:auto}
.sw input{position:absolute;opacity:0;width:0;height:0}
.sw i{position:absolute;inset:0;background:#d9d9de;border-radius:999px;transition:.2s}
.sw i::after{content:"";position:absolute;top:3px;left:3px;width:22px;height:22px;border-radius:50%;
 background:#fff;transition:.2s;box-shadow:0 1px 3px rgba(0,0,0,.2)}
.sw input:checked+i{background:#34c759}
.sw input:checked+i::after{transform:translateX(20px)}
.btns{display:flex;gap:10px;flex-wrap:wrap;padding:12px 0 6px}
.arow{gap:8px}
.arow input[type=time]{padding:8px 10px}
.del{flex:0 0 22px;text-align:center;color:#c7c7cc;font-size:17px;line-height:1}
.empty{color:#8e8e93;font-size:14px;padding:12px 0}
button{flex:1 1 auto;min-width:100px;padding:11px 14px;border:0;border-radius:12px;
 background:#f2f2f7;color:#1c1c1e;font-size:15px;font-weight:500}
button.p{background:var(--acc);color:#fff}
.tip{margin:18px 6px 0;color:#8e8e93;font-size:13px;line-height:1.8}
.sign{margin:16px 0 0;text-align:center;color:#c1c1c6;font-size:13px;
 letter-spacing:2px;font-weight:500}
#toast{position:fixed;left:50%;bottom:26px;transform:translate(-50%,16px);padding:10px 20px;
 border-radius:999px;background:#1c1c1e;color:#fff;font-size:14px;opacity:0;transition:.25s;pointer-events:none}
#toast.on{opacity:1;transform:translate(-50%,0)}
/* 清除 WiFi 之后的整屏提示：这个页面马上就要失联了，必须把"接下来怎么办"讲清楚，
   否则用户看到的就是"点了按钮，设备死机不动" */
#bye{position:fixed;inset:0;z-index:99;background:#f7f7f9;display:none;
 flex-direction:column;justify-content:center;align-items:center;padding:28px;text-align:center}
#bye.on{display:flex}
#bye h2{margin:0 0 6px;font-size:20px;color:#1c1c1e}
#bye p{margin:7px 0;color:#6e6e73;font-size:14px;line-height:1.7;max-width:330px}
#bye b{color:#1c1c1e}
#bye .ap{display:inline-block;margin:8px 0 4px;padding:9px 20px;border-radius:999px;
 background:#e8f0fe;color:#1a56db;font-weight:600;font-size:15px}
#bye .note{margin-top:16px;color:#a1a1a6;font-size:12.5px}
</style></head><body><div class="wrap">

<div class="clock">
<a class="logout" href="/logout" title="退出登录" aria-label="退出登录"><svg viewBox="0 0 260 302" fill="#272636" aria-hidden="true"><path d="M0 0h251v17H17v268h234v17H0z"/><path d="M235 0h15v59h-15zM235 243h15v59h-15z"/><path d="M84 143h158v17H84z"/><path d="M176 80l83 71-83 71z"/></svg></a>
<div class="t" id="t">--:--</div><div class="d" id="d">连接中…</div></div>

<div class="card">
<h3>显示</h3>
<div class="row"><label>亮度</label><input type="range" id="br" min="1" max="7" value="{{BRIGHT}}" oninput="brv.textContent=this.value" onchange="save()"><span class="val" id="brv">{{BRIGHT}}</span></div>
<div class="row"><label>夜间时段调暗</label><label class="sw"><input type="checkbox" id="non" {{NON}} onchange="save()"><i></i></label></div>
<div class="row hrrow"><label>时段</label><select class="hr" id="ns" onchange="save()">{{NS_OPTS}}</select><span class="to">→</span><select class="hr" id="ne" onchange="save()">{{NE_OPTS}}</select></div>
<div class="row"><label>夜间亮度</label><input type="range" id="nbr" min="1" max="7" value="{{NBR}}" oninput="nbrv.textContent=this.value" onchange="save()"><span class="val" id="nbrv">{{NBR}}</span></div>
<div class="row"><label>24 小时制</label><label class="sw"><input type="checkbox" id="m24" {{M24}} onchange="m24Touch=1;save()"><i></i></label></div>
<p class="tip" style="margin:6px 0 0">按整点设置，可以跨零点，比如 22 点 → 07 点。这个时段内自动用「夜间亮度」，其余时间用上面的「亮度」。起止设成同一小时 = 不启用夜间调暗。</p>
</div>

<div class="card">
<h3>氛围灯（WS2812B ×2）</h3>
<div class="row"><label>开启</label><label class="sw"><input type="checkbox" id="rgbon" {{RGBON}} onchange="save()"><i></i></label></div>
<div class="row"><label>亮度</label><input type="range" id="rgbb" min="1" max="10" value="{{RGBB}}" oninput="rgbbv.textContent=this.value" onchange="save()"><span class="val" id="rgbbv">{{RGBB}}</span></div>
<p class="tip" style="margin:2px 0 0">平时暖白微光，夜间跟随时段自动压暗；等待配网时蓝闪、闹铃时红闪。两颗灯串在 GPIO10 上，显示同样颜色。</p>
</div>

<div class="card">
<h3>闹钟</h3>
<div id="alist"></div>
<div class="btns"><button id="addbtn" onclick="alarmAdd()">＋ 添加闹钟</button></div>
<div class="row"><label>贪睡时长</label><select id="sn" onchange="save()"><option value="0">不贪睡</option><option value="3">3 分钟</option><option value="5">5 分钟</option><option value="10">10 分钟</option></select></div>
</div>

<div class="card">
<h3>声音</h3>
<div class="row"><label>音量</label><input type="range" id="vol" min="1" max="10" value="{{VOL}}" oninput="volv.textContent=this.value" onchange="save()"><span class="val" id="volv">{{VOL}}</span></div>
<p class="tip" style="margin:2px 0 0">共 10 档，1 档最轻、10 档最响（线性映射 I2S 输出幅度）。没有静音档。</p>
<div class="row"><label>闹钟铃声</label><select id="atone" onchange="save()">{{ATONE_OPTS}}</select></div>
<p class="tip" style="margin:2px 0 0">6 段都是真实采样的铃声：<b>晨曦</b> 钟声琶音、<b>雷达</b> 急促脉冲、<b>木琴</b> 木质琶音、<b>广播</b> 四短一长授时、<b>信号</b> 三音上行、<b>渐强</b> 越逼越紧。到点循环响，按主键或点「立即静音」才停。</p>
<div class="row"><label>整点报时</label><label class="sw"><input type="checkbox" id="chime" {{CHIME}} onchange="save()"><i></i></label></div>
<div class="row hrrow"><label>报时时段</label><select class="hr" id="cs" onchange="save()">{{CS_OPTS}}</select><span class="to">→</span><select class="hr" id="ce" onchange="save()">{{CE_OPTS}}</select></div>
<p class="tip" style="margin:6px 0 0">开启后，整点前 5 秒用真人语音播报「现在时间 X 点整」（24 小时制）。按整点设置，只在这个时段内报时，可以跨零点（比如 22 点 → 07 点就只报夜里那几个整点）。起止设成同一小时 = 全天都报，要整个关掉用上面的开关。</p>
<div class="btns"><button onclick="act('/audiotest?t=alarm','试听闹钟')">试听闹钟</button><button onclick="act('/audiotest?t=chime','试听报时')">试听报时</button></div>
<div class="btns"><button onclick="act('/audiostop','已停止')">立即静音</button></div>
</div>

<div class="card">
<h3>系统</h3>
<div class="row"><label>WiFi</label><span class="val" style="flex:1;color:#1c1c1e;font-size:13px">{{WIFI}}</span></div>
<div class="row"><label>最近校时</label><span class="val" style="flex:1;color:#1c1c1e">{{SYNC}}</span></div>
<div class="btns">
<button class="p" onclick="act('/sync','校时中…')">立即校时</button>
<button onclick="location.href='/portal'">重新配网</button>
</div>
<div class="btns">
<button style="background:#ffe9e0;color:#d13d00" onclick="if(confirm('清除设备里保存的 WiFi 和全部设置，然后重启？\n\n清除后设备会重新开热点 XiaoMiClock，不再连原来的路由器。'))bye()">清除 WiFi 与全部设置</button>
</div>
</div>

<p class="tip">主按键：短按 停止响铃 / 夜间点亮 3 秒　·　中按 切换 12·24 小时<br>
主按键长按 3 秒 进入配网　·　另一按键长按 8 秒 清除 WiFi 与全部设置<br>
（长按到第 5 秒屏上会开始倒数 3·2·1，数到 0 才真正清除，中途松手即取消）<br>
闹钟最多 5 组，可分别设置重复的星期<br>
本板无 DS1302：断电后时间会丢，重新上电需联网校时（通常几秒）<br>
硬件：ESP32-S3 · MAX98357 (I2S) · ICS-43434 (I2S) · WS2812B ×2<br>
整点报时：SPIFFS 内置真人语音，播报「现在时间 X 点整」<br>
固件 {{FW}}　·　IP {{IP}}</p>

<p class="sign">Made by Dayu</p>
</div>
<div id="toast"></div>
<div id="bye">
 <h2>已清除，设备正在重启</h2>
 <p>热点会重新打开，名字是</p>
 <div class="ap">XiaoMiClock（无密码）</div>
 <p>请到手机 WiFi 列表里连上它，再打开 <b>http://192.168.4.1</b></p>
 <p class="note">设备不再连接原来的路由器，之前那个地址不会再响应——这是清除成功，不是死机。</p>
</div>
<script>
var $=function(i){return document.getElementById(i)};
/* m24Touch：用户这次是不是真的动过「24 小时制」开关。
   设备上中按也能切小时制，网页手里可能还是打开那一刻的旧值，
   照着提交就会把按键改的覆盖回去 —— 所以没动过就干脆不提交这个字段
   （固件是「带了参数才改」，缺参数保留原值）。
   saving：保存请求在路上时不让轮询回写开关，免得跟用户的手打架。 */
var m24Touch=0, saving=0;
function toast(m){var e=$('toast');e.textContent=m;e.classList.add('on');
 clearTimeout(e._t);e._t=setTimeout(function(){e.classList.remove('on')},1500)}
function save(){
 var p=new URLSearchParams({br:$('br').value,non:$('non').checked?1:0,nbr:$('nbr').value,
  ns:$('ns').value,ne:$('ne').value,
  sn:$('sn').value,vol:$('vol').value,chime:$('chime').checked?1:0,
  cs:$('cs').value,ce:$('ce').value,
  atone:$('atone').value,
  rgb:$('rgbon').checked?1:0,rgbb:$('rgbb').value});
 if(m24Touch) p.set('m24',$('m24').checked?1:0);
 saving=1;
 fetch('/save?'+p.toString()).then(function(r){return r.text()})
  .then(function(){m24Touch=0;saving=0;toast('已保存')})
  .catch(function(){saving=0;toast('保存失败')});
}
function act(u,m){fetch(u).then(function(){toast(m||'完成')}).catch(function(){toast('失败')})}
/* 清除 WiFi：先把整屏提示顶上去，再发请求。设备重启会让这个页面短暂失联，
   等它回来后自动刷新；如果用户是通过路由器访问的，就停在这屏提示上（地址已失效） */
function bye(){
 var b=$('bye'); b.classList.add('on');
 fetch('/resetwifi').catch(function(){});
 setTimeout(function(){
  var t=setInterval(function(){
   fetch('/api/status',{cache:'no-store'}).then(function(r){return r.text()})
    .then(function(){clearInterval(t);location.href='/'}).catch(function(){});
  },1500);
 },4500);
}
$('sn').value='{{SNOOZE}}';

/* ---------------- 闹钟列表 ---------------- */
var MAXA=5, AL=[];
var DAYS=[[127,'每天'],[31,'工作日'],[96,'周末'],[1,'周一'],[2,'周二'],[4,'周三'],
          [8,'周四'],[16,'周五'],[32,'周六'],[64,'周日']];
function pad2(n){return (n<10?'0':'')+n}
function alRow(i){
 var a=AL[i], row=document.createElement('div'); row.className='row arow';
 var t=document.createElement('input'); t.type='time';
 t.value=pad2(a.h)+':'+pad2(a.m); t.style.marginLeft='0';
 t.onchange=function(){var v=this.value.split(':');
   alSet(i,'h='+parseInt(v[0],10)+'&m='+parseInt(v[1],10))};
 var s=document.createElement('select');
 for(var k=0;k<DAYS.length;k++){
   var o=document.createElement('option'); o.value=DAYS[k][0]; o.textContent=DAYS[k][1];
   if(DAYS[k][0]==a.d) o.selected=true; s.appendChild(o);
 }
 s.onchange=function(){alSet(i,'d='+this.value)};
 var w=document.createElement('label'); w.className='sw'; w.style.marginLeft='0';
 var c=document.createElement('input'); c.type='checkbox'; c.checked=!!a.e;
 c.onchange=function(){alSet(i,'e='+(this.checked?1:0))};
 var ii=document.createElement('i'); w.appendChild(c); w.appendChild(ii);
 var x=document.createElement('span'); x.className='del'; x.textContent='✕';
 x.onclick=function(){ if(confirm('删除这个闹钟？')) alDo('/alarmdel?i='+i) };
 row.appendChild(t); row.appendChild(s); row.appendChild(w); row.appendChild(x);
 return row;
}
function alRender(){
 var box=$('alist'); box.innerHTML='';
 if(!AL.length){
   var e=document.createElement('div'); e.className='empty';
   e.textContent='还没有闹钟，点下面添加'; box.appendChild(e);
 }
 for(var i=0;i<AL.length;i++) box.appendChild(alRow(i));
}
function loadAlarms(){
 return fetch('/api/alarms').then(function(r){return r.json()})
  .then(function(a){AL=a;alRender()}).catch(function(){});
}
function alSet(i,q){
 fetch('/alarm?i='+i+'&'+q).then(function(r){return r.text()})
  .then(function(){return loadAlarms()})
  .then(function(){toast('已保存')}).catch(function(){toast('保存失败')});
}
function alDo(u){
 fetch(u).then(function(r){return r.text()})
  .then(function(){return loadAlarms()}).catch(function(){});
}
function alarmAdd(){
 if(AL.length>=MAXA){toast('最多 '+MAXA+' 组');return}
 alDo('/alarmadd');
}
loadAlarms();
/* 设备上中按切小时制时，网页这个开关原来会一直停在打开那一刻的状态，
   看起来就像"没同步"。每秒跟一次设备真值，只在真的不一样时才改写并提示 */
function syncM24(v){
 var c=$('m24');
 if(!c||saving||v===undefined||v===null) return;
 var want=!!v;
 if(c.checked!==want){
  c.checked=want;
  toast(want?'设备按键切换到 24 小时制':'设备按键切换到 12 小时制');
 }
}
/* 会话过期（页面放着不管超过 2 分钟）时接口会回 401，
   这时直接把页面送回验证页，而不是让时间栏显示 undefined */
function poll(){fetch('/api/status',{cache:'no-store'}).then(function(r){
  if(r.status===401){location.href='/login';throw 0}
  return r.json()})
 .then(function(s){$('t').textContent=s.t;$('d').textContent=s.d;syncM24(s.m24)})
 .catch(function(){})}
poll();setInterval(poll,1000);
</script>
</body></html>
)HTML";

// ============================================================
//  网页 · 验证页（v4.7.4 引入，v4.7.6 改成「主动获取」，v4.7.7 优化交互）
//  打开任何设置页之前都先到这里，输设备屏幕上的 4 位数字。
//  提交走 fetch，失败原地提示，不整页重载 —— 重载会清掉已输入的 4 位数字，
//  手机上看起来就是"卡了一下然后重来"。
//  原生 form POST 仍然保留：万一 JS 出问题，退回去还能用。
// ============================================================
const char LOGIN_HTML[] = R"HTML(
<!DOCTYPE html><html lang="zh-CN"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>小米闹钟 · 验证</title>
<style>
:root{--acc:#ff6a00}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
body{margin:0;padding:16px 14px 40px;background:#f2f2f7;color:#1c1c1e;
 font:16px/1.55 -apple-system,BlinkMacSystemFont,"PingFang SC","Microsoft YaHei",sans-serif}
.wrap{max-width:520px;margin:0 auto}
h1{font-size:22px;margin:26px 6px 2px}
.sub{color:#8e8e93;font-size:13px;margin:0 6px 14px;line-height:1.7}
.card{margin-top:12px;border-radius:16px;background:#fff;box-shadow:0 1px 4px rgba(0,0,0,.06);
 padding:22px 16px;text-align:center}
input{width:100%;border:0;background:#f2f2f7;border-radius:12px;padding:14px 12px;
 font-size:34px;letter-spacing:.42em;text-align:center;font-variant-numeric:tabular-nums;
 color:inherit;text-indent:.42em;-webkit-appearance:none}
input:focus{outline:2px solid var(--acc);outline-offset:0}
button{border:0;border-radius:12px;font-size:16px;font-weight:500;display:block;
 width:100%;margin:10px 0 0;padding:13px;transition:opacity .12s}
button:active{opacity:.85}
button:disabled{opacity:.45}
.renew{background:#f2f2f7;color:#1c1c1e;margin-top:12px}
.go{background:var(--acc);color:#fff;margin-top:10px}
.hint{min-height:22px;margin:12px 2px 0;font-size:13px;color:#1a8f3c;text-align:center}
.hint.warn{color:#d13d00}
.msg{margin:14px 6px;font-size:13px;line-height:1.8;color:#8e8e93}
.msg.err{color:#d13d00;font-weight:500;margin-top:16px}
.sign{margin:22px 0 0;text-align:center;color:#c1c1c6;font-size:13px;
 letter-spacing:.04em}
</style></head><body><div class="wrap">
<h1>需要验证</h1>
<p class="sub">点下面的「获取验证码」，闹钟屏幕会显示 4 位数字，输入它才能进入设置页。<br>
打开这一页本身不会打扰闹钟 —— 只有你主动点了才会亮。</p>
<div class="card">
<form method="post" action="/login" id="f">
<input id="pin" name="pin" inputmode="numeric" pattern="[0-9]{4}" maxlength="4"
 autocomplete="off" placeholder="0000" autofocus>
<button type="button" class="renew" id="getpin" onclick="getPin()">获取验证码</button>
<button type="submit" class="go" id="go">进入</button>
</form>
<p class="hint" id="hint"></p>
</div>
<p class="msg err">{{ERR}}</p>
<p class="msg">数字在屏幕上亮 30 秒，用完立即作废。两次获取之间要隔 60 秒。<br>
这是故意的 —— 只有能看到这台机器的人才能改设置。</p>
<p class="sign">Made by Dayu</p>
</div>
<script>
var $=function(i){return document.getElementById(i)};
var cool={{COOL}}, timer=null, busy=0;
function paint(){
 var b=$('getpin');
 if(cool>0){b.disabled=true;b.textContent=cool+' 秒后可再获取';}
 else{b.disabled=false;b.textContent='获取验证码';}
}
function startCool(s){
 cool=s; paint();
 clearInterval(timer);
 timer=setInterval(function(){
  cool--; if(cool<=0){cool=0;clearInterval(timer);} paint();
 },1000);
}
function say(t, warn){
 var e=$('hint'); e.textContent=t;
 e.className='hint'+(warn?' warn':'');
}
/* 点了立刻变字样，别让用户以为没点上 —— 这是"卡"的主要来源 */
function getPin(){
 if(busy) return;
 var b=$('getpin');
 busy=1; b.disabled=true; b.textContent='获取中…';
 fetch('/login?get=1',{cache:'no-store'}).then(function(r){return r.json()})
  .then(function(d){
   busy=0;
   if(d.e==='wait'){ startCool(d.s||1); say('还没到时间，'+d.s+' 秒后再点', true); return; }
   startCool(d.cool||60);
   say('已发出，看闹钟屏幕（亮 '+(d.pin||30)+' 秒）');
   $('pin').focus();
  })
  .catch(function(){
   busy=0; b.disabled=false; b.textContent='获取验证码';
   say('没连上，再点一次', true);
  });
}
function submitPin(ev){
 ev.preventDefault();
 if(busy) return;
 var v=$('pin').value;
 if(!/^[0-9]{4}$/.test(v)){ say('请输入 4 位数字', true); $('pin').focus(); return; }
 var g=$('go');
 busy=1; g.disabled=true; g.textContent='验证中…';
 fetch('/login',{method:'POST',
  headers:{'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8'},
  body:'pin='+encodeURIComponent(v)+'&ajax=1'})
  .then(function(r){return r.json()})
  .then(function(d){
   busy=0;
   if(d.ok){ location.replace('/'); return; }   /* replace：别在历史里留验证页 */
   g.disabled=false; g.textContent='进入';
   say(d.m||'验证没通过', true);
   if(d.e==='expired'){ $('pin').value=''; }
  })
  .catch(function(){
   busy=0; g.disabled=false; g.textContent='进入';
   say('没连上，再点一次', true);
  });
}
$('f').addEventListener('submit', submitPin);
/* 服务端已经把剩余冷却秒数渲染进 {{COOL}}，刷新页面也绕不过去 */
if(cool>0) startCool(cool);
</script>
</body></html>
)HTML";
