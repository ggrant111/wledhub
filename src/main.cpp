#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <WiFiManager.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <SPI.h>
#include <math.h>
#include "ddp_packet.h"

static const char *VERSION="0.1.1";
static constexpr int MAX_DEVICES=16;
static constexpr uint32_t POLL_MS=12000;
static constexpr uint32_t DISCOVERY_MS=60000;
static constexpr uint32_t FRAME_MS=40;
static constexpr uint16_t MAX_LEDS=2048; // frame generated in chunks; bounded workload

struct WDevice {
  IPAddress ip;
  String name;
  String mac;
  String version;
  uint16_t count=0;
  int bri=0;
  int effect=0;
  bool on=false;
  bool live=false;
  bool online=false;
  String savedState;
};
WDevice devices[MAX_DEVICES];
int deviceCount=0, selected=-1;
bool streaming=false;
int effectMode=0, speed=128, intensity=128, brightness=180;
uint32_t rgb=0xFF4400;
unsigned long streamSince=0, lastFrame=0, lastPoll=0, lastDiscovery=0, lastScreen=0;
uint8_t sequenceNum=1;
WiFiUDP ddpUdp;
WebServer server(80);
Preferences prefs;
TFT_eSPI screen;
// CYD XPT2046 touch is wired to its own SPI bus, NOT TFT SPI.
SPIClass touchSPI(VSPI);
XPT2046_Touchscreen touch(33, 36);
bool touchDown=false;
bool invertPanel=true; // v0.1.1 default: CYD units that display negative colors
bool touchSwap=false, touchFlipX=false, touchFlipY=false;
uint16_t touchMinX=200,touchMaxX=3800,touchMinY=200,touchMaxY=3800;
int touchX=-1,touchY=-1, touchRawX=-1,touchRawY=-1;
uint32_t touches=0;
String lastError;

String ipText(IPAddress ip) { return ip.toString(); }
int indexFor(IPAddress ip) {
  for(int i=0;i<deviceCount;i++) if(devices[i].ip==ip) return i;
  return -1;
}
bool readWled(IPAddress ip, JsonDocument &doc) {
  HTTPClient http;
  http.setConnectTimeout(750);
  http.setTimeout(1200);
  if(!http.begin("http://"+ipText(ip)+"/json/si")) return false;
  int status=http.GET();
  if(status!=200) { http.end(); return false; }
  DeserializationError error=deserializeJson(doc,http.getStream());
  http.end();
  return !error && doc["info"].is<JsonObject>() && doc["state"].is<JsonObject>();
}
bool sendJson(IPAddress ip, const String &json) {
  HTTPClient http;
  http.setConnectTimeout(750); http.setTimeout(1500);
  if(!http.begin("http://"+ipText(ip)+"/json/state")) return false;
  http.addHeader("Content-Type","application/json");
  int status=http.POST(json);
  http.end();
  return status>=200 && status<300;
}
bool addIP(IPAddress ip) {
  if(ip[0]==0) return false;
  int idx=indexFor(ip);
  if(idx<0) {
    if(deviceCount>=MAX_DEVICES) return false;
    idx=deviceCount++;
    devices[idx].ip=ip;
    devices[idx].name=ip.toString();
  }
  JsonDocument doc;
  if(!readWled(ip,doc)) { devices[idx].online=false; return false; }
  JsonObject info=doc["info"];
  JsonObject state=doc["state"];
  devices[idx].online=true;
  devices[idx].name=String(info["name"] | "WLED");
  devices[idx].mac=String(info["mac"] | "");
  devices[idx].version=String(info["ver"] | "");
  devices[idx].count=constrain((int)(info["leds"]["count"] | 0),0,65535);
  devices[idx].on=state["on"] | false;
  devices[idx].bri=state["bri"] | 0;
  devices[idx].live=info["live"] | false;
  if(state["seg"].is<JsonArray>() && state["seg"].size()) devices[idx].effect=state["seg"][0]["fx"] | 0;
  if(selected<0) selected=idx;
  return true;
}
void rememberIPs() {
  String saved;
  for(int i=0;i<deviceCount;i++) { if(i) saved+=","; saved+=devices[i].ip.toString(); }
  prefs.putString("devices",saved);
}
void loadIPs() {
  String s=prefs.getString("devices","");
  int begin=0;
  while(begin<s.length()) {
    int end=s.indexOf(',',begin); if(end<0) end=s.length();
    IPAddress ip;
    if(ip.fromString(s.substring(begin,end)) && indexFor(ip)<0 && deviceCount<MAX_DEVICES) {
      devices[deviceCount].ip=ip;
      devices[deviceCount].name=ip.toString();
      deviceCount++;
    }
    begin=end+1;
  }
}
void discover() {
  int n=MDNS.queryService("wled","tcp");
  for(int i=0;i<n;i++) {
    IPAddress ip=MDNS.IP(i);
    if(ip[0]) addIP(ip);
  }
  for(int i=0;i<deviceCount;i++) addIP(devices[i].ip);
  rememberIPs();
  lastDiscovery=millis();
}
void pollDevices() {
  for(int i=0;i<deviceCount;i++) {
    if(streaming && i==selected) continue; // don't overwrite pre-stream view
    addIP(devices[i].ip);
  }
  lastPoll=millis();
}
uint8_t scaleColor(uint8_t value,float gain) {
  return (uint8_t)constrain((int)(value*gain),0,255);
}
void pixelColor(uint16_t i,uint16_t count,uint32_t elapsed,uint8_t &r,uint8_t &g,uint8_t &b) {
  float t=(float)elapsed/1000.0f;
  float rate=0.3f+((float)speed/255.0f)*4.5f;
  float factor=1.0f;
  if(effectMode==1) { // Pulse: adjustable floor via intensity
    float wave=0.5f+0.5f*sinf(t*rate*6.2831853f);
    float depth=(float)intensity/255.0f;
    factor=(1.0f-depth)+depth*wave;
  } else if(effectMode==2) { // Chase: rolling soft-tail pixel, intensity controls width
    float pos=fmodf(t*rate*12.0f,(float)max(1,(int)count));
    float delta=fmodf((float)i-pos+(float)count,(float)max(1,(int)count));
    float tail=1.0f+((float)intensity/255.0f)*((float)count*0.35f);
    factor=expf(-delta/tail);
  } else { // Solid: intensity controls output strength
    factor=0.1f+0.9f*((float)intensity/255.0f);
  }
  float gain=factor*((float)brightness/255.0f);
  r=scaleColor((rgb>>16)&255,gain);
  g=scaleColor((rgb>>8)&255,gain);
  b=scaleColor(rgb&255,gain);
}
bool emitFrame() {
  if(!streaming || selected<0 || !devices[selected].online) return false;
  uint16_t count=min(devices[selected].count, MAX_LEDS);
  if(!count) return false;
  uint8_t packet[ddp::HEADER_LEN+ddp::PIXELS_PER_PACKET*3];
  uint32_t elapsed=millis()-streamSince;
  for(uint16_t start=0;start<count;start+=ddp::PIXELS_PER_PACKET) {
    uint16_t pixels=min((int)ddp::PIXELS_PER_PACKET,(int)(count-start));
    bool push=(start+pixels==count);
    ddp::header(packet,sequenceNum++, (uint32_t)start*3,pixels*3,push);
    for(uint16_t j=0;j<pixels;j++) {
      uint8_t r,g,b;
      pixelColor(start+j,count,elapsed,r,g,b);
      packet[10+3*j]=r; packet[11+3*j]=g; packet[12+3*j]=b;
    }
    if(!ddpUdp.beginPacket(devices[selected].ip,ddp::PORT)) return false;
    ddpUdp.write(packet,10+pixels*3);
    if(!ddpUdp.endPacket()) return false;
  }
  return true;
}
bool startStream() {
  if(selected<0 || selected>=deviceCount || !devices[selected].online) {lastError="Select an online WLED device";return false;}
  if(streaming) return true;
  JsonDocument doc;
  if(!readWled(devices[selected].ip,doc)) {lastError="Cannot fetch device state";return false;}
  serializeJson(doc["state"],devices[selected].savedState);
  if(devices[selected].count==0 || devices[selected].count>MAX_LEDS) {lastError="LED count unsupported (>2048)";return false;}
  // Set on/brightness and allow realtime packets to take control.
  JsonDocument cmd;
  cmd["on"]=true; cmd["bri"]=255; cmd["lor"]=0;
  String body; serializeJson(cmd,body);
  if(!sendJson(devices[selected].ip,body)) {lastError="Device rejected DDP setup";return false;}
  streamSince=millis(); lastFrame=0; streaming=true; lastError="";
  return true;
}
void stopStream() {
  if(!streaming || selected<0) return;
  streaming=false;
  IPAddress ip=devices[selected].ip;
  sendJson(ip,"{\"live\":false}");
  if(devices[selected].savedState.length()) sendJson(ip,devices[selected].savedState);
  devices[selected].savedState="";
  addIP(ip);
}
String statusJSON() {
  JsonDocument doc;
  doc["version"]=VERSION;
  doc["wifi"]=WiFi.SSID();
  doc["ip"]=WiFi.localIP().toString();
  doc["selected"]=selected;
  doc["streaming"]=streaming;
  doc["effect"]=effectMode; doc["speed"]=speed; doc["intensity"]=intensity;
  doc["brightness"]=brightness; doc["color"]=String(rgb,HEX);
  doc["error"]=lastError;
  doc["displayInvert"]=invertPanel;
  doc["touchCount"]=touches;
  doc["touchX"]=touchX; doc["touchY"]=touchY;
  doc["touchRawX"]=touchRawX; doc["touchRawY"]=touchRawY;
  doc["touchSwap"]=touchSwap; doc["touchFlipX"]=touchFlipX; doc["touchFlipY"]=touchFlipY;
  JsonArray a=doc["devices"].to<JsonArray>();
  for(int i=0;i<deviceCount;i++) {
    JsonObject v=a.add<JsonObject>();
    v["index"]=i; v["name"]=devices[i].name;
    v["ip"]=ipText(devices[i].ip); v["online"]=devices[i].online;
    v["on"]=devices[i].on; v["bri"]=devices[i].bri;
    v["fx"]=devices[i].effect; v["count"]=devices[i].count;
    v["live"]=devices[i].live; v["version"]=devices[i].version;
  }
  String output; serializeJson(doc,output); return output;
}
const char INDEX_HTML[] PROGMEM=R"HTML(
<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>WLEDHub</title><style>
:root{font-family:system-ui;background:#0c1220;color:#f2f5fc}body{max-width:800px;margin:20px auto;padding:12px}
h1{margin-bottom:0;color:#64dfcb}small,.muted{color:#93a4bc}
section{background:#172236;border:1px solid #29374d;border-radius:15px;margin:16px 0;padding:17px}
button,select,input{font:inherit;border-radius:8px;padding:10px;background:#243651;color:white;border:1px solid #435775}
button{cursor:pointer}button.primary{background:#168f80}button.danger{background:#813345}
input[type=range]{width:100%;padding:0}input[type=color]{width:90px;height:44px;padding:2px}
.line{display:flex;gap:10px;flex-wrap:wrap;align-items:center;justify-content:space-between}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:12px}
label{display:block;margin-top:12px}#msg{color:#ffc77a;min-height:18px}
</style></head><body>
<h1>WLEDHub</h1><small>CYD lighting command center · <span id="v"></span></small>
<section><div class="line"><h2>Controllers</h2><button onclick="action('/api/discover')">Discover</button></div>
<select id="devices" onchange="choose(this.value)" style="width:100%"></select>
<div class="line"><span id="info">Searching...</span><button onclick="toggle()">Toggle Power</button></div>
<div class="line"><input id="manual" placeholder="Add WLED IP, e.g. 192.168.1.60"><button onclick="add()">Add IP</button></div>
<label>Native brightness <span id="nativeValue"></span><input type="range" min="0" max="255" id="native" onchange="nativeBrightness(this.value)"></label>
</section>
<section><h2>DDP Effects</h2><div class="grid">
<label>Effect<select id="effect"><option value="0">Solid</option><option value="1">Pulse</option><option value="2">Chase</option></select></label>
<label>Color<input type="color" id="color" value="#ff4400"></label>
</div>
<label>Speed <output id="speedVal"></output><input id="speed" type="range" min="0" max="255" value="128" oninput="document.getElementById('speedVal').textContent=value"></label>
<label>Intensity <output id="intensityVal"></output><input id="intensity" type="range" min="0" max="255" value="128" oninput="document.getElementById('intensityVal').textContent=value"></label>
<label>DDP brightness <output id="brightnessVal"></output><input id="brightness" type="range" min="0" max="255" value="180" oninput="document.getElementById('brightnessVal').textContent=value"></label>
<div class="line" style="margin-top:20px"><button onclick="apply()">Apply Parameters</button><button class="primary" onclick="action('/api/start')">Start DDP</button><button class="danger" onclick="action('/api/stop')">Stop / Restore</button></div>
<p id="msg"></p>
</section><section><h2>Maintenance</h2><p id="status" class="muted"></p>
<a style="color:#64dfcb" href="/update">OTA Firmware Update</a>
<h3>Display & touch diagnostics</h3>
<label><input type="checkbox" id="invert"> Invert display colors</label>
<label><input type="checkbox" id="swap"> Swap touch axes</label>
<label><input type="checkbox" id="flipx"> Flip touch X</label>
<label><input type="checkbox" id="flipy"> Flip touch Y</label>
<button onclick="displaySettings()">Save display and touch settings</button>
<p class="muted" id="touchStatus">No touches yet</p>
</section>
<script>
let state={},editing=false;
async function action(url,body){try {let response=await fetch(url,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(body||{})});let data=await response.json();document.getElementById('msg').textContent=data.error||(!response.ok?'Request failed':'');await refresh(true)}catch(e){document.getElementById('msg').textContent=e.message}}
function choose(index){action('/api/select',{index})}
function add(){action('/api/add',{ip:document.getElementById('manual').value})}
function toggle(){let d=state.devices?.[state.selected];if(d)action('/api/native',{on:d.on?'0':'1'})}
function nativeBrightness(bri){action('/api/native',{bri})}
function displaySettings(){action('/api/display',{invert:document.getElementById('invert').checked?'1':'0',swap:document.getElementById('swap').checked?'1':'0',flipx:document.getElementById('flipx').checked?'1':'0',flipy:document.getElementById('flipy').checked?'1':'0'})}
function apply(){action('/api/params',{effect:document.getElementById('effect').value,color:document.getElementById('color').value.slice(1),speed:document.getElementById('speed').value,intensity:document.getElementById('intensity').value,brightness:document.getElementById('brightness').value})}
async function refresh(force=false){
try{
let r=await fetch('/api/status');state=await r.json();
let list=document.getElementById('devices'),previous=list.value;
list.innerHTML=state.devices.map(d=>'<option value="'+d.index+'">'+(d.online?'● ':'○ ')+d.name+' ('+d.ip+')</option>').join('');
list.value=String(state.selected);let d=state.devices[state.selected];
document.getElementById('info').textContent=d?(d.online?'Online':'Offline')+' · '+d.count+' LEDs · WLED '+d.version+' · '+(d.on?'On':'Off'):'No device selected';
document.getElementById('native').value=d?.bri??0;
document.getElementById('nativeValue').textContent=d?.bri??0;
document.getElementById('v').textContent=state.version;
document.getElementById('invert').checked=state.displayInvert;
document.getElementById('swap').checked=state.touchSwap;
document.getElementById('flipx').checked=state.touchFlipX;
document.getElementById('flipy').checked=state.touchFlipY;
document.getElementById('touchStatus').textContent='Detected touches: '+state.touchCount+' · Last screen: ('+state.touchX+','+state.touchY+') · Raw: ('+state.touchRawX+','+state.touchRawY+')';
document.getElementById('status').textContent=(state.streaming?'DDP streaming':'Idle')+' · '+state.ip+' · '+state.wifi;
if(force||!editing){
for(let k of ['effect','speed','intensity','brightness'])document.getElementById(k).value=state[k];
document.getElementById('color').value='#'+state.color.padStart(6,'0');
for(let k of ['speed','intensity','brightness'])document.getElementById(k+'Val').textContent=state[k];
}
if(state.error) document.getElementById('msg').textContent=state.error;
}catch(e){document.getElementById('msg').textContent='Connection lost: '+e.message}
}
for(let id of ['effect','color','speed','intensity','brightness'])document.addEventListener('input',e=>{if(e.target.id===id)editing=true});
document.querySelector('button[onclick="apply()"]').addEventListener('click',()=>editing=false);
refresh();setInterval(()=>refresh(),5000);
</script></body></html>
)HTML";

void reply(bool ok=true) {
  server.send(ok?200:400,"application/json",statusJSON());
}
void drawScreen();
void routes() {
  server.on("/",HTTP_GET,[](){server.send_P(200,"text/html",INDEX_HTML);});
  server.on("/api/status",HTTP_GET,[](){server.send(200,"application/json",statusJSON());});
  server.on("/api/discover",HTTP_POST,[](){discover();reply();});
  server.on("/api/add",HTTP_POST,[](){
    IPAddress ip;
    if(!ip.fromString(server.arg("ip"))) {lastError="Invalid IP address";reply(false);return;}
    bool ok=addIP(ip); rememberIPs();
    if(!ok) lastError="No WLED device responded at "+ip.toString();else lastError="";
    reply(ok);
  });
  server.on("/api/select",HTTP_POST,[](){
    int i=server.arg("index").toInt();
    if(i<0||i>=deviceCount) {lastError="Invalid selection";reply(false);return;}
    stopStream();selected=i;lastError="";reply();
  });
  server.on("/api/native",HTTP_POST,[](){
    if(selected<0) {lastError="Select a device";reply(false);return;}
    stopStream();
    JsonDocument cmd;
    if(server.hasArg("on"))cmd["on"]=(server.arg("on")=="1");
    if(server.hasArg("bri"))cmd["bri"]=constrain(server.arg("bri").toInt(),0,255);
    String data;serializeJson(cmd,data);
    bool ok=sendJson(devices[selected].ip,data);
    lastError=ok?"":"Native update failed";
    addIP(devices[selected].ip);reply(ok);
  });
  server.on("/api/params",HTTP_POST,[](){
    effectMode=constrain(server.arg("effect").toInt(),0,2);
    speed=constrain(server.arg("speed").toInt(),0,255);
    intensity=constrain(server.arg("intensity").toInt(),0,255);
    brightness=constrain(server.arg("brightness").toInt(),0,255);
    String color=server.arg("color");if(color.startsWith("#"))color.remove(0,1);
    if(color.length()!=6) {lastError="Color requires 6 hex characters";reply(false);return;}
    for(unsigned int i=0;i<color.length();i++)if(!isxdigit(color[i])){lastError="Invalid color";reply(false);return;}
    rgb=strtoul(color.c_str(),nullptr,16);lastError="";reply();
  });
  server.on("/api/display",HTTP_POST,[](){
    invertPanel=server.arg("invert")=="1";
    touchSwap=server.arg("swap")=="1";
    touchFlipX=server.arg("flipx")=="1";
    touchFlipY=server.arg("flipy")=="1";
    prefs.putBool("invert",invertPanel);
    prefs.putBool("swap",touchSwap);
    prefs.putBool("flipx",touchFlipX);
    prefs.putBool("flipy",touchFlipY);
    screen.invertDisplay(invertPanel);
    drawScreen();
    reply();
  });
  server.on("/api/start",HTTP_POST,[](){reply(startStream());});
  server.on("/api/stop",HTTP_POST,[](){stopStream();lastError="";reply();});
  server.on("/update",HTTP_GET,[](){
    server.send(200,"text/html","<html><body style='font-family:system-ui'><h2>WLEDHub Firmware OTA</h2><p>Trusted LAN only. Upload firmware.bin compiled for CYD.</p><form method='POST' action='/update' enctype='multipart/form-data'><input type='file' name='firmware' accept='.bin' required><button>Upload & Reboot</button></form></body></html>");
  });
  server.on("/update",HTTP_POST,[](){
    bool ok=!Update.hasError();
    server.send(ok?200:500,"text/plain",ok?"Update completed. Rebooting...":"Update failed.");
    delay(300);if(ok)ESP.restart();
  },[](){
    HTTPUpload &upload=server.upload();
    if(upload.status==UPLOAD_FILE_START) {
      stopStream();
      if(!Update.begin(UPDATE_SIZE_UNKNOWN,U_FLASH))lastError="Update begin failed";
    } else if(upload.status==UPLOAD_FILE_WRITE) {
      if(Update.write(upload.buf,upload.currentSize)!=upload.currentSize)lastError="Update write failed";
    } else if(upload.status==UPLOAD_FILE_END) {
      if(!Update.end(true))lastError="Update validation failed";
    }
  });
  server.begin();
}
void drawScreen() {
  screen.fillScreen(TFT_BLACK);
  screen.setTextColor(TFT_CYAN,TFT_BLACK); screen.setTextSize(2);
  screen.setCursor(8,8);screen.print("WLEDHub ");screen.print(VERSION);
  screen.setTextSize(1);
  screen.setTextColor(TFT_WHITE,TFT_BLACK);
  screen.setCursor(8,37);screen.print(WiFi.localIP());
  screen.setCursor(8,56);screen.printf("Found: %d  Select: %d",deviceCount,selected+1);
  if(selected>=0) {
    WDevice &d=devices[selected];
    screen.setTextSize(2);screen.setCursor(8,78);
    screen.print(d.name.substring(0,19));
    screen.setTextSize(1);screen.setCursor(8,105);
    screen.printf("%s | %u LEDs | bri %d",d.online?"ONLINE":"OFFLINE",d.count,d.bri);
  }
  screen.setCursor(8,127);screen.printf("DDP: %s | %s",streaming?"PLAYING":"STOPPED",effectMode==0?"Solid":effectMode==1?"Pulse":"Chase");
  screen.setCursor(8,145);screen.printf("Color #%06lX S:%d I:%d",(unsigned long)rgb,speed,intensity);
  screen.fillRoundRect(5,176,72,44,5,TFT_DARKGREY);
  screen.fillRoundRect(84,176,72,44,5,TFT_DARKGREY);
  screen.fillRoundRect(163,176,72,44,5,streaming?TFT_RED:TFT_DARKGREEN);
  screen.setTextSize(2);screen.setTextColor(TFT_WHITE);
  screen.setCursor(24,190);screen.print("<");
  screen.setCursor(113,190);screen.print(">");
  screen.setCursor(170,190);screen.print(streaming?"STOP":"PLAY");
  screen.setTextSize(1); screen.setCursor(8,228);
  screen.print("Web: ");screen.print(WiFi.localIP());
}
void handleTouch() {
  // Read the separate XPT2046 SPI bus (GPIO 25/32/39, CS33 IRQ36).
  bool down=touch.touched();
  if(down && !touchDown) {
    TS_Point p=touch.getPoint();
    touchRawX=p.x; touchRawY=p.y;
    int a=touchSwap?p.y:p.x;
    int b=touchSwap?p.x:p.y;
    touchX=constrain(map(a,touchMinX,touchMaxX,0,screen.width()-1),0,screen.width()-1);
    touchY=constrain(map(b,touchMinY,touchMaxY,0,screen.height()-1),0,screen.height()-1);
    if(touchFlipX)touchX=screen.width()-1-touchX;
    if(touchFlipY)touchY=screen.height()-1-touchY;
    touches++;
    Serial.printf("TOUCH raw=(%d,%d) mapped=(%d,%d) count=%lu\n",p.x,p.y,touchX,touchY,(unsigned long)touches);
    // Yellow marker helps distinguish detection from mis-calibration.
    screen.fillCircle(touchX,touchY,4,TFT_YELLOW);
    if(touchY>=168 && touchY<=226 && deviceCount) {
      if(touchX<80) {stopStream();selected=(selected+deviceCount-1)%deviceCount;}
      else if(touchX<160) {stopStream();selected=(selected+1)%deviceCount;}
      else {if(streaming)stopStream();else startStream();}
      drawScreen();
    }
  }
  touchDown=down;
}

void setup() {
  Serial.begin(115200);
  pinMode(21,OUTPUT); digitalWrite(21,HIGH);
  screen.init();screen.setRotation(1);
  touchSPI.begin(25,39,32,33);
  touch.begin(touchSPI);
  touch.setRotation(1);
  // Touch SPI is separate; TFT_eSPI::setTouch() would read the wrong bus.
  screen.fillScreen(TFT_BLACK);
  screen.setTextColor(TFT_CYAN,TFT_BLACK);screen.setTextSize(2);
  screen.setCursor(8,20);screen.println("WLEDHub booting");
  WiFi.mode(WIFI_STA);
  WiFiManager manager;
  manager.setConfigPortalTimeout(180);
  if(!manager.autoConnect("WLEDHub-Setup")) ESP.restart();
  prefs.begin("wledhub",false);
  invertPanel=prefs.getBool("invert",true);
  touchSwap=prefs.getBool("swap",false);
  touchFlipX=prefs.getBool("flipx",false);
  touchFlipY=prefs.getBool("flipy",false);
  screen.invertDisplay(invertPanel);
  loadIPs();
  MDNS.begin("wledhub");
  discover();
  routes();
  drawScreen();
}
void loop() {
  server.handleClient();
  if(WiFi.status()!=WL_CONNECTED) {delay(50);return;}
  handleTouch();
  unsigned long now=millis();
  if(streaming && now-lastFrame>=FRAME_MS) {
    if(!emitFrame()){lastError="DDP send failure";stopStream();}
    lastFrame=now;
  }
  if(now-lastPoll>=POLL_MS) pollDevices();
  if(!streaming && now-lastDiscovery>=DISCOVERY_MS) discover();
  if(now-lastScreen>=3000) {drawScreen();lastScreen=now;}
  delay(1);
}
