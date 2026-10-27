/*
  Buzzer quiz game – ESP32 version with a live web scoreboard
  ------------------------------------------------------------
  Ported from buzzer_game_lcdV2_Ver0.ino (Arduino Uno/Nano).

  What changed for the ESP32:
    * SoftwareSerial  -> HardwareSerial Serial2 (GPIO16 RX / GPIO17 TX) for the DY audio player
    * Analog pins A0–A5 -> ESP32 GPIOs (see the pin table below)
    * I2C LCD now on the ESP32 default I2C pins (SDA 21 / SCL 22)
    * The game loop is now non-blocking (no while() waiting for the host),
      so the web server keeps answering while a player is buzzed in
    * Built-in web server: big Red / Yellow / Blue / Green scoreboard,
      plus optional host controls (Correct / Wrong / Reopen / Reset)
    * Player names per colour button, set from the web page (host bar -> Player names).
      Names are saved in flash (Preferences/NVS) so they survive a reboot.
      They are shown on the web page only – the LCD still shows the colours.

  Libraries (Arduino IDE, board package "esp32" by Espressif):
    * DYPlayer            https://github.com/SnijderC/dyplayer
    * LiquidCrystal I2C   by Frank de Brabander
    * WiFi, WebServer, ESPmDNS, Preferences – included with the ESP32 core

  Players / colours:
    Player 1 = Red, Player 2 = Yellow, Player 3 = Blue, Player 4 = Green

  ESP32 wiring
    Function          GPIO   Notes
    Red button         32    to GND (internal pull-up)
    Yellow button      33    to GND
    Blue button        25    to GND
    Green button       26    to GND
    Red LED            18    via ~220 Ω resistor to GND
    Yellow LED         19
    Blue LED           23
    Green LED          27
    Host "Correct"     13    to GND
    Host "Wrong"       14    to GND
    LCD SDA            21
    LCD SCL            22
    DY player TX ->    16    (ESP32 RX2)
    DY player RX <-    17    (ESP32 TX2)
  Avoid GPIO 34–39 for buttons (no internal pull-ups) and GPIO 0/2/12/15 (boot strapping pins).
*/

// INCLUDES
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <DYPlayerArduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ---------------- Wi-Fi SETTINGS ----------------
// Option A (default): the ESP32 makes its own Wi-Fi network – works anywhere, no router needed.
//   Join "Buzzer-Game" (password below), then open http://192.168.4.1
// Option B: join your home Wi-Fi. Set USE_HOME_WIFI to true and fill in the details.
//   Then open http://buzzer.local (or the IP shown on the LCD).
//   If it can't connect within 15 s it falls back to Option A automatically.
const bool  USE_HOME_WIFI = false;
const char* HOME_SSID     = "YourWiFiName";
const char* HOME_PASS     = "YourWiFiPassword";
const char* AP_SSID       = "Buzzer-Game";
const char* AP_PASS       = "letsplay123";   // must be at least 8 characters
const char* MDNS_NAME     = "buzzer";        // -> http://buzzer.local

// CONSTANTS
const int numPlayers = 4;
const int buttonPins[numPlayers] = {32, 33, 25, 26}; // Red, Yellow, Blue, Green
const int ledPins[numPlayers]    = {18, 19, 23, 27}; // Red, Yellow, Blue, Green
const char* colourNames[numPlayers] = {"Red", "Yellow", "Blue", "Green"};
const size_t maxNameBytes = 24; // up to 16 characters typed on the web page (more room for accented letters / emoji)

const int correctButtonPin = 13; // Host button: correct answer (also reopens buzzers when idle)
const int wrongButtonPin   = 14; // Host button: wrong answer

// DY player serial pins (Serial2)
const int dyRxPin = 16;
const int dyTxPin = 17;

// LCD settings
const uint8_t lcdAddress = 0x27; // Try 0x3F if nothing shows up
const uint8_t lcdColumns = 16;
const uint8_t lcdRows = 2;

// Sound effect file numbers
const int sfxBuzzer[numPlayers] = {4, 4, 4, 4};
const int sfxIncorrect = 2;
const int sfxCorrect   = 3;
const int sfxIntro     = 1;

// Timing
const unsigned long debounceMs   = 30;
const unsigned long rebuzzLockMs = 400; // ignore buzzers briefly after a ruling so a held button doesn't re-buzz

// GLOBALS
int8_t  playerWhoBuzzed = -1;
bool    playerActive[numPlayers] = {true, true, true, true};
int16_t playerScore[numPlayers]  = {0, 0, 0, 0};
unsigned long buzzLockUntil = 0;
String  playerNames[numPlayers]; // custom names; empty = show the colour name
Preferences prefs;

DY::Player player(&Serial2);
LiquidCrystal_I2C lcd(lcdAddress, lcdColumns, lcdRows);
WebServer server(80);

// Simple debounced "button was just pressed" detector for the host buttons
struct Button {
  uint8_t pin;
  bool lastState;
  unsigned long lastChange;
};
Button correctBtn = {correctButtonPin, HIGH, 0};
Button wrongBtn   = {wrongButtonPin,   HIGH, 0};

bool justPressed(Button &b) {
  bool reading = digitalRead(b.pin);
  if (reading != b.lastState && millis() - b.lastChange > debounceMs) {
    b.lastChange = millis();
    b.lastState = reading;
    return reading == LOW;
  }
  return false;
}

// ---------------- WEB PAGE ----------------
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<title>Buzz! Quiz scoreboard</title>
<style>
:root{
  --bg:#24104a; --bg2:#3a1a72; --ray:rgba(255,255,255,.035);
  --ink:#fff7e8; --ink-soft:rgba(255,247,232,.7);
  --bar:rgba(20,6,44,.55); --bar-line:rgba(255,255,255,.12);
  --red:#ff3b4e; --red-d:#b3142a;
  --yel:#ffc93c; --yel-d:#c98a00;
  --blu:#2f80ff; --blu-d:#1747a8;
  --grn:#2fd07a; --grn-d:#128a4a;
  color-scheme:dark;
  box-sizing:border-box;
  padding-top:env(safe-area-inset-top,0px);
  padding-bottom:env(safe-area-inset-bottom,0px);
}
@media (prefers-color-scheme: light){
  :root:not([data-theme="dark"]){
    --bg:#f6ecff; --bg2:#e4cffd; --ray:rgba(90,30,160,.05);
    --ink:#2a0f55; --ink-soft:rgba(42,15,85,.7);
    --bar:rgba(255,255,255,.7); --bar-line:rgba(42,15,85,.15);
    color-scheme:light;
  }
}
:root[data-theme="light"]{
  --bg:#f6ecff; --bg2:#e4cffd; --ray:rgba(90,30,160,.05);
  --ink:#2a0f55; --ink-soft:rgba(42,15,85,.7);
  --bar:rgba(255,255,255,.7); --bar-line:rgba(42,15,85,.15);
  color-scheme:light;
}
:root[data-theme="dark"]{ color-scheme:dark; }
*,*::before,*::after{box-sizing:inherit}
html{height:100%;scroll-padding-top:env(safe-area-inset-top,0px);background:var(--bg)}
body{
  height:100%;margin:0;color:var(--ink);overflow:hidden;
  font-family:ui-rounded,"SF Pro Rounded","Arial Rounded MT Bold","Nunito","Varela Round","Trebuchet MS",system-ui,sans-serif;
  background:
    repeating-conic-gradient(from 0deg at 50% 115%, var(--ray) 0 6deg, transparent 6deg 12deg),
    radial-gradient(ellipse at 50% 120%, var(--bg2), var(--bg) 70%);
  display:flex;flex-direction:column;
  -webkit-tap-highlight-color:transparent;
}
header{
  display:flex;align-items:center;gap:.75rem;
  padding:.8rem clamp(.8rem,3vw,1.6rem) .2rem;
}
.logo{
  margin:0;font-size:clamp(1.8rem,5vw,2.8rem);font-weight:900;letter-spacing:-.02em;line-height:1;
  transform:rotate(-4deg);
  text-shadow:0 3px 0 var(--red),0 6px 0 var(--yel),0 9px 0 var(--blu),0 12px 0 var(--grn);
  padding-bottom:12px;
}
.status{
  flex:1;min-width:0;font-size:clamp(1rem,2.6vw,1.4rem);font-weight:800;
  white-space:nowrap;overflow:hidden;text-overflow:ellipsis;
  padding-left:.5rem;
}
.tools{display:flex;align-items:center;gap:.4rem}
.conn{display:flex;align-items:center;gap:.4rem;font-size:.85rem;font-weight:700;color:var(--ink-soft);margin-right:.3rem}
.dot{width:.65rem;height:.65rem;border-radius:50%;background:#999}
.conn[data-s="live"] .dot{background:var(--grn);box-shadow:0 0 0 0 var(--grn);animation:ping 2s infinite}
.conn[data-s="lost"] .dot{background:var(--red)}
.conn[data-s="demo"] .dot{background:var(--yel)}
@keyframes ping{70%{box-shadow:0 0 0 .5rem transparent}100%{box-shadow:0 0 0 0 transparent}}
.icon-btn{
  font:inherit;font-size:1.15rem;width:2.6rem;height:2.6rem;border-radius:50%;
  border:2px solid var(--bar-line);background:var(--bar);color:var(--ink);cursor:pointer;
}
.icon-btn[aria-pressed="true"]{border-color:var(--yel)}

main{
  flex:1;min-height:0;display:grid;gap:clamp(.7rem,2vw,1.3rem);
  grid-template-columns:1fr 1fr;grid-template-rows:1fr 1fr;
  padding:clamp(.7rem,2vw,1.3rem) clamp(.8rem,3vw,1.6rem) calc(clamp(.7rem,2vw,1.3rem) + 10px);
}
@media (min-aspect-ratio:16/10) and (min-width:900px){
  main{grid-template-columns:repeat(4,1fr);grid-template-rows:1fr}
}

/* The arcade pads */
.pad{
  --c:var(--red);--d:var(--red-d);
  position:relative;font:inherit;color:#fff;border:0;cursor:default;
  border-radius:clamp(1.2rem,3vw,2.2rem);
  background:
    radial-gradient(circle at 30% 22%, rgba(255,255,255,.55), transparent 32%),
    radial-gradient(circle at 50% 60%, var(--c), var(--d) 120%);
  box-shadow:0 10px 0 var(--d),0 18px 30px rgba(0,0,0,.35),inset 0 -8px 0 rgba(0,0,0,.12);
  display:flex;flex-direction:column;align-items:center;justify-content:center;
  overflow:hidden;min-height:0;
  transition:transform .18s cubic-bezier(.3,1.6,.6,1),opacity .25s,filter .25s,box-shadow .18s;
}
.pad.y{--c:var(--yel);--d:var(--yel-d);color:#4a2c00}
.pad.b{--c:var(--blu);--d:var(--blu-d)}
.pad.g{--c:var(--grn);--d:var(--grn-d);color:#063a1f}
body.demo .pad{cursor:pointer}
body.demo .pad:active{transform:translateY(8px);box-shadow:0 2px 0 var(--d),0 6px 12px rgba(0,0,0,.3)}
.pad:focus-visible{outline:4px solid var(--ink);outline-offset:4px}

.pad .name{
  position:absolute;top:.7em;left:.9em;font-size:clamp(1.1rem,3vw,2rem);font-weight:900;
}
.pad .who{
  position:absolute;top:1.2em;right:1.2em;font-size:clamp(.75rem,1.6vw,1rem);font-weight:700;opacity:.8;
}
.pad .score{
  font-size:clamp(4rem,min(18vw,22vh),11rem);font-weight:900;line-height:1;
  font-variant-numeric:tabular-nums;letter-spacing:-.04em;
  text-shadow:0 .06em 0 rgba(0,0,0,.18);
}
.pad .score.pop{animation:pop .5s cubic-bezier(.3,1.8,.5,1)}
@keyframes pop{0%{transform:scale(1)}35%{transform:scale(1.35)}100%{transform:scale(1)}}
.pad .tag{
  position:absolute;bottom:.9em;left:50%;transform:translateX(-50%);
  font-size:clamp(.8rem,1.8vw,1.1rem);font-weight:800;white-space:nowrap;
  padding:.3em .9em;border-radius:99px;background:rgba(0,0,0,.28);color:#fff;
  opacity:0;transition:opacity .2s;
}
.pad .crown{
  position:absolute;top:.35em;left:50%;transform:translateX(-50%) rotate(-8deg);
  font-size:clamp(1.6rem,4vw,2.6rem);opacity:0;transition:opacity .3s;
  filter:drop-shadow(0 3px 0 rgba(0,0,0,.25));
}
.pad.leader .crown{opacity:1}

/* states */
.pad.locked{filter:grayscale(.85) brightness(.75);opacity:.55}
.pad.locked .tag{opacity:1}
.pad.dim{opacity:.3;filter:saturate(.5)}
.pad.buzzed{transform:scale(1.04);z-index:2;box-shadow:0 10px 0 var(--d),0 0 0 6px var(--ink),0 0 60px 10px var(--c)}
.pad.buzzed::before{
  content:"";position:absolute;inset:-50%;
  background:repeating-conic-gradient(rgba(255,255,255,.22) 0 10deg,transparent 10deg 20deg);
  animation:spin 6s linear infinite;
}
.pad.buzzed .tag{opacity:1;background:#fff;color:#2a0f55}
.pad > *{position:relative}
.pad .name,.pad .who,.pad .tag,.pad .crown{position:absolute}
@keyframes spin{to{transform:rotate(360deg)}}
.pad.shake{animation:shake .45s}
@keyframes shake{20%{translate:-12px 0}40%{translate:10px 0}60%{translate:-7px 0}80%{translate:4px 0}}

.pad .float{
  position:absolute;left:50%;top:40%;font-weight:900;font-size:clamp(2rem,6vw,4rem);
  pointer-events:none;animation:floatUp 1s ease-out forwards;text-shadow:0 3px 0 rgba(0,0,0,.25)
}
@keyframes floatUp{from{transform:translate(-50%,0);opacity:1}to{transform:translate(-50%,-120%);opacity:0}}
.confetti{position:fixed;width:10px;height:14px;border-radius:2px;pointer-events:none;z-index:50}

/* Host bar */
.host{
  display:none;gap:.6rem;justify-content:center;flex-wrap:wrap;
  padding:.7rem clamp(.8rem,3vw,1.6rem) calc(.8rem + env(safe-area-inset-bottom,0px));
  background:var(--bar);border-top:1px solid var(--bar-line);backdrop-filter:blur(8px);
}
body.host-open .host{display:flex}
.host button{
  font:inherit;font-weight:800;font-size:1.05rem;border:0;border-radius:99px;
  padding:.75rem 1.3rem;cursor:pointer;color:#fff;
  box-shadow:0 5px 0 rgba(0,0,0,.3);transition:transform .1s;
}
.host button:active{transform:translateY(4px);box-shadow:0 1px 0 rgba(0,0,0,.3)}
.host button:disabled{opacity:.35;cursor:not-allowed}
.host button:focus-visible{outline:3px solid var(--ink);outline-offset:3px}
#bCorrect{background:var(--grn-d)}
#bWrong{background:var(--red-d)}
#bReopen{background:var(--blu-d)}
#bZero{background:#5b4a7a}
#bZero.arm{background:#e0115f}
.hint{width:100%;text-align:center;font-size:.85rem;color:var(--ink-soft);margin:0}

#bNames{background:#7a3fd1}
.pad .name{max-width:calc(100% - 7em);overflow:hidden;text-overflow:ellipsis;white-space:nowrap}

/* Player names dialog */
dialog{
  border:0;border-radius:1.6rem;padding:1.4rem 1.4rem 1.2rem;width:min(26rem,calc(100vw - 2rem));
  background:var(--bg);color:var(--ink);box-shadow:0 20px 60px rgba(0,0,0,.5);
}
dialog::backdrop{background:rgba(10,0,30,.6);backdrop-filter:blur(3px)}
dialog h2{margin:0 0 .3rem;font-size:1.6rem;font-weight:900}
dialog p{margin:0 0 1rem;color:var(--ink-soft);font-size:.95rem}
.nrow{display:flex;align-items:center;gap:.7rem;margin-bottom:.7rem}
.sw{flex:none;width:2.6rem;height:2.6rem;border-radius:50%;box-shadow:0 4px 0 rgba(0,0,0,.3),inset 0 -4px 0 rgba(0,0,0,.15)}
.nrow input{
  flex:1;min-width:0;font:inherit;font-size:1.1rem;font-weight:700;padding:.65rem .9rem;
  border-radius:.9rem;border:2px solid var(--bar-line);background:var(--bar);color:var(--ink);
}
.nrow input:focus-visible{outline:3px solid var(--yel);outline-offset:1px}
.dlg-btns{display:flex;gap:.6rem;justify-content:flex-end;margin-top:1rem}
.dlg-btns button{font:inherit;font-weight:800;font-size:1rem;border:0;border-radius:99px;padding:.7rem 1.3rem;cursor:pointer}
#nSave{background:var(--grn-d);color:#fff}
#nClear{background:transparent;color:var(--ink-soft);margin-right:auto;padding-left:0}
#nCancel{background:var(--bar);color:var(--ink);border:2px solid var(--bar-line)}

@media (prefers-reduced-motion:reduce){
  *{animation:none!important;transition:none!important}
}
</style>
</head>
<body>
<header>
  <h1 class="logo">Buzz!</h1>
  <div class="status" id="status" aria-live="polite">Connecting…</div>
  <div class="tools">
    <div class="conn" id="conn" data-s="wait"><span class="dot"></span><span id="connTxt">…</span></div>
    <button class="icon-btn" id="snd" aria-pressed="false" title="Sound effects">🔇</button>
    <button class="icon-btn" id="hostBtn" aria-pressed="false" title="Host controls">🎤</button>
  </div>
</header>

<main>
  <button class="pad r" data-i="0"><span class="crown">👑</span><span class="name">Red</span><span class="who">Player 1</span><span class="score">0</span><span class="tag">Locked out</span></button>
  <button class="pad y" data-i="1"><span class="crown">👑</span><span class="name">Yellow</span><span class="who">Player 2</span><span class="score">0</span><span class="tag">Locked out</span></button>
  <button class="pad b" data-i="2"><span class="crown">👑</span><span class="name">Blue</span><span class="who">Player 3</span><span class="score">0</span><span class="tag">Locked out</span></button>
  <button class="pad g" data-i="3"><span class="crown">👑</span><span class="name">Green</span><span class="who">Player 4</span><span class="score">0</span><span class="tag">Locked out</span></button>
</main>

<div class="host" id="host">
  <button id="bCorrect">✔ Correct</button>
  <button id="bWrong">✘ Wrong</button>
  <button id="bReopen">↻ Reopen buzzers</button>
  <button id="bNames">✎ Player names</button>
  <button id="bZero">Reset scores</button>
  <p class="hint" id="hint">Keys: C correct, W wrong, R reopen</p>
</div>

<dialog id="nameDlg" aria-labelledby="nTitle">
  <h2 id="nTitle">Player names</h2>
  <p>Leave a box empty to show the colour instead.</p>
  <div class="nrow"><span class="sw" style="background:var(--red)"></span><input id="n0" maxlength="16" placeholder="Red" aria-label="Red player name" autocomplete="off"></div>
  <div class="nrow"><span class="sw" style="background:var(--yel)"></span><input id="n1" maxlength="16" placeholder="Yellow" aria-label="Yellow player name" autocomplete="off"></div>
  <div class="nrow"><span class="sw" style="background:var(--blu)"></span><input id="n2" maxlength="16" placeholder="Blue" aria-label="Blue player name" autocomplete="off"></div>
  <div class="nrow"><span class="sw" style="background:var(--grn)"></span><input id="n3" maxlength="16" placeholder="Green" aria-label="Green player name" autocomplete="off"></div>
  <div class="dlg-btns">
    <button id="nClear" type="button">Clear all</button>
    <button id="nCancel" type="button">Cancel</button>
    <button id="nSave" type="button">Save names</button>
  </div>
</dialog>

<script>
(function(){
  var NAMES=['Red','Yellow','Blue','Green'];
  var COLS=['#ff3b4e','#ffc93c','#2f80ff','#2fd07a'];
  var pads=[].slice.call(document.querySelectorAll('.pad'));
  var $=function(id){return document.getElementById(id)};
  var reduce=matchMedia('(prefers-reduced-motion: reduce)').matches;
  var cur=null, demo=false, connected=false, fails=0;
  function nm(s,i){ var n=(s.names&&s.names[i]||'').trim(); return n||NAMES[i]; }

  /* ---------- sound (Web Audio, off by default) ---------- */
  var ac=null, soundOn=false;
  function tone(f,d,type,when){
    if(!soundOn||!ac) return;
    var t=ac.currentTime+(when||0), o=ac.createOscillator(), g=ac.createGain();
    o.type=type||'square'; o.frequency.setValueAtTime(f,t);
    g.gain.setValueAtTime(.18,t); g.gain.exponentialRampToValueAtTime(.001,t+d);
    o.connect(g); g.connect(ac.destination); o.start(t); o.stop(t+d);
  }
  var sfx={
    buzz:function(){tone(880,.12,'square');tone(1320,.25,'square',.1)},
    right:function(){tone(523,.15,'triangle');tone(659,.15,'triangle',.12);tone(784,.35,'triangle',.24)},
    wrong:function(){tone(180,.5,'sawtooth');tone(140,.6,'sawtooth',.05)}
  };
  $('snd').onclick=function(){
    soundOn=!soundOn;
    if(soundOn&&!ac){try{ac=new (window.AudioContext||window.webkitAudioContext)()}catch(e){}}
    if(ac&&ac.state==='suspended') ac.resume();
    this.textContent=soundOn?'🔊':'🔇'; this.setAttribute('aria-pressed',soundOn);
  };

  /* ---------- host bar ---------- */
  function setHost(open){
    document.body.classList.toggle('host-open',open);
    $('hostBtn').setAttribute('aria-pressed',open);
    try{localStorage.setItem('buzzHost',open?'1':'0')}catch(e){}
  }
  $('hostBtn').onclick=function(){setHost(!document.body.classList.contains('host-open'))};
  try{ if(localStorage.getItem('buzzHost')==='1') setHost(true) }catch(e){}

  /* ---------- effects ---------- */
  function floatText(i,txt){
    var s=document.createElement('span'); s.className='float'; s.textContent=txt;
    pads[i].appendChild(s); setTimeout(function(){s.remove()},1000);
  }
  function confetti(i){
    if(reduce) return;
    var r=pads[i].getBoundingClientRect(), cx=r.left+r.width/2, cy=r.top+r.height/2;
    for(var k=0;k<36;k++){
      var c=document.createElement('span'); c.className='confetti';
      c.style.left=cx+'px'; c.style.top=cy+'px';
      c.style.background=COLS[k%4];
      document.body.appendChild(c);
      var a=Math.random()*Math.PI*2, v=120+Math.random()*260;
      var dx=Math.cos(a)*v, dy=Math.sin(a)*v-180;
      var anim=c.animate([
        {transform:'translate(0,0) rotate(0)',opacity:1},
        {transform:'translate('+dx+'px,'+(dy+420)+'px) rotate('+(Math.random()*900)+'deg)',opacity:0}
      ],{duration:1200+Math.random()*600,easing:'cubic-bezier(.2,.6,.4,1)'});
      anim.onfinish=(function(el){return function(){el.remove()}})(c);
    }
  }
  function restart(el,cls){el.classList.remove(cls); void el.offsetWidth; el.classList.add(cls)}

  /* ---------- render ---------- */
  function apply(s){
    var prev=cur; cur=s;
    var max=Math.max.apply(null,s.scores);
    pads.forEach(function(p,i){
      var sc=p.querySelector('.score');
      sc.textContent=s.scores[i];
      p.classList.toggle('locked',!s.active[i] && s.buzzed!==i);
      p.classList.toggle('buzzed',s.buzzed===i);
      p.classList.toggle('dim',s.buzzed>=0 && s.buzzed!==i && s.active[i]);
      p.classList.toggle('leader',max>0 && s.scores[i]===max);
      var custom=s.names&&s.names[i]&&s.names[i].trim();
      p.querySelector('.name').textContent = nm(s,i);
      p.querySelector('.who').textContent = custom ? NAMES[i] : 'Player '+(i+1);
      p.querySelector('.tag').textContent = s.buzzed===i ? 'Buzzed in!' : 'Locked out';
      p.setAttribute('aria-label',nm(s,i)+', '+NAMES[i]+' button, score '+s.scores[i]+(s.active[i]?'':', locked out'));
      if(prev){
        var d=s.scores[i]-prev.scores[i];
        if(d>0){restart(sc,'pop');floatText(i,'+'+d);confetti(i);sfx.right()}
        if(d<0){restart(p,'shake');floatText(i,String(d));sfx.wrong()}
      }
    });
    if(prev && s.buzzed>=0 && prev.buzzed!==s.buzzed) sfx.buzz();

    var st;
    if(s.buzzed>=0) st=nm(s,s.buzzed)+' buzzed in!';
    else if(s.active.every(function(a){return !a})) st='Everyone’s locked out. Reopen the buzzers.';
    else st=demo?'Tap a colour to buzz in':'Waiting for a buzz…';
    $('status').textContent=st;

    $('bCorrect').disabled=s.buzzed<0;
    $('bWrong').disabled=s.buzzed<0;
  }

  function setConn(state,txt){ $('conn').dataset.s=state; $('connTxt').textContent=txt; }

  /* ---------- live mode: poll the ESP32 ---------- */
  function poll(){
    fetch('/state',{cache:'no-store'}).then(function(r){
      if(!r.ok) throw new Error(r.status); return r.json();
    }).then(function(s){
      connected=true; fails=0; setConn('live','Live'); apply(s);
      setTimeout(poll,250);
    }).catch(function(){
      if(!connected){ startDemo(); return; }
      fails++; setConn('lost','Reconnecting…');
      setTimeout(poll,Math.min(3000,250*fails));
    });
  }
  function post(path,body){
    if(demo){ demoAct(path,body); return; }
    var opt={method:'POST'};
    if(body){ opt.body=new URLSearchParams(body).toString(); opt.headers={'Content-Type':'application/x-www-form-urlencoded'}; }
    fetch(path,opt).then(function(r){return r.json()}).then(apply).catch(function(){});
  }

  /* ---------- demo mode: simulates the ESP32 so the page can be tried anywhere ---------- */
  function startDemo(){
    demo=true; document.body.classList.add('demo');
    setConn('demo','Demo'); setHost(true);
    $('hint').textContent='Demo mode: tap a colour (or press 1–4) to buzz, then mark it. Keys: C correct, W wrong, R reopen';
    apply({scores:[3,1,4,2],active:[true,true,true,true],buzzed:-1,names:['Mum','Dad','Ella','']});
  }
  function clone(s){return JSON.parse(JSON.stringify(s))}
  function demoAct(path,body){
    var s=clone(cur);
    if(path.indexOf('/buzz')===0){
      var i=+path.split('=')[1];
      if(s.buzzed<0 && s.active[i]) s.buzzed=i; else return;
    } else if(path==='/correct'){
      if(s.buzzed<0) return;
      s.scores[s.buzzed]++; s.active=[true,true,true,true]; s.buzzed=-1;
    } else if(path==='/wrong'){
      if(s.buzzed<0) return;
      s.scores[s.buzzed]--; s.active[s.buzzed]=false; s.buzzed=-1;
    } else if(path==='/reopen'){
      s.active=[true,true,true,true];
    } else if(path==='/names'){
      s.names=[body.n0,body.n1,body.n2,body.n3];
    } else if(path==='/resetscores'){
      s={scores:[0,0,0,0],active:[true,true,true,true],buzzed:-1,names:s.names};
      cur=s; // no score-change effects on a full reset
    }
    apply(s);
  }
  pads.forEach(function(p,i){ p.onclick=function(){ if(demo) demoAct('/buzz?p='+i) } });

  /* ---------- host controls ---------- */
  $('bCorrect').onclick=function(){post('/correct')};
  $('bWrong').onclick=function(){post('/wrong')};
  $('bReopen').onclick=function(){post('/reopen')};
  var armT=null;
  $('bZero').onclick=function(){
    var b=this;
    if(!b.classList.contains('arm')){
      b.classList.add('arm'); b.textContent='Tap again to reset';
      armT=setTimeout(function(){b.classList.remove('arm');b.textContent='Reset scores'},3000);
      return;
    }
    clearTimeout(armT); b.classList.remove('arm'); b.textContent='Reset scores';
    if(!demo) cur=null; // skip effects for the reset
    post('/resetscores');
  };
  var dlg=$('nameDlg');
  $('bNames').onclick=function(){
    for(var i=0;i<4;i++) $('n'+i).value=(cur&&cur.names&&cur.names[i])||'';
    if(dlg.showModal) dlg.showModal(); else dlg.setAttribute('open','');
    $('n0').focus();
  };
  function closeDlg(){ if(dlg.close) dlg.close(); else dlg.removeAttribute('open'); }
  $('nCancel').onclick=closeDlg;
  $('nClear').onclick=function(){ for(var i=0;i<4;i++) $('n'+i).value=''; $('n0').focus(); };
  function saveNames(){
    var b={};
    for(var i=0;i<4;i++) b['n'+i]=$('n'+i).value.trim().slice(0,16);
    post('/names',b); closeDlg();
  }
  $('nSave').onclick=saveNames;
  [0,1,2,3].forEach(function(i){
    $('n'+i).addEventListener('keydown',function(e){
      if(e.key==='Enter'){ e.preventDefault(); if(i<3) $('n'+(i+1)).focus(); else saveNames(); }
    });
  });

  document.addEventListener('keydown',function(e){
    if(e.target.tagName==='INPUT' || dlg.open) return;
    var k=e.key.toLowerCase();
    if(k==='c') post('/correct');
    else if(k==='w') post('/wrong');
    else if(k==='r') post('/reopen');
    else if(demo && '1234'.indexOf(k)>=0 && k) demoAct('/buzz?p='+(+k-1));
  });

  if(location.protocol==='file:' || /[?&]demo\b/.test(location.search)) startDemo(); else poll();
})();
</script>
</body>
</html>

)rawliteral";

// ---------------- SETUP ----------------
void setup() {
  Serial.begin(115200);
  Serial.println(__FILE__ " " __DATE__);

  // LCD
  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Buzzer game");
  lcd.setCursor(0, 1);
  lcd.print("Starting WiFi...");

  // Pins
  for (int i = 0; i < numPlayers; i++) {
    pinMode(buttonPins[i], INPUT_PULLUP);
    pinMode(ledPins[i], OUTPUT);
  }
  pinMode(correctButtonPin, INPUT_PULLUP);
  pinMode(wrongButtonPin, INPUT_PULLUP);

  // DY audio player on hardware Serial2
  Serial2.begin(9600, SERIAL_8N1, dyRxPin, dyTxPin);
  player.begin();
  player.setCycleMode(DY::PlayMode::OneOff);
  player.setVolume(25); // 0-30
  player.stop();
  player.playSpecified(sfxIntro);

  // Saved player names
  loadNames();

  // Wi-Fi + web server
  startWiFi();
  setupWebServer();

  updateScoreDisplay();
}

// ---------------- LOOP (non-blocking) ----------------
void loop() {
  server.handleClient();

  bool correctPressed = justPressed(correctBtn);
  bool wrongPressed   = justPressed(wrongBtn);

  if (playerWhoBuzzed == -1) {
    // Idle: "Correct" button reopens all buzzers (same as the original reset behaviour)
    if (correctPressed) {
      Serial.println("Reactivating all players");
      reactivateAll();
      updateScoreDisplay();
    }

    // Scan player buttons
    if (millis() >= buzzLockUntil) {
      for (int i = 0; i < numPlayers; i++) {
        if (digitalRead(buttonPins[i]) == LOW && playerActive[i]) {
          playerBuzzed(i);
          break;
        }
      }
    }

    // LEDs show who can still buzz
    if (playerWhoBuzzed == -1) {
      for (int i = 0; i < numPlayers; i++) {
        digitalWrite(ledPins[i], playerActive[i] ? HIGH : LOW);
      }
    }
  } else {
    // Someone has buzzed: wait for the host (physical buttons or web page)
    if (correctPressed)      judgeAnswer(true);
    else if (wrongPressed)   judgeAnswer(false);
  }
}

// ---------------- GAME LOGIC ----------------
void playerBuzzed(int i) {
  Serial.printf("Player %d (%s) buzzed in\n", i + 1, displayName(i).c_str());
  player.playSpecified(sfxBuzzer[i]);
  playerWhoBuzzed = i;
  for (int j = 0; j < numPlayers; j++) {
    digitalWrite(ledPins[j], j == i ? HIGH : LOW);
  }
  showBuzzedMessage(i);
}

void judgeAnswer(bool isAnswerCorrect) {
  if (playerWhoBuzzed == -1) return;

  if (isAnswerCorrect) {
    player.playSpecified(sfxCorrect);
    playerScore[playerWhoBuzzed]++;
    Serial.println("CORRECT. Reactivating all players");
    reactivateAll();
  } else {
    player.playSpecified(sfxIncorrect);
    playerScore[playerWhoBuzzed]--;
    Serial.printf("INCORRECT. Deactivating player %d\n", playerWhoBuzzed + 1);
    deactivatePlayer(playerWhoBuzzed);
  }

  playerWhoBuzzed = -1;
  buzzLockUntil = millis() + rebuzzLockMs;
  updateScoreDisplay();
}

void reactivateAll() {
  for (int i = 0; i < numPlayers; i++) activatePlayer(i);
}

void deactivatePlayer(int p) { playerActive[p] = false; }
void activatePlayer(int p)   { playerActive[p] = true;  }

// ---------------- DISPLAY ----------------
void updateScoreDisplay() {
  // Serial output
  char buffer[12];
  Serial.print("Player ");
  for (int i = 0; i < numPlayers; i++) {
    snprintf(buffer, sizeof(buffer), "%5d", i + 1);
    Serial.print(buffer);
  }
  Serial.println();
  Serial.print("Score  ");
  for (int i = 0; i < numPlayers; i++) {
    snprintf(buffer, sizeof(buffer), "%5d", playerScore[i]);
    Serial.print(buffer);
  }
  Serial.println();

  // LCD: 4 characters per player
  // Line 1: "R   Y   B   G   "   Line 2: scores
  const char initials[numPlayers] = {'R', 'Y', 'B', 'G'};
  char line[lcdColumns + 1];
  for (int i = 0; i < numPlayers; i++) {
    snprintf(line + i * 4, 5, "%c   ", initials[i]);
  }
  lcd.setCursor(0, 0);
  lcd.print(line);
  for (int i = 0; i < numPlayers; i++) {
    snprintf(line + i * 4, 5, "%-4d", playerScore[i]);
  }
  lcd.setCursor(0, 1);
  lcd.print(line);
}

void showBuzzedMessage(int playerIndex) {
  char line[lcdColumns + 1];
  snprintf(line, sizeof(line), "%-6s BUZZED!  ", colourNames[playerIndex]); // padded to 16
  lcd.setCursor(0, 0);
  lcd.print(line);
  lcd.setCursor(0, 1);
  lcd.print("Correct or wrong");
}

// ---------------- WIFI ----------------
void startWiFi() {
  IPAddress ip;
  bool joined = false;

  if (USE_HOME_WIFI) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(HOME_SSID, HOME_PASS);
    Serial.print("Connecting to WiFi");
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
      delay(250);
      Serial.print(".");
    }
    Serial.println();
    joined = (WiFi.status() == WL_CONNECTED);
    if (joined) ip = WiFi.localIP();
  }

  if (!joined) {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    ip = WiFi.softAPIP();
    Serial.printf("Access point \"%s\" started\n", AP_SSID);
  }

  if (MDNS.begin(MDNS_NAME)) {
    MDNS.addService("http", "tcp", 80);
  }

  Serial.print("Scoreboard: http://");
  Serial.println(ip);

  // Show the address on the LCD for a few seconds
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(joined ? "Open in browser:" : "WiFi Buzzer-Game");
  lcd.setCursor(0, 1);
  lcd.print(ip.toString());
  delay(4000);
  lcd.clear();
}

// ---------------- PLAYER NAMES ----------------
// Name shown for a player: the custom name, or the colour if none is set
String displayName(int i) {
  return playerNames[i].length() ? playerNames[i] : String(colourNames[i]);
}

// Keep names safe to put straight into JSON: drop quotes, backslashes and
// control characters, trim spaces, and cut to maxNameBytes without splitting
// a multi-byte UTF-8 character.
String cleanName(const String &in) {
  String out;
  for (size_t k = 0; k < in.length(); k++) {
    uint8_t c = (uint8_t)in[k];
    if (c < 32 || c == 127 || c == '"' || c == '\\') continue;
    out += (char)c;
  }
  out.trim();
  if (out.length() > maxNameBytes) {
    size_t cut = maxNameBytes;
    while (cut > 0 && (((uint8_t)out[cut]) & 0xC0) == 0x80) cut--; // back up to a character boundary
    out = out.substring(0, cut);
    out.trim();
  }
  return out;
}

void loadNames() {
  prefs.begin("buzzer", true); // read-only
  for (int i = 0; i < numPlayers; i++) {
    String key = "name" + String(i);
    playerNames[i] = cleanName(prefs.getString(key.c_str(), ""));
  }
  prefs.end();
}

void saveNames() {
  prefs.begin("buzzer", false);
  for (int i = 0; i < numPlayers; i++) {
    String key = "name" + String(i);
    prefs.putString(key.c_str(), playerNames[i]);
  }
  prefs.end();
}

// ---------------- WEB SERVER ----------------
void sendState() {
  String json;
  json.reserve(256);
  json += "{\"scores\":[";
  for (int i = 0; i < numPlayers; i++) { if (i) json += ','; json += playerScore[i]; }
  json += "],\"active\":[";
  for (int i = 0; i < numPlayers; i++) { if (i) json += ','; json += playerActive[i] ? "true" : "false"; }
  json += "],\"buzzed\":";
  json += playerWhoBuzzed;
  json += ",\"names\":[";
  for (int i = 0; i < numPlayers; i++) {
    if (i) json += ',';
    json += '"'; json += playerNames[i]; json += '"'; // already cleaned, safe for JSON
  }
  json += "]}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

void setupWebServer() {
  server.on("/", HTTP_GET, []() {
    server.send_P(200, "text/html", INDEX_HTML);
  });

  server.on("/state", HTTP_GET, sendState);

  // Host controls from the web page
  server.on("/correct", HTTP_POST, []() {
    judgeAnswer(true);
    sendState();
  });
  server.on("/wrong", HTTP_POST, []() {
    judgeAnswer(false);
    sendState();
  });
  server.on("/reopen", HTTP_POST, []() {
    reactivateAll();
    updateScoreDisplay();
    sendState();
  });
  // Set player names: POST form fields n0..n3 (empty = use the colour)
  server.on("/names", HTTP_POST, []() {
    bool changed = false;
    for (int i = 0; i < numPlayers; i++) {
      String field = "n" + String(i);
      if (server.hasArg(field)) {
        String n = cleanName(server.arg(field));
        if (n != playerNames[i]) { playerNames[i] = n; changed = true; }
      }
    }
    if (changed) {
      saveNames();
      Serial.print("Names: ");
      for (int i = 0; i < numPlayers; i++) { Serial.print(displayName(i)); Serial.print(i < numPlayers - 1 ? ", " : "\n"); }
    }
    sendState();
  });

  server.on("/resetscores", HTTP_POST, []() {
    for (int i = 0; i < numPlayers; i++) playerScore[i] = 0;
    reactivateAll();
    playerWhoBuzzed = -1;
    updateScoreDisplay();
    sendState();
  });

  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
  server.begin();
}
