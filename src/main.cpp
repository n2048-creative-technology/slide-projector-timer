/*
* Master / Slave or Standalone slide timer controller for projectors or other devices.
* Created by: Mauricio van der Maesen
* License: MIT
*/

#include "Arduino.h"
#include <DNSServer.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>

enum DeviceMode : uint8_t {
  MODE_STANDALONE = 0,
  MODE_MASTER = 1,
  MODE_SLAVE = 2,
};

struct ControlOutput {
  const char *label;
  uint8_t pin;
  bool active;
  uint32_t pulseStartMs;
  uint32_t activePulseMs;
};

constexpr uint8_t resetPin = D0;
constexpr uint16_t dnsPort = 53;
constexpr uint8_t syncChannel = 6;
constexpr char defaultApSsid[] = "Slide Timer";
constexpr char defaultApPassword[] = "slidetimer";
constexpr char defaultSyncGroup[] = "default";
constexpr uint32_t defaultPulseMs = 300;
constexpr uint32_t defaultIntervalMs = 5000;
constexpr uint32_t resetHoldMs = 3100;
const IPAddress defaultApIp(4, 3, 2, 1);
const IPAddress apSubnet(255, 255, 255, 0);
const uint8_t espNowBroadcastAddress[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

struct SyncMessage {
  char magic[4];
  char label[9];
  uint32_t groupHash;
  uint32_t counter;
};

ControlOutput previousControl = {"PREVIOUS", D5, false, 0, defaultPulseMs};
ControlOutput nextControl = {"NEXT", D6, false, 0, defaultPulseMs};

DNSServer dnsServer;
WebServer server(80);
Preferences preferences;

String apSsid = defaultApSsid;
String apPassword = defaultApPassword;
String syncGroup = defaultSyncGroup;
uint32_t syncGroupHashValue = 0;
DeviceMode deviceMode = MODE_STANDALONE;
uint32_t pulseMs = defaultPulseMs;
uint32_t intervalMs = defaultIntervalMs;
bool playOnBoot = false;
bool playing = false;
uint32_t lastPlayPulseMs = 0;
uint32_t resetLowStartMs = 0;
uint32_t restartAtMs = 0;
uint32_t syncCounter = 0;
uint32_t lastRemoteCounter = 0;
IPAddress portalIp = defaultApIp;
volatile bool pendingNextPulse = false;
volatile bool pendingPreviousPulse = false;

String buildPage();

String defaultApName() {
  uint64_t mac = ESP.getEfuseMac();
  char suffix[13];
  snprintf(suffix, sizeof(suffix), "%04X%08X", static_cast<uint16_t>(mac >> 32),
           static_cast<uint32_t>(mac));
  return String(defaultApSsid) + "-" + suffix;
}

String modeName(DeviceMode mode) {
  if (mode == MODE_MASTER) {
    return "MASTER";
  }
  if (mode == MODE_SLAVE) {
    return "SLAVE";
  }
  return "STANDALONE";
}

uint32_t computeSyncGroupHash(const String &group) {
  uint32_t hash = 2166136261UL;
  for (size_t i = 0; i < group.length(); i++) {
    hash ^= static_cast<uint8_t>(group[i]);
    hash *= 16777619UL;
  }
  return hash;
}

void updateSyncGroupHash() {
  syncGroupHashValue = computeSyncGroupHash(syncGroup);
}

String secondsText(uint32_t valueMs) {
  if (valueMs % 1000 == 0) {
    return String(valueMs / 1000);
  }

  String seconds = String(valueMs / 1000.0f, 1);
  seconds.replace(".0", "");
  return seconds;
}

String htmlEscape(const String &value) {
  String escaped;
  escaped.reserve(value.length());

  for (size_t i = 0; i < value.length(); i++) {
    char c = value[i];
    if (c == '&') {
      escaped += F("&amp;");
    } else if (c == '<') {
      escaped += F("&lt;");
    } else if (c == '>') {
      escaped += F("&gt;");
    } else if (c == '"') {
      escaped += F("&quot;");
    } else {
      escaped += c;
    }
  }

  return escaped;
}

void saveDefaults() {
  pulseMs = defaultPulseMs;
  intervalMs = defaultIntervalMs;
  playOnBoot = false;
  playing = false;
  apSsid = defaultApName();
  apPassword = defaultApPassword;
  syncGroup = defaultSyncGroup;
  updateSyncGroupHash();
  deviceMode = MODE_STANDALONE;

  preferences.clear();
  preferences.putUInt("pulseMs", pulseMs);
  preferences.putUInt("intervalMs", intervalMs);
  preferences.putBool("playBoot", playOnBoot);
  preferences.putString("apSsid", apSsid);
  preferences.putString("apPass", apPassword);
  preferences.putString("syncGroup", syncGroup);
  preferences.putUChar("mode", deviceMode);
}

void factoryReset() {
  Serial.println("D0 held LOW. Restoring factory defaults.");
  saveDefaults();
  delay(100);
  ESP.restart();
}

void broadcastPulse(const char *label, uint32_t durationMs) {
  if (deviceMode != MODE_MASTER) {
    return;
  }

  syncCounter++;
  SyncMessage message = {{'S', 'P', 'T', '1'}, "", syncGroupHashValue, syncCounter};
  strncpy(message.label, label, sizeof(message.label) - 1);
  esp_err_t result = esp_now_send(espNowBroadcastAddress, reinterpret_cast<uint8_t *>(&message),
                                  sizeof(message));
  Serial.printf("ESP-NOW %s #%lu -> %s\n", label, syncCounter,
                result == ESP_OK ? "sent" : "failed");
}

void triggerPulse(ControlOutput &control, uint32_t durationMs, bool remote = false) {
  control.active = true;
  control.pulseStartMs = millis();
  control.activePulseMs = constrain(durationMs, 50, 2000);
  digitalWrite(control.pin, HIGH);
  Serial.printf("%s -> HIGH at %lu ms\n", control.label, control.pulseStartMs);

  if (!remote) {
    broadcastPulse(control.label, control.activePulseMs);
  }
}

void triggerPulse(ControlOutput &control) {
  triggerPulse(control, pulseMs, false);
}

void stopPulse(ControlOutput &control, uint32_t now) {
  if (!control.active) {
    return;
  }

  control.active = false;
  digitalWrite(control.pin, LOW);
  Serial.printf("%s -> LOW at %lu ms\n", control.label, now);
}

void stopAllPulses(uint32_t now) {
  stopPulse(previousControl, now);
  stopPulse(nextControl, now);
}

void sendHome() {
  server.send(200, "text/html", buildPage());
}

String buildPage() {
  String html;
  html.reserve(24000);

  html += F(
      "<!doctype html><html><head><meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<title>Slide Timer</title>"
      "<style>"
      ":root{color-scheme:dark;--bg:#111;--panel:#1d1f23;--panel2:#292c31;"
      "--text:#f2f4f8;--muted:#9ba3af;--accent:#ff9f1c;--line:#3a3f46;"
      "--danger:#e84855;--ok:#24c875}*{box-sizing:border-box}body{margin:0;"
      "background:#111;color:var(--text);font-family:Arial,Helvetica,sans-serif}"
      "header{position:sticky;top:0;z-index:2;background:#15171a;border-bottom:1px solid "
      "var(--line);padding:14px 16px}.wrap{max-width:720px;margin:0 auto}.brand{display:flex;"
      "align-items:center;justify-content:space-between;gap:12px}.logo{display:flex;"
      "align-items:center;gap:10px;font-size:22px;font-weight:700}.spark{width:28px;height:28px;"
      "border-radius:6px;background:linear-gradient(135deg,var(--accent),#e84855);"
      "box-shadow:0 0 22px rgba(255,159,28,.35)}.pill{font-size:12px;color:#111;"
      "background:var(--accent);padding:5px 8px;border-radius:4px;font-weight:700}"
      "main{padding:16px}.status{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;"
      "margin:0 0 12px}.stat,.panel{background:var(--panel);border:1px solid var(--line);"
      "border-radius:6px}.stat{padding:10px}.stat b{display:block;font-size:18px}"
      ".stat span{color:var(--muted);font-size:12px}.panel{padding:14px;margin-bottom:12px}"
      "h1,h2,p{margin:0}h2{font-size:18px}p{color:var(--muted);font-size:13px;margin-top:3px}"
      ".controls{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-top:14px}"
      ".controls form{margin:0}button,.btn{appearance:none;width:100%;border:0;border-radius:5px;"
      "background:var(--accent);color:#111;font-weight:800;font-size:16px;padding:14px 12px;"
      "text-decoration:none;text-align:center;display:block;transition:transform .06s ease,filter .06s ease,"
      "box-shadow .06s ease;box-shadow:0 2px 0 rgba(0,0,0,.45)}button:active,.btn:active{"
      "transform:translateY(2px);filter:brightness(.82);box-shadow:0 0 0 rgba(0,0,0,0)}"
      ".secondary{background:var(--panel2)!important;"
      "color:var(--text)!important;border:1px solid var(--line)!important}.danger{background:"
      "var(--danger)!important;color:#fff!important}.play{background:var(--ok)!important;color:"
      "#07170d!important}.setting{display:flex;align-items:center;justify-content:space-between;"
      "gap:12px;background:var(--panel2);border:1px solid var(--line);border-radius:6px;"
      "padding:12px;margin-top:12px}.valueBtn{width:auto;min-width:110px;color:var(--accent);"
      "background:#15171a!important;border:1px solid var(--line);padding:10px 12px}.field{margin-top:12px}"
      ".field label{display:block;color:var(--muted);font-size:13px;margin-bottom:6px}.field input,"
      ".field select{width:100%;background:var(--panel2);color:var(--text);border:1px solid var(--line);"
      "border-radius:5px;padding:12px;font-size:16px}.hint{color:var(--muted);font-size:12px;margin-top:7px}"
      ".toggle{display:flex;align-items:center;justify-content:space-between;gap:12px}.switch{position:"
      "relative;width:54px;height:30px;flex:none;display:block;cursor:pointer}.switch input{position:absolute;"
      "opacity:0;width:54px;height:30px;margin:0;cursor:pointer}.slider{position:absolute;"
      "cursor:pointer;inset:0;background:#555;border-radius:30px}.slider:before{content:'';position:"
      "absolute;height:22px;width:22px;left:4px;top:4px;background:white;border-radius:50%;"
      "transition:.15s}input:checked+.slider{background:var(--accent)}input:checked+.slider:before{"
      "transform:translateX(24px)}.toast{position:fixed;left:50%;bottom:18px;transform:translateX(-50%);"
      "background:#173224;border:1px solid #2f8d57;color:#baf7cf;border-radius:6px;padding:10px 12px;"
      "display:none;z-index:5}.modal{position:fixed;inset:0;background:rgba(0,0,0,.65);display:none;"
      "align-items:flex-end;justify-content:center;z-index:4}.sheet{width:100%;max-width:420px;background:"
      "#15171a;border:1px solid var(--line);border-radius:8px 8px 0 0;padding:16px}.display{background:"
      "#090a0c;border:1px solid var(--line);border-radius:6px;padding:14px;text-align:right;font-size:"
      "28px;color:var(--accent);font-weight:800;margin:12px 0}.pad{display:grid;grid-template-columns:"
      "repeat(3,1fr);gap:8px}.pad button{background:var(--panel2);color:var(--text);border:1px solid "
      "var(--line);font-size:22px;padding:18px 0}.modalActions{display:grid;grid-template-columns:1fr 1fr;"
      "gap:8px;margin-top:10px}@media(max-width:420px){.status{grid-template-columns:1fr}.controls{"
      "grid-template-columns:1fr}.setting{align-items:flex-start;flex-direction:column}.valueBtn{width:100%}}"
      "</style></head><body><header><div class='wrap brand'><div class='logo'><div class='spark'></div>"
      "<span>Slide Timer</span></div><div class='pill'>AP MODE</div></div></header><main class='wrap'>");

  html += F("<div class='status'><div class='stat'><b id='stateText'>");
  html += playing ? "PLAY" : "STOP";
  html += F("</b><span>state</span></div><div class='stat'><b>");
  html += modeName(deviceMode);
  html += F("</b><span>mode</span></div><div class='stat'><b>");
  html += WiFi.softAPIP().toString();
  html += F("</b><span>portal</span></div></div>");

  html += F(
      "<section class='panel'><h2>Controls</h2><p>D6 sends NEXT. D5 sends PREVIOUS. In master mode these pulses are broadcast to slaves.</p>"
      "<div class='controls'><form method='post' action='/previous'><button class='secondary' "
      "type='submit'>PREVIOUS</button></form><form method='post' action='/next'><button "
      "type='submit'>NEXT</button></form><form method='post' action='/play'><button class='play' "
      "type='submit'>PLAY</button></form><form method='post' action='/stop'><button class='danger' "
      "type='submit'>STOP</button></form></div></section>");

  html += F(
      "<section class='panel'><h2>Settings</h2><p>Tap a value to edit it with the keypad. Changes save immediately.</p>"
      "<div class='setting'><div><h2>Pulse Duration</h2><p>How long D5 or D6 stays HIGH.</p></div>"
      "<button class='valueBtn' type='button' onclick=\"openPad('pulse','Pulse Duration',50,2000,10,");
  html += String(pulseMs);
  html += F(",' ms')\"><span id='pulseValue'>");
  html += String(pulseMs);
  html += F("</span> ms</button></div><div class='setting'><div><h2>Play Interval</h2>"
            "<p>Delay between automatic NEXT pulses.</p></div><button class='valueBtn' type='button' "
            "onclick=\"openPad('interval','Play Interval',1,600,1,");
  html += String(intervalMs / 1000);
  html += F(",' s')\"><span id='intervalValue'>");
  html += String(intervalMs / 1000);
  html += F("</span> s</button></div><div class='setting toggle'><div><h2>Play on Boot</h2>"
            "<p>Start automatic NEXT pulses after power-up. Ignored in slave mode.</p></div><label class='switch'>"
            "<input id='bootToggle' type='checkbox' onchange='saveBoot(this.checked)'");
  if (playOnBoot) {
    html += F(" checked");
  }
  html += F("><span class='slider'></span></label></div></section>");

  html += F("<section class='panel'><h2>Network Role</h2><p>Standalone runs alone. Master broadcasts pulses. Slave follows broadcasts from the same sync group.</p>"
            "<div class='field'><label for='mode'>Mode</label><select id='mode' onchange='saveMode()'>"
            "<option value='0'");
  if (deviceMode == MODE_STANDALONE) {
    html += F(" selected");
  }
  html += F(">Standalone</option><option value='1'");
  if (deviceMode == MODE_MASTER) {
    html += F(" selected");
  }
  html += F(">Master</option><option value='2'");
  if (deviceMode == MODE_SLAVE) {
    html += F(" selected");
  }
  html += F(">Controlled / Slave</option></select></div>"
            "<div class='field'><label for='syncGroup'>Sync group</label><input id='syncGroup' maxlength='31' onchange='saveSyncGroup()' value=\"");
  html += htmlEscape(syncGroup);
  html += F("\"></div><div class='hint'>Set the same sync group on one master and its slaves. Use a different group for another independent projector set. ESP-NOW uses WiFi channel 6.</div></section>");

  html += F("<section class='panel'><h2>Access Point</h2><p>This device always creates its own AP for the web UI. AP changes save when a field changes, then the controller restarts.</p>"
            "<div class='field'><label for='apName'>AP name</label><input id='apName' maxlength='31' onchange='saveAp()' value=\"");
  html += htmlEscape(apSsid);
  html += F("\"></div><div class='field'><label for='apPass'>AP password</label><input id='apPass' "
            "maxlength='63' minlength='8' type='password' onchange='saveAp()' value=\"");
  html += htmlEscape(apPassword);
  html += F("\"></div><div class='hint'>Password must be 8 to 63 characters. Factory reset: hold D0 to GND for over 3 seconds.</div>"
            "</section>");

  html += F(
      "</main><div id='toast' class='toast'></div><div id='modal' class='modal'><div class='sheet'>"
      "<h2 id='padTitle'></h2><p id='padHint'></p><div id='padDisplay' class='display'>0</div>"
      "<div class='pad'><button onclick=\"tap('1')\">1</button><button onclick=\"tap('2')\">2</button>"
      "<button onclick=\"tap('3')\">3</button><button onclick=\"tap('4')\">4</button><button onclick=\"tap('5')\">5</button>"
      "<button onclick=\"tap('6')\">6</button><button onclick=\"tap('7')\">7</button><button onclick=\"tap('8')\">8</button>"
      "<button onclick=\"tap('9')\">9</button><button onclick='clearPad()'>CLR</button><button onclick=\"tap('0')\">0</button>"
      "<button onclick='backPad()'>DEL</button></div><div class='modalActions'><button class='secondary' "
      "onclick='closePad()'>CANCEL</button><button onclick='savePad()'>SAVE</button></div></div></div>"
      "<script>"
      "let pad={key:'',min:0,max:0,step:1,value:'',unit:''};"
      "function toast(t){let e=document.getElementById('toast');e.textContent=t;e.style.display='block';"
      "clearTimeout(window.toastTimer);window.toastTimer=setTimeout(()=>e.style.display='none',2600)}"
      "function openPad(k,t,min,max,step,val,unit){pad={key:k,min:min,max:max,step:step,value:String(val),unit:unit};"
      "document.getElementById('padTitle').textContent=t;document.getElementById('padHint').textContent="
      "'Range '+min+' to '+max+unit;document.getElementById('padDisplay').textContent=pad.value+unit;"
      "document.getElementById('modal').style.display='flex'}"
      "function closePad(){document.getElementById('modal').style.display='none'}"
      "function renderPad(){document.getElementById('padDisplay').textContent=(pad.value||'0')+pad.unit}"
      "function tap(n){if(pad.value.length<5){pad.value=(pad.value==='0'?n:pad.value+n);renderPad()}}"
      "function clearPad(){pad.value='';renderPad()}function backPad(){pad.value=pad.value.slice(0,-1);renderPad()}"
      "async function post(url,body){return fetch(url,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body})}"
      "function setNumber(k,v){document.getElementById(k==='pulse'?'pulseValue':'intervalValue').textContent=v}"
      "async function savePad(){let old=document.getElementById(pad.key==='pulse'?'pulseValue':'intervalValue').textContent;"
      "let v=parseInt(pad.value||'0');v=Math.max(pad.min,Math.min(pad.max,v));v=Math.round(v/pad.step)*pad.step;"
      "setNumber(pad.key,v);closePad();toast('Saving...');let r=await post('/api/number','key='+encodeURIComponent(pad.key)+'&value='+v);"
      "if(r.ok){toast('Saved')}else{setNumber(pad.key,old);toast('Save failed')}}"
      "async function saveBoot(v){toast('Saving...');let r=await post('/api/boot','value='+(v?'1':'0'));"
      "if(r.ok){toast('Saved')}else{document.getElementById('bootToggle').checked=!v;toast('Save failed')}}"
      "async function saveMode(){let m=document.getElementById('mode').value;let r=await post('/api/mode','mode='+encodeURIComponent(m));"
      "toast(r.ok?'Saved. Rebooting to apply mode.':'Save failed')}"
      "async function saveSyncGroup(){let g=document.getElementById('syncGroup').value.trim();"
      "if(!g){toast('Sync group required');return}let r=await post('/api/sync','group='+encodeURIComponent(g));"
      "toast(r.ok?'Saved':'Save failed')}"
      "async function saveAp(){let s=document.getElementById('apName').value.trim();let p=document.getElementById('apPass').value;"
      "if(!s){toast('AP name required');return}if(p.length<8||p.length>63){toast('Password must be 8-63 characters');return}"
      "let r=await post('/api/ap','ssid='+encodeURIComponent(s)+'&pass='+encodeURIComponent(p));toast(r.ok?'Saved. Rebooting to apply AP.':'Save failed')}"
      "</script></body></html>");

  return html;
}

void redirectToPortal() {
  server.sendHeader("Location", String("http://") + portalIp.toString(), true);
  server.send(302, "text/plain", "");
}

void redirectHome() {
  server.sendHeader("Location", "/", true);
  server.send(303, "text/plain", "");
}

void handleNext() {
  triggerPulse(nextControl);
  redirectHome();
}

void handlePrevious() {
  triggerPulse(previousControl);
  redirectHome();
}

void handlePlay() {
  if (deviceMode != MODE_SLAVE) {
    playing = true;
    lastPlayPulseMs = millis();
    triggerPulse(nextControl);
  }
  redirectHome();
}

void handleStop() {
  playing = false;
  stopAllPulses(millis());
  redirectHome();
}

void handleNumberSetting() {
  String key = server.arg("key");
  uint32_t value = server.arg("value").toInt();

  if (key == "pulse") {
    pulseMs = constrain(value, 50, 2000);
    preferences.putUInt("pulseMs", pulseMs);
  } else if (key == "interval") {
    intervalMs = constrain(value, 1, 600) * 1000;
    lastPlayPulseMs = millis();
    preferences.putUInt("intervalMs", intervalMs);
  } else {
    server.send(400, "text/plain", "unknown setting");
    return;
  }

  server.send(200, "text/plain", "ok");
}

void handleBootSetting() {
  playOnBoot = server.arg("value") == "1";
  preferences.putBool("playBoot", playOnBoot);
  server.send(200, "text/plain", "ok");
}

void handleModeSetting() {
  uint8_t mode = server.arg("mode").toInt();
  if (mode > MODE_SLAVE) {
    server.send(400, "text/plain", "invalid mode");
    return;
  }

  deviceMode = static_cast<DeviceMode>(mode);
  preferences.putUChar("mode", deviceMode);
  restartAtMs = millis() + 1000;
  server.send(200, "text/plain", "ok");
}

void handleSyncSetting() {
  String group = server.arg("group");
  group.trim();

  if (group.length() == 0 || group.length() > 31) {
    server.send(400, "text/plain", "invalid sync group");
    return;
  }

  syncGroup = group;
  updateSyncGroupHash();
  preferences.putString("syncGroup", syncGroup);
  server.send(200, "text/plain", "ok");
}

bool validSsidAndPass(const String &ssid, const String &pass) {
  return ssid.length() > 0 && ssid.length() <= 31 && pass.length() >= 8 && pass.length() <= 63;
}

void handleApSetting() {
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  ssid.trim();

  if (!validSsidAndPass(ssid, pass)) {
    server.send(400, "text/plain", "invalid AP settings");
    return;
  }

  apSsid = ssid;
  apPassword = pass;
  preferences.putString("apSsid", apSsid);
  preferences.putString("apPass", apPassword);
  restartAtMs = millis() + 1000;
  server.send(200, "text/plain", "ok");
}

void onEspNowReceive(const uint8_t *mac, const uint8_t *data, int length) {
  (void)mac;

  if (deviceMode != MODE_SLAVE || length != sizeof(SyncMessage)) {
    return;
  }

  SyncMessage message;
  memcpy(&message, data, sizeof(message));
  if (memcmp(message.magic, "SPT1", 4) != 0 || message.groupHash != syncGroupHashValue ||
      message.counter == lastRemoteCounter) {
    return;
  }

  lastRemoteCounter = message.counter;
  if (strncmp(message.label, "NEXT", sizeof(message.label)) == 0) {
    pendingNextPulse = true;
  } else if (strncmp(message.label, "PREVIOUS", sizeof(message.label)) == 0) {
    pendingPreviousPulse = true;
  }
}

void setupEspNow() {
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    return;
  }

  esp_now_register_recv_cb(onEspNowReceive);

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, espNowBroadcastAddress, 6);
  peerInfo.channel = syncChannel;
  peerInfo.ifidx = WIFI_IF_AP;
  peerInfo.encrypt = false;
  if (!esp_now_is_peer_exist(espNowBroadcastAddress)) {
    esp_err_t result = esp_now_add_peer(&peerInfo);
    Serial.printf("ESP-NOW broadcast peer -> %s\n", result == ESP_OK ? "ok" : "failed");
  }
}

void setupPortal() {
  portalIp = defaultApIp;
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  WiFi.softAPConfig(portalIp, portalIp, apSubnet);
  WiFi.softAP(apSsid.c_str(), apPassword.c_str(), syncChannel);
  esp_wifi_set_channel(syncChannel, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_ps(WIFI_PS_NONE);
  setupEspNow();

  dnsServer.start(dnsPort, "*", portalIp);

  server.on("/", HTTP_GET, sendHome);
  server.on("/next", HTTP_POST, handleNext);
  server.on("/previous", HTTP_POST, handlePrevious);
  server.on("/play", HTTP_POST, handlePlay);
  server.on("/stop", HTTP_POST, handleStop);
  server.on("/api/number", HTTP_POST, handleNumberSetting);
  server.on("/api/boot", HTTP_POST, handleBootSetting);
  server.on("/api/mode", HTTP_POST, handleModeSetting);
  server.on("/api/sync", HTTP_POST, handleSyncSetting);
  server.on("/api/ap", HTTP_POST, handleApSetting);
  server.on("/generate_204", HTTP_GET, redirectToPortal);
  server.on("/gen_204", HTTP_GET, redirectToPortal);
  server.on("/hotspot-detect.html", HTTP_GET, sendHome);
  server.on("/connecttest.txt", HTTP_GET, redirectToPortal);
  server.on("/ncsi.txt", HTTP_GET, redirectToPortal);
  server.onNotFound(redirectToPortal);
  server.begin();
}

void setupOutput(ControlOutput &control) {
  pinMode(control.pin, OUTPUT);
  digitalWrite(control.pin, LOW);
  control.active = false;
  control.pulseStartMs = 0;
  control.activePulseMs = pulseMs;
}

void loadSettings() {
  pulseMs = preferences.getUInt("pulseMs", defaultPulseMs);
  intervalMs = preferences.getUInt("intervalMs", defaultIntervalMs);
  playOnBoot = preferences.getBool("playBoot", false);
  apSsid = preferences.getString("apSsid", "");
  apPassword = preferences.getString("apPass", defaultApPassword);
  syncGroup = preferences.getString("syncGroup", defaultSyncGroup);
  uint8_t storedMode = preferences.getUChar("mode", MODE_STANDALONE);

  pulseMs = constrain(pulseMs, 50, 2000);
  intervalMs = constrain(intervalMs, 1000, 600000);
  deviceMode = storedMode <= MODE_SLAVE ? static_cast<DeviceMode>(storedMode) : MODE_STANDALONE;
  if (!validSsidAndPass(apSsid, apPassword)) {
    apSsid = defaultApName();
    apPassword = defaultApPassword;
  }
  syncGroup.trim();
  if (syncGroup.length() == 0 || syncGroup.length() > 31) {
    syncGroup = defaultSyncGroup;
  }
  updateSyncGroupHash();
}

void checkFactoryReset(uint32_t now) {
  if (digitalRead(resetPin) == LOW) {
    if (resetLowStartMs == 0) {
      resetLowStartMs = now;
    } else if (now - resetLowStartMs >= resetHoldMs) {
      factoryReset();
    }
  } else {
    resetLowStartMs = 0;
  }
}

void processPendingSyncPulse() {
  if (deviceMode != MODE_SLAVE) {
    return;
  }

  if (pendingNextPulse) {
    pendingNextPulse = false;
    triggerPulse(nextControl, pulseMs, true);
  }

  if (pendingPreviousPulse) {
    pendingPreviousPulse = false;
    triggerPulse(previousControl, pulseMs, true);
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(resetPin, INPUT_PULLUP);
  preferences.begin("timer", false);
  loadSettings();

  if (digitalRead(resetPin) == LOW) {
    delay(resetHoldMs);
    if (digitalRead(resetPin) == LOW) {
      factoryReset();
    }
  }

  playing = deviceMode != MODE_SLAVE && playOnBoot;
  lastPlayPulseMs = millis();

  setupOutput(previousControl);
  setupOutput(nextControl);
  setupPortal();

  Serial.printf("Captive portal started: %s at http://%s\n", apSsid.c_str(),
                WiFi.softAPIP().toString().c_str());
  Serial.printf("Mode: %s, pulse: %lu ms, interval: %lu ms, play on boot: %s\n",
                modeName(deviceMode).c_str(), pulseMs, intervalMs, playOnBoot ? "yes" : "no");
}

void loop() {
  dnsServer.processNextRequest();
  server.handleClient();
  processPendingSyncPulse();

  uint32_t now = millis();

  if (restartAtMs != 0 && now - restartAtMs < 0x80000000UL) {
    ESP.restart();
  }

  checkFactoryReset(now);

  if (previousControl.active && now - previousControl.pulseStartMs >= previousControl.activePulseMs) {
    stopPulse(previousControl, now);
  }

  if (nextControl.active && now - nextControl.pulseStartMs >= nextControl.activePulseMs) {
    stopPulse(nextControl, now);
  }

  if (deviceMode != MODE_SLAVE && playing && !nextControl.active &&
      now - lastPlayPulseMs >= intervalMs) {
    lastPlayPulseMs = now;
    triggerPulse(nextControl);
  }
}
