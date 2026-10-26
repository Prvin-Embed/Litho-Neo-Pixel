/*
 * PARVEEN – ESP32-C3 Super Mini + 4x4 WS2812B Wi-Fi LED controller
 *
 * Hotspot "PARVEEN" (password 12345678) is ALWAYS on:
 *    http://192.168.4.1/        -> control page (or setup page if no Wi-Fi saved yet)
 *    http://192.168.4.1/setup   -> enter / change home Wi-Fi credentials
 * Once joined to your home Wi-Fi (IP from router / DHCP):
 *    http://PARVEEN.local/     -> control page (mDNS)
 *    http://<ip shown on page or Serial>/
 *
 * Mode, colour, speed and brightness are saved to flash and restored after power loss / reset.
 *
 * Library needed: Adafruit NeoPixel (Library Manager)
 * Board: "ESP32C3 Dev Module"  (USB CDC On Boot: Enabled for Serial output)
 */

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Adafruit_NeoPixel.h>

// ---------------- hardware ----------------
#define LED_PIN     8           // matrix DIN
#define NUM_LEDS    16
#define AP_SSID     "PARVEEN"
#define AP_PASS     "12345678"
#define MDNS_NAME   "PARVEEN"  // -> http://PARVEEN.local

Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);
WebServer   server(80);
DNSServer   dns;
Preferences prefs;

// ---------------- state ----------------
enum Mode { OFF, SOLID, RAINBOW, BREATHE, BLINK };
Mode     mode       = RAINBOW;
uint8_t  r = 255, g = 136, b = 0;
uint8_t  speed      = 50;    // 1..100
uint8_t  brightness = 120;   // 5..255
bool     haveCreds  = false;
bool     staUp      = false;

bool     settingsDirty   = false;
uint32_t settingsDirtyAt = 0;

const char* modeName(Mode m) {
  switch (m) {
    case OFF:     return "off";
    case SOLID:   return "solid";
    case RAINBOW: return "rainbow";
    case BREATHE: return "breathe";
    default:      return "blink";
  }
}
Mode modeFromName(const String& s) {
  if (s == "off")     return OFF;
  if (s == "solid")   return SOLID;
  if (s == "rainbow") return RAINBOW;
  if (s == "breathe") return BREATHE;
  return BLINK;
}

// ---------------- web pages ----------------
const char SETUP_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>PARVEEN Setup</title>
<style>
body{font-family:system-ui,sans-serif;background:#0b0d12;color:#eef1f6;display:flex;align-items:center;justify-content:center;min-height:100vh;margin:0}
.card{background:rgba(255,255,255,.06);border:1px solid rgba(255,255,255,.12);border-radius:18px;padding:28px;width:min(92vw,380px)}
h1{font-size:20px;margin:0 0 4px}p{color:#9aa3b2;font-size:13px;margin:0 0 18px}
label{display:block;font-size:12px;color:#9aa3b2;margin:12px 0 4px}
input{width:100%;padding:11px;border-radius:10px;border:1px solid rgba(255,255,255,.15);background:rgba(0,0,0,.3);color:#fff;font-size:15px;box-sizing:border-box}
button{width:100%;margin-top:20px;padding:13px;border:none;border-radius:10px;background:#ff8800;color:#000;font-weight:600;font-size:15px;cursor:pointer}
a{color:#9aa3b2;font-size:12px;display:block;text-align:center;margin-top:14px}
</style></head><body>
<form class="card" method="POST" action="/save">
<h1>PARVEEN</h1><p>Connect this device to your home Wi-Fi</p>
<label>Wi-Fi name (SSID)</label><input name="ssid" required>
<label>Password</label><input name="pass" type="password">
<button>Save &amp; connect</button>
<a href="/">Back to control page</a>
</form></body></html>
)rawliteral";

const char CONTROL_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>PARVEEN · LED Control</title>
<style>
:root{--accent:#ff8800;--bg:#0b0d12;--card:rgba(255,255,255,.06);--line:rgba(255,255,255,.12);--txt:#eef1f6;--muted:#9aa3b2}
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:Inter,system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;background:var(--bg);color:var(--txt);min-height:100vh;overflow-x:hidden}
.bg{position:fixed;inset:0;z-index:-1;overflow:hidden}
.bg span{position:absolute;border-radius:50%;filter:blur(90px);background:var(--accent);opacity:.55;animation:float 14s ease-in-out infinite alternate;transition:background .4s}
.bg span:nth-child(1){width:55vmax;height:55vmax;top:-20vmax;left:-15vmax}
.bg span:nth-child(2){width:45vmax;height:45vmax;bottom:-15vmax;right:-10vmax;animation-delay:-5s;opacity:.4}
.bg span:nth-child(3){width:30vmax;height:30vmax;top:40%;left:40%;animation-delay:-9s;opacity:.3}
@keyframes float{from{transform:translate(0,0) scale(1)}to{transform:translate(8vmax,6vmax) scale(1.15)}}
.wrap{max-width:720px;margin:0 auto;padding:40px 20px 60px}
header{display:flex;align-items:center;justify-content:space-between;margin-bottom:28px}
h1{font-size:22px;font-weight:600;letter-spacing:.5px}
.badge{font-size:12px;color:var(--muted);display:flex;align-items:center;gap:6px}
.dot{width:8px;height:8px;border-radius:50%;background:#2ecc71;box-shadow:0 0 10px #2ecc71}
.dot.off{background:#e74c3c;box-shadow:0 0 10px #e74c3c}
.card{background:var(--card);border:1px solid var(--line);border-radius:18px;padding:22px;margin-bottom:18px;backdrop-filter:blur(18px);-webkit-backdrop-filter:blur(18px)}
.card h2{font-size:13px;text-transform:uppercase;letter-spacing:1.5px;color:var(--muted);margin-bottom:14px}
.modes{display:grid;grid-template-columns:repeat(auto-fit,minmax(110px,1fr));gap:10px}
.modes button{padding:14px 10px;border-radius:12px;border:1px solid var(--line);background:rgba(255,255,255,.04);color:var(--txt);font-size:14px;cursor:pointer;transition:.2s}
.modes button:hover{background:rgba(255,255,255,.1)}
.modes button.on{background:var(--accent);border-color:transparent;color:#000;font-weight:600;box-shadow:0 0 24px var(--accent)}
.row{display:flex;align-items:center;gap:14px;margin-top:12px}
.row label{min-width:90px;font-size:14px;color:var(--muted)}
input[type=range]{flex:1;accent-color:var(--accent)}
.val{min-width:44px;text-align:right;font-variant-numeric:tabular-nums;font-size:14px}
.picker{display:flex;align-items:center;gap:16px;flex-wrap:wrap}
input[type=color]{width:64px;height:64px;border:none;border-radius:50%;background:none;cursor:pointer;padding:0}
input[type=color]::-webkit-color-swatch-wrapper{padding:0}
input[type=color]::-webkit-color-swatch{border-radius:50%;border:3px solid var(--line)}
.swatches{display:flex;gap:10px;flex-wrap:wrap}
.swatches div{width:30px;height:30px;border-radius:50%;cursor:pointer;border:2px solid rgba(255,255,255,.2);transition:.15s}
.swatches div:hover{transform:scale(1.15)}
.hex{font-family:ui-monospace,monospace;color:var(--muted);font-size:14px}
footer{text-align:center;color:var(--muted);font-size:12px;margin-top:20px}
footer a{color:var(--muted)}
</style></head><body>
<div class="bg"><span></span><span></span><span></span></div>
<div class="wrap">
<header><h1>PARVEEN</h1><div class="badge"><span class="dot" id="dot"></span><span id="ip">…</span></div></header>

<div class="card"><h2>Mode</h2><div class="modes" id="modes">
<button data-m="off">Off</button><button data-m="solid">Solid</button><button data-m="rainbow">Rainbow</button>
<button data-m="breathe">Breathe</button><button data-m="blink">Blink</button></div></div>

<div class="card"><h2>Colour</h2><div class="picker">
<input type="color" id="color" value="#ff8800"><span class="hex" id="hex">#ff8800</span>
<div class="swatches" id="sw"></div></div></div>

<div class="card"><h2>Settings</h2>
<div class="row"><label>Speed</label><input type="range" id="speed" min="1" max="100" value="50"><span class="val" id="speedv">50</span></div>
<div class="row"><label>Brightness</label><input type="range" id="bri" min="5" max="255" value="120"><span class="val" id="briv">120</span></div></div>

<footer>ESP32-C3 · <a href="/setup">Wi-Fi setup</a> · <a href="/reset" onclick="return confirm('Forget Wi-Fi and reboot?')">forget Wi-Fi</a></footer>
</div>
<script>
const $=id=>document.getElementById(id);
const presets=['#ff0000','#ff8800','#ffee00','#00ff66','#00d4ff','#2255ff','#aa33ff','#ff33aa','#ffffff'];
presets.forEach(c=>{const d=document.createElement('div');d.style.background=c;d.onclick=()=>setColor(c,true);$('sw').appendChild(d)});
let t;
function apply(s){setMode(s.mode);$('ip').textContent=s.sta?('Home Wi-Fi: '+s.ip):'Hotspot only';$('dot').classList.toggle('off',!s.sta)}
function send(p){fetch('/set?'+new URLSearchParams(p)).then(r=>r.json()).then(apply).catch(()=>{})}
function debounce(p){clearTimeout(t);t=setTimeout(()=>send(p),80)}
function setColor(c,push){document.documentElement.style.setProperty('--accent',c);$('color').value=c;$('hex').textContent=c;if(push)send({color:c.slice(1)})}
function setMode(m){[...$('modes').children].forEach(b=>b.classList.toggle('on',b.dataset.m===m))}
$('color').addEventListener('input',e=>{setColor(e.target.value,false);debounce({color:e.target.value.slice(1)})});
$('modes').addEventListener('click',e=>{if(e.target.dataset.m){setMode(e.target.dataset.m);send({mode:e.target.dataset.m})}});
$('speed').addEventListener('input',e=>{$('speedv').textContent=e.target.value;debounce({speed:e.target.value})});
$('bri').addEventListener('input',e=>{$('briv').textContent=e.target.value;debounce({bri:e.target.value})});
function refresh(){fetch('/state').then(r=>r.json()).then(s=>{apply(s);setColor('#'+s.color,false);
 $('speed').value=s.speed;$('speedv').textContent=s.speed;$('bri').value=s.bri;$('briv').textContent=s.bri}).catch(()=>{})}
refresh();setInterval(()=>fetch('/state').then(r=>r.json()).then(apply).catch(()=>{}),5000);
</script></body></html>
)rawliteral";

// ---------------- helpers ----------------
String stateJson() {
  char buf[200];
  snprintf(buf, sizeof(buf),
           "{\"mode\":\"%s\",\"color\":\"%02x%02x%02x\",\"speed\":%u,\"bri\":%u,\"sta\":%s,\"ip\":\"%s\"}",
           modeName(mode), r, g, b, speed, brightness,
           staUp ? "true" : "false", WiFi.localIP().toString().c_str());
  return String(buf);
}

// ---------------- settings persistence ----------------
void loadSettings() {
  prefs.begin("led", true);
  mode       = (Mode)prefs.getUChar("mode", RAINBOW);
  r          = prefs.getUChar("r",     255);
  g          = prefs.getUChar("g",     136);
  b          = prefs.getUChar("b",     0);
  speed      = prefs.getUChar("speed", 50);
  brightness = prefs.getUChar("bri",   120);
  prefs.end();
}

void saveSettings() {
  prefs.begin("led", false);
  prefs.putUChar("mode",  (uint8_t)mode);
  prefs.putUChar("r",     r);
  prefs.putUChar("g",     g);
  prefs.putUChar("b",     b);
  prefs.putUChar("speed", speed);
  prefs.putUChar("bri",   brightness);
  prefs.end();
  Serial.println("Settings saved");
}

void markDirty() { settingsDirty = true; settingsDirtyAt = millis(); }

// write to flash 2 s after the last change (avoids wearing flash while dragging sliders)
void serviceSettings() {
  if (settingsDirty && millis() - settingsDirtyAt > 2000) {
    settingsDirty = false;
    saveSettings();
  }
}

// ---------------- LED animation (non-blocking) ----------------
void updateLeds() {
  static uint32_t lastFrame = 0, lastBlink = 0;
  static uint16_t hue = 0;
  static bool blinkOn = true;
  uint32_t now = millis();
  if (now - lastFrame < 20) return;          // ~50 fps
  lastFrame = now;

  switch (mode) {
    case OFF:
      strip.clear();
      break;
    case SOLID:
      strip.fill(strip.Color(r, g, b));
      break;
    case RAINBOW:
      hue += map(speed, 1, 100, 20, 600);
      for (int i = 0; i < NUM_LEDS; i++)
        strip.setPixelColor(i, strip.ColorHSV(hue + i * (65536L / NUM_LEDS)));
      break;
    case BREATHE: {
      float period = map(speed, 1, 100, 6000, 800);
      float lvl = (sinf(2 * PI * now / period - PI / 2) + 1) / 2;
      lvl = 0.03 + 0.97 * lvl * lvl;
      strip.fill(strip.Color(r * lvl, g * lvl, b * lvl));
      break;
    }
    case BLINK: {
      uint32_t interval = map(speed, 1, 100, 1000, 50);
      if (now - lastBlink >= interval) { lastBlink = now; blinkOn = !blinkOn; }
      if (blinkOn) strip.fill(strip.Color(r, g, b)); else strip.clear();
      break;
    }
  }
  strip.show();
}

// ---------------- HTTP handlers ----------------
void handleRoot()  { server.send_P(200, "text/html", haveCreds ? CONTROL_HTML : SETUP_HTML); }
void handleSetup() { server.send_P(200, "text/html", SETUP_HTML); }
void handleState() { server.send(200, "application/json", stateJson()); }

void handleSet() {
  if (server.hasArg("mode"))  mode = modeFromName(server.arg("mode"));
  if (server.hasArg("color")) {
    uint32_t c = strtoul(server.arg("color").c_str(), nullptr, 16);
    r = (c >> 16) & 0xFF; g = (c >> 8) & 0xFF; b = c & 0xFF;
    if (mode == OFF || mode == RAINBOW) mode = SOLID;
  }
  if (server.hasArg("speed")) speed = constrain(server.arg("speed").toInt(), 1, 100);
  if (server.hasArg("bri")) {
    brightness = constrain(server.arg("bri").toInt(), 5, 255);
    strip.setBrightness(brightness);
  }
  markDirty();
  server.send(200, "application/json", stateJson());
}

void handleSave() {
  prefs.begin("wifi", false);
  prefs.putString("ssid", server.arg("ssid"));
  prefs.putString("pass", server.arg("pass"));
  prefs.end();
  server.send(200, "text/html",
    "<body style='font-family:system-ui;background:#0b0d12;color:#eef1f6;text-align:center;padding:30vh 20px 0'>"
    "<h2>Saved. Rebooting…</h2>"
    "<p>Stay on the PARVEEN hotspot and open <b>http://192.168.4.1/</b> — the page will show the home-network IP.<br>"
    "Or on your home Wi-Fi open <b>http://PARVEEN.local/</b></p></body>");
  delay(1500);
  ESP.restart();
}

void handleReset() {
  prefs.begin("wifi", false); prefs.clear(); prefs.end();
  server.send(200, "text/plain", "Wi-Fi cleared. Rebooting...");
  delay(1000);
  ESP.restart();
}

// Captive-portal probes -> redirect to this device's own address on whichever interface was used
void handleCaptive() {
  server.sendHeader("Location", "http://" + server.client().localIP().toString() + "/", true);
  server.send(302, "text/plain", "");
}

// ---------------- Wi-Fi ----------------
void startWiFi() {
  prefs.begin("wifi", true);
  String ssid = prefs.getString("ssid", "");
  String pass = prefs.getString("pass", "");
  prefs.end();
  haveCreds = !ssid.isEmpty();

  WiFi.persistent(false);
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  WiFi.softAP(AP_SSID, AP_PASS, 1, 0, 4);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);             // stability fix for C3 Super Mini
  delay(100);
  dns.start(53, "*", WiFi.softAPIP());
  Serial.printf("Hotspot %s at %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());

  if (haveCreds) {
    WiFi.setAutoReconnect(true);
    WiFi.begin(ssid.c_str(), pass.c_str());        // DHCP: router assigns the IP
    Serial.printf("Connecting to %s ...\n", ssid.c_str());
  } else {
    Serial.println("No Wi-Fi saved. Open http://192.168.4.1/ on the hotspot to set it up.");
  }
}

// Called from loop: tracks home Wi-Fi connection state
void serviceWiFi() {
  bool connected = (WiFi.status() == WL_CONNECTED);

  if (connected && !staUp) {
    staUp = true;
    Serial.printf("Connected. IP: %s   (http://%s.local/)\n",
                  WiFi.localIP().toString().c_str(), MDNS_NAME);
    if (MDNS.begin(MDNS_NAME)) MDNS.addService("http", "tcp", 80);
  } else if (!connected && staUp) {
    staUp = false;
    Serial.println("Wi-Fi lost, reconnecting...");
  }
}

// ---------------- setup / loop ----------------
void setup() {
  Serial.begin(115200);
  loadSettings();                                 // restore last mode/colour/speed/brightness
  strip.begin();
  strip.setBrightness(brightness);
  strip.show();

  startWiFi();

  server.on("/",      HTTP_GET,  handleRoot);
  server.on("/setup", HTTP_GET,  handleSetup);
  server.on("/state", HTTP_GET,  handleState);
  server.on("/set",   HTTP_GET,  handleSet);
  server.on("/save",  HTTP_POST, handleSave);
  server.on("/reset", HTTP_GET,  handleReset);
  server.on("/generate_204",              handleCaptive);   // Android
  server.on("/gen_204",                   handleCaptive);
  server.on("/hotspot-detect.html",       handleCaptive);   // iOS / macOS
  server.on("/library/test/success.html", handleCaptive);
  server.on("/connecttest.txt",           handleCaptive);   // Windows
  server.on("/ncsi.txt",                  handleCaptive);
  server.on("/fwlink",                    handleCaptive);
  server.onNotFound(handleCaptive);
  server.begin();
}

void loop() {
  dns.processNextRequest();
  server.handleClient();
  serviceWiFi();
  serviceSettings();
  updateLeds();
}
