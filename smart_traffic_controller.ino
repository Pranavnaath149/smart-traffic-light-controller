/*
  Smart Traffic Light - corrected version
  - Round-robin fixed: when no IR for >5s, cycles through all lanes (does NOT skip)
  - Emojis replaced with HTML numeric entities
  - Fancy UI with animated gradient and glow
  - Emergency, Pedestrian all-red blink, Night-mode preserved
  - Shows currentGreenLane in /status for the dashboard
*/

#include <WiFi.h>
#include <WebServer.h>

// ---------- CONFIG ----------
#define DEBUG_SERIAL false

// ---------- PIN MAP ----------
int lanePins[4][3] = {
  {14, 27, 26},  // Lane 1: R, Y, G
  {25, 33, 32},  // Lane 2
  {23, 22, 21},  // Lane 3
  {19, 18, 15}   // Lane 4 (G on 15)
};

// MUX pins
const int MUX_S0 = 4;
const int MUX_S1 = 16;
const int MUX_S2 = 17;
const int MUX_S3 = 5;
const int MUX_SIG = 34; // ADC1

// MUX channel mapping
int laneIR[4][2] = {
  {0,1}, {2,3}, {4,5}, {6,7}
};

// ---------- NETWORK ----------
const char *AP_SSID = "TrafficLightESP";
const char *AP_PASS = "12345678";
WebServer server(80);

// ---------- TIMINGS & THRESHOLDS ----------
const int IR_THRESHOLD = 1000;               // tune for your IR module
const unsigned long GREEN_LOW_MS = 4000UL;   // density==1
const unsigned long GREEN_HIGH_MS = 7000UL;  // density==2 (blinking)
const unsigned long BLINK_MS = 1000UL;       // 1s on/off for priority
const unsigned long YELLOW_MS = 2000UL;
const unsigned long PED_ALLRED_MS = 6000UL;  // pedestrian all-red (blink red)
const unsigned long NO_IR_ROUNDROBIN_MS = 5000UL; // 5s -> round-robin fallback
const int MAX_CONSECUTIVE_PRIORITY = 2;      // fairness

// ---------- STATE ----------
volatile bool pedAllRedRequested = false;
volatile int emergencyLane = -1; // -1 none, 0..3 lane index
volatile bool nightMode = false;
bool autoMode = true;           // when false due to emergency override
bool roundRobinOnly = false;
unsigned long lastAnyDetectionMillis = 0; // last time any IR was seen

// fairness trackers
int lastPriorityLane = -1;
int consecutivePriorityCount = 0;

// current green for dashboard
int currentGreenLane = -1;
bool currentGreenBlink = false;

// ---------- FORWARD DECLARATIONS ----------
void handleRoot();
void handleStatus();
void handlePed();
void handleE0(); void handleE1(); void handleE2(); void handleE3();
void handleClear();
void handleNight();

// ---------- UTIL: MUX & ADC ----------
int readMuxChannel(int ch) {
  digitalWrite(MUX_S0, (ch >> 0) & 1);
  digitalWrite(MUX_S1, (ch >> 1) & 1);
  digitalWrite(MUX_S2, (ch >> 2) & 1);
  digitalWrite(MUX_S3, (ch >> 3) & 1);
  delayMicroseconds(2);
  const int SAMPLES = 3;
  long sum = 0;
  for (int i = 0; i < SAMPLES; ++i) sum += analogRead(MUX_SIG);
  return (int)(sum / SAMPLES);
}

int readLaneDensityReal(int laneIdx) {
  int active = 0;
  for (int s = 0; s < 2; ++s) {
    int ch = laneIR[laneIdx][s];
    int v = readMuxChannel(ch);
    if (v < IR_THRESHOLD) active++;
  }
  if (active > 0) lastAnyDetectionMillis = millis();
  return active; // 0,1,2
}

int readLaneDensity(int laneIdx) {
  // always use real sensors (simulation removed)
  return readLaneDensityReal(laneIdx);
}

// ---------- LED Helpers ----------
void setLaneLEDs(int laneIdx, bool R, bool Y, bool G) {
  digitalWrite(lanePins[laneIdx][0], R ? HIGH : LOW);
  digitalWrite(lanePins[laneIdx][1], Y ? HIGH : LOW);
  digitalWrite(lanePins[laneIdx][2], G ? HIGH : LOW);
}
void setAllRed() {
  for (int i = 0; i < 4; ++i) setLaneLEDs(i, true, false, false);
}

void waitWithServer(unsigned long ms) {
  unsigned long start = millis();
  while (millis() - start < ms) {
    server.handleClient();
    delay(5);
  }
}

// solid green (responsive)
void doSolidGreen(int laneIdx, unsigned long durMs) {
  setAllRed();
  currentGreenLane = laneIdx;
  currentGreenBlink = false;
  digitalWrite(lanePins[laneIdx][0], LOW);  // red off
  digitalWrite(lanePins[laneIdx][2], HIGH); // green on
  unsigned long start = millis();
  while (millis() - start < durMs) {
    server.handleClient();
    delay(10);
  }
  digitalWrite(lanePins[laneIdx][2], LOW);
}

// blinking green (priority)
void doBlinkingGreen(int laneIdx, unsigned long durMs) {
  setAllRed();
  currentGreenLane = laneIdx;
  currentGreenBlink = true;
  digitalWrite(lanePins[laneIdx][0], LOW); // red off
  unsigned long endT = millis() + durMs;
  while (millis() < endT) {
    digitalWrite(lanePins[laneIdx][2], HIGH);
    unsigned long t0 = millis();
    while (millis() - t0 < BLINK_MS && millis() < endT) { server.handleClient(); delay(10); }
    digitalWrite(lanePins[laneIdx][2], LOW);
    t0 = millis();
    while (millis() - t0 < BLINK_MS && millis() < endT) { server.handleClient(); delay(10); }
  }
  digitalWrite(lanePins[laneIdx][2], LOW);
}

// yellow then red
void doYellowThenRed(int laneIdx) {
  digitalWrite(lanePins[laneIdx][2], LOW);  // green off
  digitalWrite(lanePins[laneIdx][1], HIGH); // yellow on
  waitWithServer(YELLOW_MS);
  digitalWrite(lanePins[laneIdx][1], LOW);
  digitalWrite(lanePins[laneIdx][0], HIGH); // red on
  // clear current green after finishing the lane's sequence
  currentGreenLane = -1;
  currentGreenBlink = false;
}

// emergency blink Y+G until cleared
void doEmergencyBlinkYandG(int laneIdx) {
  setAllRed();
  currentGreenLane = laneIdx;
  currentGreenBlink = true;
  digitalWrite(lanePins[laneIdx][0], LOW); // red off
  while (emergencyLane == laneIdx) {
    digitalWrite(lanePins[laneIdx][1], HIGH);
    digitalWrite(lanePins[laneIdx][2], HIGH);
    unsigned long t0 = millis();
    while (millis() - t0 < 500 && emergencyLane == laneIdx) { server.handleClient(); delay(10); }
    digitalWrite(lanePins[laneIdx][1], LOW);
    digitalWrite(lanePins[laneIdx][2], LOW);
    t0 = millis();
    while (millis() - t0 < 500 && emergencyLane == laneIdx) { server.handleClient(); delay(10); }
  }
  digitalWrite(lanePins[laneIdx][1], LOW);
  digitalWrite(lanePins[laneIdx][2], LOW);
  currentGreenLane = -1;
  currentGreenBlink = false;
}

// pedestrian all-red: blink vehicle REDs
void doPedestrianAllRed() {
  unsigned long endT = millis() + PED_ALLRED_MS;
  currentGreenLane = -1;
  currentGreenBlink = false;
  while (millis() < endT) {
    // ON
    for (int i = 0; i < 4; ++i) digitalWrite(lanePins[i][0], HIGH);
    unsigned long t0 = millis();
    while (millis() - t0 < 300 && millis() < endT) { server.handleClient(); delay(10); }
    // OFF
    for (int i = 0; i < 4; ++i) digitalWrite(lanePins[i][0], LOW);
    t0 = millis();
    while (millis() - t0 < 300 && millis() < endT) { server.handleClient(); delay(10); }
  }
  setAllRed();
  // done
}

// night mode: blink all yellow until nightMode cleared
void doNightModeBlink() {
  while (nightMode) {
    for (int i = 0; i < 4; ++i) {
      digitalWrite(lanePins[i][0], LOW);  // red off
      digitalWrite(lanePins[i][1], HIGH); // yellow on
      digitalWrite(lanePins[i][2], LOW);
    }
    unsigned long t0 = millis();
    while (millis() - t0 < 500 && nightMode) { server.handleClient(); delay(10); }
    for (int i = 0; i < 4; ++i) digitalWrite(lanePins[i][1], LOW);
    t0 = millis();
    while (millis() - t0 < 500 && nightMode) { server.handleClient(); delay(10); }
  }
  setAllRed();
}

// ---------- WEB UI (embedded) ----------
String makeDashboardHTML() {
  String html = R"rawliteral(
<!doctype html>
<html>
<head>
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>Smart Traffic Controller</title>
  <style>
    @import url('https://fonts.googleapis.com/css2?family=Inter:wght@400;600;700&display=swap');
    body{font-family:Inter,Arial,Helvetica,sans-serif;background:#081023;color:#e6eef8;margin:0;padding:12px}
    .wrap{max-width:980px;margin:12px auto;padding:18px;background:linear-gradient(180deg,#071029,#08203a);border-radius:12px;box-shadow:0 10px 30px rgba(0,0,0,0.5)}
    h1{margin:0 0 12px;text-align:center;font-weight:700}
    .topbar{display:flex;justify-content:space-between;align-items:center;gap:8px;margin-bottom:12px;flex-wrap:wrap}
    .controls{display:flex;gap:8px}
    .btn{padding:10px 14px;border-radius:8px;border:none;cursor:pointer;font-weight:600}
    .btn-ped{background:#7c3aed;color:white}
    .btn-clear{background:#64748b;color:white}
    .btn-emer{background:#ff5c8a;color:white}
    .switch{display:inline-flex;align-items:center;gap:8px}
    .grid{display:flex;flex-wrap:wrap;justify-content:space-around}
    .card{width:220px;margin:10px;padding:12px;border-radius:10px;background:rgba(255,255,255,0.03);text-align:center;border:1px solid rgba(255,255,255,0.03)}
    .dot{width:48px;height:48px;border-radius:50%;margin:10px auto;box-shadow:0 8px 20px rgba(0,0,0,0.5);transition:all .3s}
    .dot.red{background:#ff4d4f; box-shadow:0 0 18px #ff4d4f}
    .dot.green{background:#36d399; box-shadow:0 0 18px #36d399}
    .dot.yellow{background:#ffd166; box-shadow:0 0 18px #ffd166}
    .dot.off{background:#374151}
    .legend{display:flex;justify-content:center;gap:12px;margin-top:8px;color:#94a3b8;font-size:13px}
    .footer{margin-top:12px;text-align:center;color:#9fb0c8;font-size:13px}
    .status{font-size:14px;color:#cbd5e1}
    @media(max-width:600px){ .card{width:46%} }
  </style>
</head>
<body>
  <div class="wrap">
    <h1>&#128678; Smart Traffic Controller</h1>
    <div class="topbar">
      <div class="status" id="statusBar">Connecting...</div>
      <div class="controls">
        <button class="btn btn-ped" onclick="ped()">&#128694; All-Red Ped</button>
        <button class="btn btn-clear" onclick="clearEmergency()">&#10060; Clear Emergency</button>
        <label class="switch"><input type="checkbox" id="night" onchange="toggleNight()"><span style="margin-left:6px">Night Mode</span></label>
      </div>
    </div>

    <div id="grid" class="grid"></div>

    <div style="text-align:center;margin-top:12px">
      <div id="emControls"></div>
    </div>

    <div class="legend">
      <div>• <span style="color:#36d399">GREEN</span></div>
      <div>• <span style="color:#ffd166">YELLOW</span></div>
      <div>• <span style="color:#ff4d4f">RED</span></div>
    </div>

    <div class="footer">Connect to Wi-Fi: <b>TrafficLightESP</b> (pass: 12345678). Status updates every second.</div>
  </div>

<script>
async function fetchStatus(){
  try{
    const res = await fetch('/status');
    const j = await res.json();
    render(j);
  } catch(e) {
    console.error(e);
    document.getElementById('statusBar').textContent = 'Cannot reach device';
  }
}
function render(j){
  document.getElementById('statusBar').textContent = 'IP: ' + j.ip + '  |  Mode: ' + (j.nightMode? 'Night' : (j.emergency>=0 ? 'Emergency L'+(j.emergency+1) : 'Auto'));
  const grid = document.getElementById('grid'); grid.innerHTML = '';
  for(let i=0;i<4;i++){
    const card = document.createElement('div'); card.className='card';
    card.innerHTML = '<h3>Lane '+(i+1)+'</h3>';
    const dot = document.createElement('div');
    let cls = 'dot off';
    if (j.emergency === i) cls = 'dot yellow';
    else if (j.currentGreen === i) cls = (j.currentGreenBlink? 'dot yellow' : 'dot green');
    else cls = (j.skipped[i]? 'dot off' : 'dot red');
    dot.className = cls; card.appendChild(dot);
    const info = document.createElement('div');
    info.innerHTML = 'Density: <b>'+j.density[i]+'</b><br>Status: '+(j.emergency===i?'EMERGENCY': (j.currentGreen===i? (j.currentGreenBlink? 'PRIORITY BLINK':'GREEN') : 'RED'));
    card.appendChild(info);
    grid.appendChild(card);
  }
  // emergency controls
  const em = document.getElementById('emControls'); em.innerHTML = '';
  if (j.emergency === -1) {
    for(let i=0;i<4;i++){
      const b = document.createElement('button'); b.className='btn btn-emer';
      b.innerHTML='&#128657; Emergency L'+(i+1);
      b.onclick = (function(x){ return function(){ fetch('/e'+x).then(()=>fetchStatus()); }; })(i);
      em.appendChild(b);
    }
  } else {
    const p = document.createElement('div'); p.style.color='#ffd1d1'; p.textContent = 'Emergency active on Lane '+(j.emergency+1);
    em.appendChild(p);
  }
  document.getElementById('night').checked = j.nightMode;
}
function ped(){ fetch('/ped').then(()=>fetchStatus()); }
function clearEmergency(){ fetch('/clear').then(()=>fetchStatus()); }
function toggleNight(){ let cb=document.getElementById('night'); fetch('/night?val='+(cb.checked?1:0)).then(()=>fetchStatus()); }
setInterval(fetchStatus,1000); fetchStatus();
</script>
</body>
</html>
)rawliteral";
  return html;
}

void handleRoot() {
  server.send(200, "text/html", makeDashboardHTML());
}

void handleStatus() {
  int dens[4];
  for (int i = 0; i < 4; ++i) dens[i] = readLaneDensity(i);
  String out = "{";
  out += "\"density\":[" + String(dens[0]) + "," + String(dens[1]) + "," + String(dens[2]) + "," + String(dens[3]) + "],";
  out += "\"emergency\":" + String(emergencyLane) + ",";
  out += "\"currentGreen\":" + String(currentGreenLane) + ",";
  out += "\"currentGreenBlink\":" + String(currentGreenBlink?1:0) + ",";
  out += "\"nightMode\":" + String(nightMode?1:0) + ",";
  out += "\"ip\":\"" + WiFi.softAPIP().toString() + "\",";
  out += "\"skipped\":[" + String(dens[0]==0?1:0) + "," + String(dens[1]==0?1:0) + "," + String(dens[2]==0?1:0) + "," + String(dens[3]==0?1:0) + "]";
  out += "}";
  server.send(200, "application/json", out);
}

void handlePed() {
  pedAllRedRequested = true;
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleE0(){ emergencyLane = 0; autoMode = false; server.sendHeader("Location","/"); server.send(303); }
void handleE1(){ emergencyLane = 1; autoMode = false; server.sendHeader("Location","/"); server.send(303); }
void handleE2(){ emergencyLane = 2; autoMode = false; server.sendHeader("Location","/"); server.send(303); }
void handleE3(){ emergencyLane = 3; autoMode = false; server.sendHeader("Location","/"); server.send(303); }

void handleClear() {
  emergencyLane = -1;
  autoMode = true;
  server.sendHeader("Location","/"); server.send(303);
}

void handleNight() {
  String val = server.arg("val");
  nightMode = (val == "1");
  server.sendHeader("Location","/"); server.send(303);
}

// ---------- SETUP ----------
void setup() {
  if (DEBUG_SERIAL) {
    Serial.begin(115200);
    Serial.println("Starting Smart Traffic Controller...");
  }
  // LEDs
  for (int i = 0; i < 4; ++i) for (int j = 0; j < 3; ++j) { pinMode(lanePins[i][j], OUTPUT); digitalWrite(lanePins[i][j], LOW); }
  setAllRed();

  // MUX
  pinMode(MUX_S0, OUTPUT); pinMode(MUX_S1, OUTPUT); pinMode(MUX_S2, OUTPUT); pinMode(MUX_S3, OUTPUT);
  pinMode(MUX_SIG, INPUT);

  // WiFi AP + server
  WiFi.softAP(AP_SSID, AP_PASS);
  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/ped", handlePed);
  server.on("/e0", handleE0); server.on("/e1", handleE1); server.on("/e2", handleE2); server.on("/e3", handleE3);
  server.on("/clear", handleClear);
  server.on("/night", handleNight);
  server.begin();

  if (DEBUG_SERIAL) {
    Serial.print("AP started: "); Serial.println(AP_SSID);
    Serial.print("IP: "); Serial.println(WiFi.softAPIP());
  }
  lastAnyDetectionMillis = millis();
}

// ---------- MAIN LOOP ----------
void loop() {
  server.handleClient();

  // Night mode highest priority: blink until toggled off
  if (nightMode) {
    doNightModeBlink();
    // after nightMode disabled, continue normal operation
  }

  // Check round-robin fallback (5s no detection)
  if (millis() - lastAnyDetectionMillis > NO_IR_ROUNDROBIN_MS) roundRobinOnly = true;
  else roundRobinOnly = false;

  // Pre-scan densities (snapshot)
  int prescan[4];
  for (int i = 0; i < 4; ++i) {
    prescan[i] = readLaneDensity(i);
    waitWithServer(15); // small pause while keeping server alive
  }

  // Candidate for priority: highest density (first tie)
  int candidate = -1;
  int maxD = -1;
  for (int i = 0; i < 4; ++i) {
    if (prescan[i] > maxD) { maxD = prescan[i]; candidate = i; }
  }
  if (roundRobinOnly) candidate = -1; // disable priority during round-robin fallback

  // Iterate lanes in order
  for (int lane = 0; lane < 4; ++lane) {
    server.handleClient();

    // Emergency override: jump to emergency lane immediately
    if (emergencyLane >= 0 && lane != emergencyLane) {
      lane = emergencyLane; // jump
    }

    // fresh density read before turning
    int density = readLaneDensity(lane);
    waitWithServer(10);

    // If emergency active and this is emergency lane -> do emergency blink until cleared
    if (emergencyLane >= 0 && lane == emergencyLane) {
      doEmergencyBlinkYandG(lane);
      doYellowThenRed(lane);
      lastPriorityLane = -1; consecutivePriorityCount = 0;
      continue;
    }

    // ROUND-ROBIN MODE: do not skip lanes; just give normal green to each lane
    if (roundRobinOnly) {
      doSolidGreen(lane, GREEN_LOW_MS);
      doYellowThenRed(lane);
      lastPriorityLane = -1; consecutivePriorityCount = 0;
      // pedestrian all-red if requested
      if (pedAllRedRequested) {
        doPedestrianAllRed();
        pedAllRedRequested = false;
      }
      waitWithServer(20);
      continue;
    }

    // NORMAL MODE: skip empty lane
    if (density == 0) {
      // reset priority counters on skip
      lastPriorityLane = -1;
      consecutivePriorityCount = 0;
      continue; // next lane
    }

    // decide priority
    bool wantsPriority = (density == 2) && (candidate == lane);
    bool allowPriority = wantsPriority;
    if (wantsPriority && lastPriorityLane == lane && consecutivePriorityCount >= MAX_CONSECUTIVE_PRIORITY) {
      allowPriority = false; // fairness
    }

    if (wantsPriority && allowPriority) {
      doBlinkingGreen(lane, GREEN_HIGH_MS);
      if (lastPriorityLane == lane) consecutivePriorityCount++;
      else { lastPriorityLane = lane; consecutivePriorityCount = 1; }
    } else {
      doSolidGreen(lane, GREEN_LOW_MS);
      lastPriorityLane = -1; consecutivePriorityCount = 0;
    }

    // yellow then red
    doYellowThenRed(lane);

    // pedestrian all-red if requested
    if (pedAllRedRequested) {
      doPedestrianAllRed();
      pedAllRedRequested = false;
    }

    waitWithServer(20);
  }

  // keep server responsive a bit
  unsigned long t0 = millis();
  while (millis() - t0 < 200) { server.handleClient(); delay(10); }
}
