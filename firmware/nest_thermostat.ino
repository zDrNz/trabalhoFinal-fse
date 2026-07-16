/*
 * Nest Learning Thermostat — Réplica acadêmica com ESP32
 * Versão local standalone (sem Wi-Fi/MQTT)
 *
 * Componentes:
 *   - ESP32 DevKit C
 *   - DHT11 (temperatura/umidade)
 *   - OLED SSD1306 128x64 (I2C)
 *   - Encoder rotativo KY-040 com clique
 *   - PIR HC-SR501 (presença)
 *   - Módulo relé 2 canais (ativo em nível BAIXO)
 *
 * Controle: histerese +/- 0.5 C, tempo mínimo de ciclo 5 min,
 * modo Auto-Away após 30 min sem detecção do PIR.
 */

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#include <WiFi.h>
#include <WebServer.h>
#include <PubSubClient.h>

// ---------- Pinos ----------
#define PIN_DHT       4
#define PIN_ENC_CLK   32
#define PIN_ENC_DT    33
#define PIN_ENC_SW    25
#define PIN_PIR       27
#define PIN_RELAY_HEAT 26   // canal 1 - aquecimento (W)
#define PIN_RELAY_COOL 14   // canal 2 - resfriamento (Y)

// I2C padrão do ESP32: SDA=21, SCL=22

// ---------- OLED ----------
#define OLED_W 128
#define OLED_H 64
#define OLED_ADDR 0x3C
Adafruit_SSD1306 display(OLED_W, OLED_H, &Wire, -1);

// ---------- DHT ----------
#define DHT_TYPE DHT11
DHT dht(PIN_DHT, DHT_TYPE);

// ---------- Relé ativo em nível baixo ----------
#define RELAY_ON  LOW
#define RELAY_OFF HIGH

// ---------- Rede: PREENCHER ----------
const char* WIFI_SSID = "Familia";
const char* WIFI_PASS = "barrosgodoy";

const char* MQTT_HOST = "192.168.1.232";   // IP do broker (Mosquitto/HiveMQ)
const int   MQTT_PORT = 1883;
const char* MQTT_USER = "";                // vazio se broker sem auth
const char* MQTT_PASS = "";
const char* MQTT_TOPIC_STATE = "nest/state";
const char* MQTT_TOPIC_CMD   = "nest/cmd";

WebServer   server(80);
WiFiClient  espClient;
PubSubClient mqtt(espClient);

unsigned long lastMqttAttempt = 0;
unsigned long lastPublish     = 0;

// ---------- Parâmetros de controle ----------
const float HYSTERESIS   = 0.5;              // C
const unsigned long MIN_CYCLE_MS = 10UL * 1000UL;  // 10 s (teste; produção: 5 min = 5UL*60UL*1000UL)
const unsigned long AWAY_TIMEOUT_MS = 30UL * 1000UL; // 30 s (produção: 30 min = 30UL*60UL*1000UL)
const unsigned long DHT_INTERVAL_MS = 3000;  // leitura DHT11 (>=2s)
const unsigned long DISPLAY_WAKE_MS = 15000; // tela acesa após interação/presença

// ---------- Modos ----------
enum Mode { MODE_OFF, MODE_HEAT, MODE_COOL, MODE_AUTO };
const char* modeName[] = {"OFF", "HEAT", "COOL", "AUTO"};

// ---------- Estado ----------
volatile int encoderDelta = 0;     // atualizado na ISR
volatile uint8_t encState = 0;     // estado quadratura anterior
volatile int8_t  encAccum = 0;     // acumula sub-passos até 1 detent

float targetTemp = 22.0;
float currentTemp = NAN;
float currentHum  = NAN;
Mode  mode = MODE_AUTO;

bool  heating = false;
bool  cooling = false;
bool  present = false;
bool  away    = false;
bool  editingMode = false;   // clique alterna entre editar setpoint / editar modo

unsigned long lastDhtRead   = 0;
unsigned long lastHeatSwitch = 0;
unsigned long lastCoolSwitch = 0;
unsigned long lastPresence   = 0;
unsigned long lastInteraction = 0;

// ---------- ISR do encoder (decoder por tabela de estados) ----------
// Tabela de transições quadratura. Cada índice = (estado_ant<<2)|estado_novo.
// Valor: -1/+1 = passo válido, 0 = transição intermediária/bounce (ignora).
static const int8_t QUAD_TABLE[16] = {
  0, -1,  1,  0,
  1,  0,  0, -1,
 -1,  0,  0,  1,
  0,  1, -1,  0
};

void IRAM_ATTR encoderISR() {
  uint8_t s = (digitalRead(PIN_ENC_CLK) << 1) | digitalRead(PIN_ENC_DT);
  encState = ((encState << 2) | s) & 0x0F;
  int8_t step = QUAD_TABLE[encState];
  if (step) {
    encAccum += step;
    // KY-040: 4 sub-passos por detent. Conta 1 só no ciclo completo.
    if (encAccum >= 4)      { encoderDelta++; encAccum = 0; }
    else if (encAccum <= -4){ encoderDelta--; encAccum = 0; }
  }
}

// ---------- Botão com debounce ----------
bool readClick() {
  static unsigned long lastPress = 0;
  static int lastState = HIGH;
  int s = digitalRead(PIN_ENC_SW);
  if (s == LOW && lastState == HIGH && millis() - lastPress > 250) {
    lastPress = millis();
    lastState = s;
    return true;
  }
  if (s == HIGH) lastState = HIGH;
  return false;
}

// ---------- Página web (self-contained) ----------
const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html><html lang="pt-br"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Nest ESP32</title>
<style>
 *{box-sizing:border-box;font-family:system-ui,sans-serif}
 body{margin:0;background:#111;color:#eee;text-align:center}
 .wrap{max-width:420px;margin:0 auto;padding:20px}
 h1{font-size:1.1rem;color:#aaa;font-weight:500}
 .temp{font-size:4rem;font-weight:700;margin:10px 0}
 .temp small{font-size:1.4rem;color:#888}
 .row{display:flex;gap:10px;justify-content:center;margin:14px 0}
 .card{background:#1c1c1e;border-radius:12px;padding:12px;flex:1}
 .card b{display:block;font-size:1.4rem}
 .card span{font-size:.75rem;color:#888}
 button{border:0;border-radius:12px;padding:16px;font-size:1.1rem;
   background:#2c2c2e;color:#eee;cursor:pointer;flex:1}
 button:active{background:#3a3a3c}
 .set{font-size:2rem;padding:20px}
 .modes button.on{background:#0a84ff;color:#fff}
 .status{margin-top:14px;font-size:.85rem;color:#888}
 .badge{display:inline-block;padding:4px 10px;border-radius:20px;margin:4px}
 .heat{background:#ff453a33;color:#ff453a}
 .cool{background:#0a84ff33;color:#0a84ff}
 .away{background:#ffd60a33;color:#ffd60a}
</style></head><body><div class="wrap">
<h1>Nest Thermostat — ESP32</h1>
<div class="temp"><span id="temp">--</span><small>°C</small></div>
<div class="row">
  <div class="card"><b id="target">--</b><span>Alvo °C</span></div>
  <div class="card"><b id="hum">--</b><span>Umidade %</span></div>
  <div class="card"><b id="mode">--</b><span>Modo</span></div>
</div>
<div class="row">
  <button class="set" onclick="cmd('temp_down')">−</button>
  <button class="set" onclick="cmd('temp_up')">+</button>
</div>
<div class="row modes">
  <button id="m_OFF"  onclick="cmd('mode_off')">OFF</button>
  <button id="m_HEAT" onclick="cmd('mode_heat')">HEAT</button>
  <button id="m_COOL" onclick="cmd('mode_cool')">COOL</button>
  <button id="m_AUTO" onclick="cmd('mode_auto')">AUTO</button>
</div>
<div class="status" id="status"></div>
</div><script>
async function cmd(a){await fetch('/api/cmd?a='+a);refresh()}
async function refresh(){
 try{let r=await fetch('/api/state');let s=await r.json();
  document.getElementById('temp').textContent=s.temp==null?'--':s.temp.toFixed(1);
  document.getElementById('target').textContent=s.target.toFixed(1);
  document.getElementById('hum').textContent=s.hum==null?'--':s.hum;
  document.getElementById('mode').textContent=s.mode;
  ['OFF','HEAT','COOL','AUTO'].forEach(m=>{
   document.getElementById('m_'+m).className=(s.mode==m?'on':'')});
  let st='';
  if(s.heat)st+='<span class="badge heat">Aquecendo</span>';
  if(s.cool)st+='<span class="badge cool">Resfriando</span>';
  if(!s.heat&&!s.cool)st+='<span class="badge">Idle</span>';
  if(s.away)st+='<span class="badge away">Auto-Away</span>';
  else st+='<span class="badge">'+(s.present?'Presenca':'Sem movimento')+' &bull; Away em '+s.away_in+'s</span>';
  document.getElementById('status').innerHTML=st;
 }catch(e){}
}
refresh();setInterval(refresh,2000);
</script></body></html>
)HTML";

// ---------- Helpers de comando (encoder, web e MQTT compartilham) ----------
void clampTarget() {
  if (targetTemp < 10) targetTemp = 10;
  if (targetTemp > 32) targetTemp = 32;
}
void cmdTempDelta(float d) { targetTemp += d; clampTarget(); lastInteraction = millis(); }
void cmdModeCycle(int dir) {
  int m = (int)mode + (dir > 0 ? 1 : -1);
  if (m < 0) m = 3;
  if (m > 3) m = 0;
  mode = (Mode)m;
  lastInteraction = millis();
}
void cmdSetMode(Mode m) { mode = m; lastInteraction = millis(); }

void applyCommand(const String& c) {
  if      (c == "temp_up")   cmdTempDelta(0.5);
  else if (c == "temp_down") cmdTempDelta(-0.5);
  else if (c == "mode_off")  cmdSetMode(MODE_OFF);
  else if (c == "mode_heat") cmdSetMode(MODE_HEAT);
  else if (c == "mode_cool") cmdSetMode(MODE_COOL);
  else if (c == "mode_auto") cmdSetMode(MODE_AUTO);
  else if (c == "mode_cycle")cmdModeCycle(1);
  else if (c.startsWith("set:")) { targetTemp = c.substring(4).toFloat(); clampTarget(); lastInteraction = millis(); }
}

String buildStateJson() {
  String s = "{";
  s += "\"temp\":"    + (isnan(currentTemp) ? String("null") : String(currentTemp, 1)) + ",";
  s += "\"hum\":"     + (isnan(currentHum)  ? String("null") : String(currentHum, 0))  + ",";
  s += "\"target\":"  + String(targetTemp, 1) + ",";
  s += "\"mode\":\""  + String(modeName[mode]) + "\",";
  s += "\"heat\":"    + String(heating ? "true" : "false") + ",";
  s += "\"cool\":"    + String(cooling ? "true" : "false") + ",";
  s += "\"present\":" + String(present ? "true" : "false") + ",";
  s += "\"away\":"    + String(away    ? "true" : "false") + ",";
  long awayIn = (long)AWAY_TIMEOUT_MS - (long)(millis() - lastPresence);
  if (awayIn < 0) awayIn = 0;
  s += "\"away_in\":" + String(awayIn / 1000);
  s += "}";
  return s;
}

// ---------- Handlers web ----------
void handleRoot()  { server.send_P(200, "text/html", INDEX_HTML); }
void handleState() { server.send(200, "application/json", buildStateJson()); }
void handleCmd()   {
  if (server.hasArg("a")) applyCommand(server.arg("a"));
  server.send(200, "application/json", buildStateJson());
}

// ---------- MQTT ----------
void mqttCallback(char* topic, byte* payload, unsigned int len) {
  String c;
  for (unsigned int i = 0; i < len; i++) c += (char)payload[i];
  applyCommand(c);
}
void mqttReconnect() {
  if (mqtt.connected() || WiFi.status() != WL_CONNECTED) return;
  if (millis() - lastMqttAttempt < 5000) return;
  lastMqttAttempt = millis();
  String id = "nest-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  if (mqtt.connect(id.c_str(), MQTT_USER, MQTT_PASS)) {
    mqtt.subscribe(MQTT_TOPIC_CMD);
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(PIN_ENC_CLK, INPUT_PULLUP);
  pinMode(PIN_ENC_DT,  INPUT_PULLUP);
  pinMode(PIN_ENC_SW,  INPUT_PULLUP);
  pinMode(PIN_PIR,     INPUT);

  pinMode(PIN_RELAY_HEAT, OUTPUT);
  pinMode(PIN_RELAY_COOL, OUTPUT);
  digitalWrite(PIN_RELAY_HEAT, RELAY_OFF);
  digitalWrite(PIN_RELAY_COOL, RELAY_OFF);

  encState = (digitalRead(PIN_ENC_CLK) << 1) | digitalRead(PIN_ENC_DT);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_CLK), encoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_DT),  encoderISR, CHANGE);

  dht.begin();

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println(F("SSD1306 nao encontrado"));
    for (;;);
  }
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 24);
  display.println(F("  Nest ESP32"));
  display.display();
  delay(1500);

  // --- Wi-Fi ---
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  display.clearDisplay();
  display.setCursor(0, 20);
  display.println(F("Conectando WiFi..."));
  display.display();
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(250);

  display.clearDisplay();
  display.setCursor(0, 16);
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F("IP: ")); Serial.println(WiFi.localIP());
    display.println(F("WiFi OK. Acesse:"));
    display.println(WiFi.localIP().toString());
  } else {
    display.println(F("WiFi FALHOU"));
    display.println(F("(so controle local)"));
  }
  display.display();
  delay(2000);

  // --- Servidor web ---
  server.on("/", handleRoot);
  server.on("/api/state", handleState);
  server.on("/api/cmd", handleCmd);
  server.begin();

  // --- MQTT ---
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);

  lastInteraction = millis();
}

void loop() {
  unsigned long now = millis();

  // --- Encoder: ajusta setpoint ou modo ---
  int delta = 0;
  noInterrupts();
  delta = encoderDelta;
  encoderDelta = 0;
  interrupts();

  if (delta != 0) {
    if (editingMode) cmdModeCycle(delta);
    else             cmdTempDelta(delta * 0.5);
  }

  // --- Clique: alterna edição setpoint <-> modo ---
  if (readClick()) {
    editingMode = !editingMode;
    lastInteraction = now;
  }

  // --- PIR: presença ---
  int pirRaw = digitalRead(PIN_PIR);
  if (pirRaw == HIGH) {
    present = true;
    lastPresence = now;
    lastInteraction = now;   // acorda display na aproximação
  } else {
    present = false;
  }
  away = (now - lastPresence > AWAY_TIMEOUT_MS);

  // --- Debug PIR: imprime na borda + contagem regressiva do Away ---
  static int lastPirRaw = -1;
  static unsigned long lastPirLog = 0;
  if (pirRaw != lastPirRaw) {
    Serial.print(F("[PIR] borda -> "));
    Serial.println(pirRaw == HIGH ? F("HIGH (movimento)") : F("LOW (parado)"));
    lastPirRaw = pirRaw;
  }
  if (now - lastPirLog > 1000) {   // 1x por segundo
    lastPirLog = now;
    long restante = (long)AWAY_TIMEOUT_MS - (long)(now - lastPresence);
    if (restante < 0) restante = 0;
    Serial.print(F("[PIR] raw=")); Serial.print(pirRaw);
    Serial.print(F(" present=")); Serial.print(present);
    Serial.print(F(" away=")); Serial.print(away);
    Serial.print(F(" Away em ")); Serial.print(restante / 1000);
    Serial.println(F("s"));
  }

  // --- Leitura DHT11 ---
  if (now - lastDhtRead > DHT_INTERVAL_MS) {
    lastDhtRead = now;
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t)) currentTemp = t;
    if (!isnan(h)) currentHum  = h;
  }

  controlHVAC(now);
  updateDisplay(now);

  // --- Rede ---
  server.handleClient();
  mqttReconnect();
  mqtt.loop();
  if (mqtt.connected() && now - lastPublish > 3000) {
    lastPublish = now;
    String s = buildStateJson();
    mqtt.publish(MQTT_TOPIC_STATE, s.c_str(), true);
  }
}

// ---------- Lógica de controle com histerese + tempo mínimo de ciclo ----------
void controlHVAC(unsigned long now) {
  if (isnan(currentTemp)) return;

  bool wantHeat = false, wantCool = false;

  // Só controla se houver presença e modo != OFF.
  // Sem presença (Auto-Away) => tudo desligado.
  if (!away && mode != MODE_OFF) {
    float sp = targetTemp;
    if (mode == MODE_HEAT || mode == MODE_AUTO) {
      if (currentTemp < sp - HYSTERESIS) wantHeat = true;
      else if (currentTemp > sp + HYSTERESIS) wantHeat = false;
      else wantHeat = heating;   // dentro da banda: mantém estado
    }
    if (mode == MODE_COOL || mode == MODE_AUTO) {
      if (currentTemp > sp + HYSTERESIS) wantCool = true;
      else if (currentTemp < sp - HYSTERESIS) wantCool = false;
      else wantCool = cooling;
    }
    if (wantHeat && wantCool) wantCool = false;  // nunca os dois juntos
  }

  // Desligar é imediato (proteção de ciclo vale só pra LIGAR)
  if (!wantHeat && heating) {
    heating = false; lastHeatSwitch = now;
    digitalWrite(PIN_RELAY_HEAT, RELAY_OFF);
  }
  if (!wantCool && cooling) {
    cooling = false; lastCoolSwitch = now;
    digitalWrite(PIN_RELAY_COOL, RELAY_OFF);
  }
  // Ligar respeita tempo mínimo de ciclo
  if (wantHeat && !heating && now - lastHeatSwitch > MIN_CYCLE_MS) {
    heating = true; lastHeatSwitch = now;
    digitalWrite(PIN_RELAY_HEAT, RELAY_ON);
  }
  if (wantCool && !cooling && now - lastCoolSwitch > MIN_CYCLE_MS) {
    cooling = true; lastCoolSwitch = now;
    digitalWrite(PIN_RELAY_COOL, RELAY_ON);
  }
}

// ---------- Interface OLED ----------
void updateDisplay(unsigned long now) {
  // Proximity wake: apaga após inatividade se ausente
  bool screenOn = (now - lastInteraction < DISPLAY_WAKE_MS) || present;
  if (!screenOn) {
    display.clearDisplay();
    display.display();
    return;
  }

  display.clearDisplay();

  // Linha topo: modo + status
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print(F("Modo: "));
  display.print(modeName[mode]);
  if (editingMode) display.print(F(" *"));

  display.setCursor(96, 0);
  if (heating)      display.print(F("HEAT"));
  else if (cooling) display.print(F("COOL"));
  else              display.print(F("idle"));

  // Temperatura atual grande
  display.setTextSize(3);
  display.setCursor(4, 18);
  if (isnan(currentTemp)) display.print(F("--"));
  else                    display.print(currentTemp, 1);
  display.setTextSize(1);
  display.print(F("C"));

  // Setpoint + umidade
  display.setTextSize(1);
  display.setCursor(0, 48);
  display.print(F("Alvo:"));
  display.print(targetTemp, 1);
  display.print((char)247);   // grau
  if (!editingMode) display.print(F("*"));

  display.setCursor(72, 48);
  display.print(F("Um:"));
  if (isnan(currentHum)) display.print(F("--"));
  else                   display.print(currentHum, 0);
  display.print(F("%"));

  // Rodapé: presença / away com contagem regressiva
  display.setCursor(0, 57);
  if (away) {
    display.print(F("AUTO-AWAY (eco)"));
  } else {
    long awayIn = (long)AWAY_TIMEOUT_MS - (long)(now - lastPresence);
    if (awayIn < 0) awayIn = 0;
    display.print(present ? F("Presente") : F("Sem mov."));
    display.print(F(" Away:"));
    display.print(awayIn / 1000);
    display.print(F("s"));
  }

  display.display();
}
