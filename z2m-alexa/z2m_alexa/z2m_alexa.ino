/*
  Z2M → Alexa (ESP8266)
  Faz a Alexa enxergar dispositivos do Zigbee2MQTT: o ESP se apresenta à Alexa como uma ponte Philips Hue
  (biblioteca Espalexa) e conversa com o Zigbee2MQTT só pelo MQTT. Não toca no coordenador Zigbee, então
  roda em paralelo com o Z2M do Home Assistant sem interferir.

  Alexa  ──Hue (rede local)──▶  ESP  ──MQTT──▶  zigbee2mqtt/<nome>/set  ──▶  Z2M  ──▶  dispositivo Zigbee
  Alexa  ◀── estado ─────────  ESP  ◀──MQTT──  zigbee2mqtt/<nome>       ◀──  Z2M

  Dispositivos são cadastrados na página web do ESP (http://<ip>/) e guardados na flash (LittleFS).
*/
#define ESPALEXA_MAXDEVICES 20          // limite de dispositivos (memória do ESP8266)
#define ESPALEXA_NO_SUBPAGE             // a página /espalexa da biblioteca não é usada

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <LittleFS.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "Espalexa.h"            // cópia com ajustes (tomada Hue, gancho SSDP p/ WeMo): veja o cabeçalho do arquivo

// Wi-Fi e MQTT são configurados pela página do ESP (gravados na flash). config.h é opcional: só dá valores iniciais.
#if __has_include("config.h")
#include "config.h"
#endif
#ifndef WIFI_SSID
#define WIFI_SSID ""
#define WIFI_SENHA ""
#endif
#ifndef NOME_HOST
#define NOME_HOST "z2m-alexa"
#endif
#ifndef MQTT_HOST
#define MQTT_HOST ""
#define MQTT_PORTA 1883
#define MQTT_USUARIO ""
#define MQTT_SENHA ""
#endif
#ifndef Z2M_BASE
#define Z2M_BASE "zigbee2mqtt"
#endif
#ifndef WEB_USUARIO
#define WEB_USUARIO "admin"
#define WEB_SENHA ""
#endif
#define AP_SENHA "configurar"          // senha da rede de configuração z2m-alexa-XXXX

#define VERSAO "1.1.0"
#define ARQ_DISP "/disp.json"
#define ARQ_CFG "/cfg.json"
#define MAX_TXT 40

// ------------------------------------------------------------------ configuração (página /config)
struct Config {
  String ssid = WIFI_SSID, wsenha = WIFI_SENHA, host = NOME_HOST;
  String mqttHost = MQTT_HOST; uint16_t mqttPorta = MQTT_PORTA; String mqttUsuario = MQTT_USUARIO, mqttSenha = MQTT_SENHA;
  String base = Z2M_BASE, webUsuario = WEB_USUARIO, webSenha = WEB_SENHA;
  String tomadaModo = "hue";   // como as "tomadas" aparecem na Alexa: hue (tomada Hue) | wemo (Belkin) | luz (lâmpada, como até a 1.0.x)
} cfg;

// ------------------------------------------------------------------ dispositivos
enum Tipo : uint8_t { T_TOMADA = 0, T_LUZ = 1, T_DIMMER = 2 };
const char* TIPO_NOME[] = {"tomada", "luz", "dimmer"};

struct Disp {
  String nome;        // nome na Alexa
  String z2m;         // friendly_name no Zigbee2MQTT
  String prop;        // propriedade liga/desliga (state, state_l1, ...)
  Tipo tipo;
  bool ligado = false;
  uint8_t brilho = 254;
  bool conhecido = false;   // já recebeu estado do Z2M
  bool online = true;       // availability do Z2M
  EspalexaDevice* alexa = nullptr;
};

Disp disps[ESPALEXA_MAXDEVICES];
uint8_t nDisps = 0;

ESP8266WebServer server(80);
ESP8266HTTPUpdateServer atualizador;
WiFiClient wcli;
PubSubClient mqtt(wcli);
Espalexa espalexa;

String topicoStatus;
unsigned long proxMqtt = 0, proxWifi = 0;
bool alexaIniciada = false, wifiJaConectou = false, modoAP = false;
unsigned long inicioWifi = 0;
unsigned long caiuEm = 0;
uint32_t conexoesMqtt = 0;

// ------------------------------------------------------------------ utilidades
String esc(const String& s) {
  String o; o.reserve(s.length() + 8);
  for (char c : s) {
    if (c == '<') o += F("&lt;"); else if (c == '>') o += F("&gt;");
    else if (c == '&') o += F("&amp;"); else if (c == '"') o += F("&quot;");
    else o += c;
  }
  return o;
}

bool textoValido(const String& s, bool topico) {
  if (s.length() == 0 || s.length() > MAX_TXT) return false;
  for (char c : s) {
    if ((uint8_t)c < 32) return false;
    if (topico && (c == '#' || c == '+')) return false;
  }
  return true;
}

// ------------------------------------------------------------------ armazenamento
bool salvaDisps() {
  JsonDocument doc;
  JsonArray a = doc.to<JsonArray>();
  for (uint8_t i = 0; i < nDisps; i++) {
    JsonObject o = a.add<JsonObject>();
    o["n"] = disps[i].nome; o["z"] = disps[i].z2m; o["p"] = disps[i].prop; o["t"] = (uint8_t)disps[i].tipo;
  }
  File f = LittleFS.open(ARQ_DISP, "w");
  if (!f) return false;
  serializeJson(doc, f);
  f.close();
  return true;
}

bool salvaCfg() {
  JsonDocument doc;
  doc["ssid"] = cfg.ssid; doc["wsenha"] = cfg.wsenha; doc["host"] = cfg.host;
  doc["mh"] = cfg.mqttHost; doc["mp"] = cfg.mqttPorta; doc["mu"] = cfg.mqttUsuario; doc["ms"] = cfg.mqttSenha;
  doc["base"] = cfg.base; doc["wu"] = cfg.webUsuario; doc["ws"] = cfg.webSenha; doc["tm"] = cfg.tomadaModo;
  File f = LittleFS.open(ARQ_CFG, "w");
  if (!f) return false;
  serializeJson(doc, f); f.close();
  return true;
}

void carregaCfg() {
  File f = LittleFS.open(ARQ_CFG, "r");
  if (!f) return;                       // primeira vez: valores do config.h (ou vazios)
  JsonDocument doc;
  if (deserializeJson(doc, f)) { f.close(); return; }
  f.close();
  cfg.ssid = doc["ssid"] | cfg.ssid; cfg.wsenha = doc["wsenha"] | cfg.wsenha; cfg.host = doc["host"] | cfg.host;
  cfg.mqttHost = doc["mh"] | cfg.mqttHost; cfg.mqttPorta = doc["mp"] | cfg.mqttPorta;
  cfg.mqttUsuario = doc["mu"] | cfg.mqttUsuario; cfg.mqttSenha = doc["ms"] | cfg.mqttSenha;
  cfg.base = doc["base"] | cfg.base; cfg.webUsuario = doc["wu"] | cfg.webUsuario; cfg.webSenha = doc["ws"] | cfg.webSenha;
  cfg.tomadaModo = doc["tm"] | cfg.tomadaModo;
  if (cfg.tomadaModo != "hue" && cfg.tomadaModo != "wemo" && cfg.tomadaModo != "luz") cfg.tomadaModo = "hue";
}

void carregaDisps() {
  nDisps = 0;
  File f = LittleFS.open(ARQ_DISP, "r");
  if (!f) return;
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, f);
  f.close();
  if (e) { Serial.printf("disp.json ilegível: %s\n", e.c_str()); return; }
  for (JsonObject o : doc.as<JsonArray>()) {
    if (nDisps >= ESPALEXA_MAXDEVICES) break;
    Disp& d = disps[nDisps];
    d.nome = o["n"] | ""; d.z2m = o["z"] | ""; d.prop = o["p"] | "state";
    uint8_t t = o["t"] | 0; d.tipo = (Tipo)(t > 2 ? 0 : t);
    if (d.nome.length() && d.z2m.length()) nDisps++;
  }
}

// ------------------------------------------------------------------ MQTT → Z2M
String topicoDe(const Disp& d) { return cfg.base + "/" + d.z2m; }

void enviaZ2M(Disp& d, bool ligar, int brilho) {
  if (!mqtt.connected()) return;
  JsonDocument doc;
  doc[d.prop] = ligar ? "ON" : "OFF";
  if (ligar && brilho >= 0 && d.tipo == T_DIMMER) doc["brightness"] = brilho;
  String corpo; serializeJson(doc, corpo);
  String t = topicoDe(d) + "/set";
  mqtt.publish(t.c_str(), corpo.c_str());
  Serial.printf("→ %s %s\n", t.c_str(), corpo.c_str());
}

void pedeEstado(Disp& d) {
  JsonDocument doc; doc[d.prop] = "";
  String corpo; serializeJson(doc, corpo);
  String t = topicoDe(d) + "/get";
  mqtt.publish(t.c_str(), corpo.c_str());
}

// estado vindo do Z2M → mostra para a Alexa (sem gerar comando de volta)
void aplicaEstado(Disp& d, bool ligado, int brilho) {
  d.conhecido = true;
  d.ligado = ligado;
  if (brilho > 0) d.brilho = brilho > 254 ? 254 : brilho;
  if (!d.alexa) return;
  // setValue direto: o setState(true) da Espalexa fica em 0 se o aparelho nunca teve brilho
  if (!ligado) d.alexa->setValue(0);
  else if (d.tipo == T_DIMMER) d.alexa->setValue(d.brilho < 1 ? 1 : d.brilho);
  else d.alexa->setValue(255);
}

void recebeMqtt(char* topico, byte* payload, unsigned int len) {
  String t(topico);
  String base = cfg.base + "/";
  if (!t.startsWith(base)) return;
  String resto = t.substring(base.length());
  bool disponib = resto.endsWith("/availability");
  if (disponib) resto = resto.substring(0, resto.length() - 13);
  for (uint8_t i = 0; i < nDisps; i++) {
    Disp& d = disps[i];
    if (d.z2m != resto) continue;
    if (disponib) {
      // formato novo: {"state":"online"}; antigo: online
      String p; p.reserve(len); for (unsigned j = 0; j < len; j++) p += (char)payload[j];
      d.online = p.indexOf("offline") < 0;
      continue;
    }
    JsonDocument filtro; filtro[d.prop] = true; filtro["brightness"] = true;
    JsonDocument doc;
    if (deserializeJson(doc, payload, len, DeserializationOption::Filter(filtro))) continue;
    if (!doc[d.prop].is<const char*>()) continue;
    String st = doc[d.prop].as<String>(); st.toUpperCase();
    if (st != "ON" && st != "OFF") continue;
    aplicaEstado(d, st == "ON", doc["brightness"] | -1);
  }
}

void conectaMqtt() {
  if (mqtt.connected() || millis() < proxMqtt) return;
  proxMqtt = millis() + 5000;
  if (!cfg.mqttHost.length()) return;
  String id = cfg.host + "-" + String(ESP.getChipId(), HEX);
  Serial.printf("MQTT %s:%u…\n", cfg.mqttHost.c_str(), cfg.mqttPorta);
  bool ok = cfg.mqttUsuario.length()
    ? mqtt.connect(id.c_str(), cfg.mqttUsuario.c_str(), cfg.mqttSenha.c_str(), topicoStatus.c_str(), 1, true, "offline")
    : mqtt.connect(id.c_str(), topicoStatus.c_str(), 1, true, "offline");
  if (!ok) {
    Serial.printf("MQTT falhou (rc=%d)\n", mqtt.state());
    return;
  }
  conexoesMqtt++;
  mqtt.publish(topicoStatus.c_str(), "online", true);
  for (uint8_t i = 0; i < nDisps; i++) {
    String t = topicoDe(disps[i]);
    mqtt.subscribe(t.c_str());
    mqtt.subscribe((t + "/availability").c_str());
  }
  for (uint8_t i = 0; i < nDisps; i++) pedeEstado(disps[i]);
  Serial.println("MQTT conectado");
}

// ------------------------------------------------------------------ Alexa → Z2M
void comandoAlexa(uint8_t i, EspalexaDevice* dev) {
  if (i >= nDisps) return;
  Disp& d = disps[i];
  EspalexaDeviceProperty p = dev->getLastChangedProperty();
  bool liga = dev->getValue() > 0;
  int brilho = -1;
  if (d.tipo == T_DIMMER && liga && (p == EspalexaDeviceProperty::bri || p == EspalexaDeviceProperty::on)) {
    brilho = dev->getValue() > 254 ? 254 : dev->getValue();
    d.brilho = brilho;
  }
  d.ligado = liga;
  Serial.printf("Alexa: %s → %s\n", d.nome.c_str(), liga ? "ON" : "OFF");
  enviaZ2M(d, liga, brilho);
}

// ------------------------------------------------------------------ emulação WeMo (Belkin Socket): a Alexa mostra como TOMADA
// Um servidor HTTP por tomada, na porta 49153, 49154… (como o Tasmota). Descoberta pelo mesmo SSDP da Espalexa (gancho onSearch).
#define WEMO_PORTA_BASE 49153
struct WeMo { ESP8266WebServer* srv; uint8_t disp; uint16_t porta; };
WeMo wemos[ESPALEXA_MAXDEVICES];
uint8_t nWemo = 0;

bool usaWemo(const Disp& d) { return d.tipo == T_TOMADA && cfg.tomadaModo == "wemo"; }
String wemoSerial(uint8_t k) { char b[20]; snprintf(b, sizeof b, "221%06X%02u", (unsigned)(ESP.getChipId() & 0xFFFFFF), k); return b; }
String wemoUdn(uint8_t k) { return "Socket-1_0-" + wemoSerial(k); }

const char WEMO_EVENTSERVICE[] PROGMEM = "<?xml version=\"1.0\"?><scpd xmlns=\"urn:Belkin:service-1-0\"><specVersion><major>1</major><minor>0</minor></specVersion>"
  "<actionList><action><name>SetBinaryState</name><argumentList><argument><retval/><name>BinaryState</name><relatedStateVariable>BinaryState</relatedStateVariable>"
  "<direction>in</direction></argument></argumentList></action><action><name>GetBinaryState</name><argumentList><argument><retval/><name>BinaryState</name>"
  "<relatedStateVariable>BinaryState</relatedStateVariable><direction>out</direction></argument></argumentList></action></actionList>"
  "<serviceStateTable><stateVariable sendEvents=\"yes\"><name>BinaryState</name><dataType>Boolean</dataType><defaultValue>0</defaultValue></stateVariable>"
  "</serviceStateTable></scpd>";
const char WEMO_METAINFO[] PROGMEM = "<?xml version=\"1.0\"?><scpd xmlns=\"urn:Belkin:service-1-0\"><specVersion><major>1</major><minor>0</minor></specVersion>"
  "<actionList><action><name>GetMetaInfo</name><argumentList><retval/><name>GetMetaInfo</name><relatedStateVariable>MetaInfo</relatedStateVariable>"
  "<direction>in</direction></argumentList></action></actionList><serviceStateTable><stateVariable sendEvents=\"yes\"><name>MetaInfo</name>"
  "<dataType>string</dataType><defaultValue>0</defaultValue></stateVariable></serviceStateTable></scpd>";

void wemoInicia(uint8_t i) {
  if (nWemo >= ESPALEXA_MAXDEVICES) return;
  uint8_t k = nWemo++;
  WeMo& w = wemos[k];
  w.disp = i; w.porta = WEMO_PORTA_BASE + k; w.srv = new ESP8266WebServer(w.porta);
  ESP8266WebServer* sv = w.srv;
  sv->on("/setup.xml", HTTP_GET, [k, sv]() {
    Disp& d = disps[wemos[k].disp];
    String x = F("<?xml version=\"1.0\"?><root xmlns=\"urn:Belkin:device-1-0\"><specVersion><major>1</major><minor>0</minor></specVersion><device>"
                 "<deviceType>urn:Belkin:device:controllee:1</deviceType><friendlyName>");
    x += esc(d.nome);
    x += F("</friendlyName><manufacturer>Belkin International Inc.</manufacturer><modelName>Socket</modelName><modelNumber>3.1415</modelNumber>"
           "<modelDescription>Belkin Plugin Socket 1.0</modelDescription><UDN>uuid:");
    x += wemoUdn(k); x += F("</UDN><serialNumber>"); x += wemoSerial(k);
    x += F("</serialNumber><binaryState>"); x += d.ligado ? "1" : "0";
    x += F("</binaryState><serviceList><service><serviceType>urn:Belkin:service:basicevent:1</serviceType><serviceId>urn:Belkin:serviceId:basicevent1</serviceId>"
           "<controlURL>/upnp/control/basicevent1</controlURL><eventSubURL>/upnp/event/basicevent1</eventSubURL><SCPDURL>/eventservice.xml</SCPDURL></service>"
           "<service><serviceType>urn:Belkin:service:metainfo:1</serviceType><serviceId>urn:Belkin:serviceId:metainfo1</serviceId>"
           "<controlURL>/upnp/control/metainfo1</controlURL><eventSubURL>/upnp/event/metainfo1</eventSubURL><SCPDURL>/metainfoservice.xml</SCPDURL></service>"
           "</serviceList></device></root>\r\n");
    sv->send(200, "text/xml", x);
  });
  sv->on("/eventservice.xml", HTTP_GET, [sv]() { sv->send_P(200, "text/xml", WEMO_EVENTSERVICE); });
  sv->on("/metainfoservice.xml", HTTP_GET, [sv]() { sv->send_P(200, "text/xml", WEMO_METAINFO); });
  sv->on("/upnp/control/basicevent1", HTTP_POST, [k, sv]() {
    Disp& d = disps[wemos[k].disp];
    String corpo = sv->arg("plain");
    const char* acao = "GetBinaryState";
    if (corpo.indexOf("SetBinaryState") >= 0) {
      acao = "SetBinaryState";
      int p = corpo.indexOf("<BinaryState>");
      if (p >= 0) {
        bool liga = corpo.charAt(p + 13) == '1';
        d.ligado = liga;
        Serial.printf("Alexa (WeMo): %s → %s\n", d.nome.c_str(), liga ? "ON" : "OFF");
        enviaZ2M(d, liga, -1);
      }
    }
    String r = F("<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:");
    r += acao; r += F("Response xmlns:u=\"urn:Belkin:service:basicevent:1\"><BinaryState>"); r += d.ligado ? "1" : "0";
    r += F("</BinaryState></u:"); r += acao; r += F("Response></s:Body></s:Envelope>\r\n");
    sv->send(200, "text/xml; charset=\"utf-8\"", r);
  });
  sv->onNotFound([sv]() { sv->send(404, "text/plain", ""); });
  sv->begin();
}

void wemoBusca(const char* req, IPAddress ip, uint16_t porta) {
  if (!nWemo) return;
  const char* st = nullptr;
  if (strstr(req, "urn:Belkin:device:**")) st = "urn:Belkin:device:**";
  else if (strstr(req, "upnp:rootdevice")) st = "upnp:rootdevice";
  else if (strstr(req, "ssdp:all")) st = "urn:Belkin:device:**";
  if (!st) return;
  String meuIp = WiFi.localIP().toString();
  char buf[512];
  for (uint8_t k = 0; k < nWemo; k++) {
    snprintf_P(buf, sizeof buf, PSTR("HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=86400\r\nDATE: Sat, 01 Jan 2026 00:00:00 GMT\r\nEXT:\r\n"
               "LOCATION: http://%s:%u/setup.xml\r\nOPT: \"http://schemas.upnp.org/upnp/1/0/\"; ns=01\r\n01-NLS: %s\r\n"
               "SERVER: Unspecified, UPnP/1.0, Unspecified\r\nST: %s\r\nUSN: uuid:%s::%s\r\nX-User-Agent: redsonic\r\n\r\n"),
               meuIp.c_str(), wemos[k].porta, wemoUdn(k).c_str(), st, wemoUdn(k).c_str(), st);
    espalexa.sendUdp(ip, porta, buf);
    delay(5);
  }
}

void iniciaAlexa() {
  for (uint8_t i = 0; i < nDisps; i++) {
    Disp& d = disps[i];
    if (usaWemo(d)) { wemoInicia(i); continue; }
    EspalexaDeviceType tp = d.tipo == T_DIMMER ? EspalexaDeviceType::dimmable : EspalexaDeviceType::onoff;
    d.alexa = new EspalexaDevice(d.nome, [i](EspalexaDevice* dev) { comandoAlexa(i, dev); }, tp, 0);
    d.alexa->plug = d.tipo == T_TOMADA && cfg.tomadaModo == "hue";
    espalexa.addDevice(d.alexa);
  }
  espalexa.onSearch = wemoBusca;
  alexaIniciada = espalexa.begin(&server);
  Serial.printf("Alexa: %s · Hue %u, WeMo %u · tomadas como %s\n", alexaIniciada ? "ok" : "FALHOU (multicast)", nDisps - nWemo, nWemo, cfg.tomadaModo.c_str());
}

String comoNaAlexa(uint8_t i) {
  Disp& d = disps[i];
  if (d.tipo == T_DIMMER) return "lâmpada com brilho (Hue)";
  if (d.tipo == T_LUZ) return "lâmpada (Hue)";
  if (cfg.tomadaModo == "luz") return "lâmpada (Hue)";
  if (cfg.tomadaModo == "hue") return "tomada Hue";
  for (uint8_t k = 0; k < nWemo; k++) if (wemos[k].disp == i) return "tomada WeMo :" + String(wemos[k].porta);
  return "tomada WeMo";
}

// ------------------------------------------------------------------ página web
bool autenticado() {
  if (modoAP || cfg.webSenha.length() == 0) return true;   // na rede de configuração (senha do AP) não pede login
  if (server.authenticate(cfg.webUsuario.c_str(), cfg.webSenha.c_str())) return true;
  server.requestAuthentication(BASIC_AUTH, "z2m-alexa");
  return false;
}

// Atualiza a lista a cada 3 s pelo /estado.json (sem recarregar: não perde o que está sendo digitado no formulário)
// e liga/desliga sem recarregar a página. Sem JavaScript, os formulários continuam funcionando.
const char JS_LISTA[] PROGMEM = R"JS(<script>
function bt(i,d){return d.conhecido?'<form method=post action=/cmd style=display:inline onsubmit="return cmd('+i+','+(d.ligado?0:1)+')"><button class="'+(d.ligado?'on':'')+'">'+(d.ligado?'LIGADO':'DESLIGADO')+'</button></form>':'<small>sem resposta do Z2M</small>'}
async function at(){try{
 const r=await fetch('/estado.json',{cache:'no-store'});if(!r.ok)return;const e=await r.json();
 if(e.dispositivos.length!=N){location.reload();return}
 document.getElementById('mq').innerHTML=e.mqtt?'<b style="color:#66bb6a">conectado</b>':'<b style="color:#ef5350">desconectado</b>';
 e.dispositivos.forEach(function(d,i){var c=document.getElementById('s'+i);if(c)c.innerHTML=bt(i,d);
  var o=document.getElementById('o'+i);if(o)o.style.display=d.online?'none':'inline'});
 document.getElementById('hr').textContent=new Date().toLocaleTimeString('pt-BR');
}catch(_){}}
function cmd(i,v){fetch('/cmd',{method:'POST',body:new URLSearchParams({i:i,v:v,ajax:1})}).then(function(){setTimeout(at,500);setTimeout(at,1500)});return false}
setInterval(at,3000);
</script>)JS";

void enviaConfig();
void enviaPagina() {
  if (!autenticado()) return;
  if (modoAP) { enviaConfig(); return; }
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html; charset=utf-8", "");
  server.sendContent(F("<!doctype html><html lang=pt-BR><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Z2M Alexa</title><style>body{font-family:system-ui,sans-serif;background:#111;color:#eee;margin:0;padding:16px;max-width:760px}"
    "h1{font-size:20px}table{width:100%;border-collapse:collapse}td,th{padding:8px 4px;border-bottom:1px solid #333;text-align:left;vertical-align:middle}"
    "small,.m{color:#999}button,input,select{font:inherit;padding:8px;border-radius:6px;border:1px solid #444;background:#222;color:#eee}"
    "button{cursor:pointer}.on{background:#2e7d32;border-color:#2e7d32}.del{background:#5a1d1d;border-color:#5a1d1d}"
    "form.add{display:grid;gap:8px;grid-template-columns:1fr 1fr;margin-top:12px}form.add button{grid-column:span 2;background:#1565c0;border-color:#1565c0}"
    "@media(max-width:560px){form.add{grid-template-columns:1fr}form.add button{grid-column:auto}}</style></head><body>"));
  String s = "<h1>Z2M → Alexa · " + esc(cfg.host) + "</h1><p class=m>IP " + WiFi.localIP().toString() +
             " · MQTT <span id=mq>" + (mqtt.connected() ? "<b style='color:#66bb6a'>conectado</b>" : "<b style='color:#ef5350'>desconectado</b>") + "</span>" +
             " (" + esc(cfg.mqttHost) + ") · Alexa " + (alexaIniciada ? "ok" : "<b style='color:#ef5350'>falhou</b>") +
             " · memória livre " + String(ESP.getFreeHeap() / 1024) + " KB · v" VERSAO "</p>";
  server.sendContent(s);
  server.sendContent(F("<table><tr><th>Na Alexa</th><th>No Z2M</th><th>Estado</th><th></th></tr>"));
  for (uint8_t i = 0; i < nDisps; i++) {
    Disp& d = disps[i];
    String linha = "<tr><td><b>" + esc(d.nome) + "</b><br><small>" + TIPO_NOME[d.tipo] + " · Alexa: " + comoNaAlexa(i) + "</small></td><td>" + esc(d.z2m) +
                   "<br><small>" + esc(d.prop) + "<b id=o" + i + " style='color:#ef5350;display:" + (d.online ? "none" : "inline") +
                   "'> · offline</b></small></td><td id=s" + i + ">";
    if (!d.conhecido) linha += "<small>sem resposta do Z2M</small>";
    else linha += String("<form method=post action=/cmd style=display:inline onsubmit='return cmd(") + i + "," + (d.ligado ? "0" : "1") +
                  ")'><input type=hidden name=i value=" + i + "><input type=hidden name=v value=" + (d.ligado ? "0" : "1") +
                  "><button class=" + (d.ligado ? "on" : "") + ">" + (d.ligado ? "LIGADO" : "DESLIGADO") + "</button></form>";
    linha += String("</td><td><form method=post action=/del onsubmit=\"return confirm('Remover ") + esc(d.nome) +
             "?')\"><input type=hidden name=i value=" + i + "><button class=del title=Remover>✕</button></form></td></tr>";
    server.sendContent(linha);
  }
  if (!nDisps) server.sendContent(F("<tr><td colspan=4 class=m>Nenhum dispositivo ainda. Cadastre abaixo.</td></tr>"));
  server.sendContent(F("</table>"));
  if (nDisps < ESPALEXA_MAXDEVICES) {
    server.sendContent(F("<h2 style=font-size:16px;margin-top:24px>Adicionar dispositivo</h2><form class=add method=post action=/add>"
      "<input name=n placeholder='Nome na Alexa (ex.: Luz da Sala)' maxlength=40 required>"
      "<input name=z placeholder='friendly_name no Z2M (exato)' maxlength=40 required>"
      "<select name=t><option value=0>tomada (liga/desliga)</option><option value=1>luz (liga/desliga)</option><option value=2>dimmer (com brilho)</option></select>"
      "<input name=p value=state placeholder='propriedade (state, state_l1…)' maxlength=40>"
      "<button>Adicionar e reiniciar</button></form>"));
  }
  server.sendContent(F("<p class=m style=margin-top:24px>Depois de adicionar: <b>“Alexa, descobrir dispositivos”</b>. "
    "Remover um dispositivo muda o número dos que vêm depois na Alexa: rode a descoberta de novo e apague no app os que ficarem sem resposta.<br>"
    "<a href=/config style=color:#90caf9>Wi-Fi e MQTT</a> · <a href=/update style=color:#90caf9>Atualizar firmware</a> · <a href=/estado.json style=color:#90caf9>estado.json</a>"
    "<br>Atualizado às <span id=hr>—</span> (a cada 3 s)</p>"));
  server.sendContent("<script>var N=" + String(nDisps) + ";</script>");
  server.sendContent(FPSTR(JS_LISTA));
  server.sendContent(F("</body></html>"));
  server.sendContent("");
}

const char CSS[] PROGMEM = "<!doctype html><html lang=pt-BR><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
  "<title>Z2M Alexa</title><style>body{font-family:system-ui,sans-serif;background:#111;color:#eee;margin:0;padding:16px;max-width:560px}"
  "h1{font-size:20px}h2{font-size:15px;margin:18px 0 6px;color:#90caf9}label{display:block;margin:8px 0 2px;color:#bbb;font-size:14px}"
  "input{width:100%;box-sizing:border-box;font:inherit;padding:8px;border-radius:6px;border:1px solid #444;background:#222;color:#eee}"
  "button{margin-top:16px;width:100%;font:inherit;padding:10px;border-radius:6px;border:0;background:#1565c0;color:#fff;cursor:pointer}small{color:#999}</style></head><body>";

void campo(String& h, const char* rot, const char* nome, const String& valor, const char* tipo = "text", const char* dica = "") {
  h += "<label>"; h += rot; h += "</label><input name="; h += nome; h += " type="; h += tipo;
  h += " value=\""; h += esc(valor); h += "\""; if (*dica) { h += " placeholder=\""; h += dica; h += "\""; } h += ">";
}

void enviaConfig() {
  if (!autenticado()) return;
  String h = FPSTR(CSS);
  h += "<h1>Z2M → Alexa · configuração</h1>";
  if (modoAP) h += "<p><small>Rede de configuração. Preencha e salve: o ESP reinicia e entra no seu Wi-Fi. Depois acesse pelo IP que o roteador der a ele (ou http://" + esc(cfg.host) + ".local).</small></p>";
  h += "<form method=post action=/config><h2>Wi-Fi (rede das Echo)</h2>";
  campo(h, "Nome da rede (SSID)", "ssid", cfg.ssid);
  campo(h, "Senha do Wi-Fi", "wsenha", "", "password", cfg.wsenha.length() ? "(mantida; preencha só para trocar)" : "");
  campo(h, "Nome deste aparelho na rede", "host", cfg.host);
  h += "<h2>MQTT (broker do Zigbee2MQTT)</h2>";
  campo(h, "Endereço do broker", "mh", cfg.mqttHost, "text", "ex.: 192.168.2.10 ou IP do túnel VPN");
  campo(h, "Porta", "mp", String(cfg.mqttPorta), "number");
  campo(h, "Usuário MQTT", "mu", cfg.mqttUsuario);
  campo(h, "Senha MQTT", "ms", "", "password", cfg.mqttSenha.length() ? "(mantida; preencha só para trocar)" : "");
  campo(h, "base_topic do Zigbee2MQTT", "base", cfg.base);
  h += "<h2>Alexa</h2><label>Dispositivos do tipo \"tomada\" aparecem na Alexa como</label><select name=tm style='width:100%;padding:8px;border-radius:6px;background:#222;color:#eee;border:1px solid #444'>";
  const char* op[][2] = {{"hue", "Tomada (perfil Hue Smart plug) — padrão"}, {"wemo", "Tomada (emulação WeMo/Belkin)"}, {"luz", "Lâmpada (como até a versão 1.0)"}};
  for (auto& o : op) { h += "<option value="; h += o[0]; if (cfg.tomadaModo == o[0]) h += " selected"; h += ">"; h += o[1]; h += "</option>"; }
  h += "</select><p><small>Ao trocar: no app Alexa, <b>exclua</b> as tomadas antigas e diga \"Alexa, descobrir dispositivos\". "
       "Se continuarem como lâmpada, teste a outra opção de tomada.</small></p>";
  h += "<h2>Acesso a esta página</h2>";
  campo(h, "Usuário", "wu", cfg.webUsuario);
  campo(h, "Senha da página", "ws", "", "password", cfg.webSenha.length() ? "(mantida; preencha só para trocar)" : "obrigatória");
  h += "<button>Salvar e reiniciar</button></form>";
  if (!modoAP) h += "<p><small><a href=/ style=color:#90caf9>← voltar</a></small></p>";
  h += "</body></html>";
  server.send(200, "text/html; charset=utf-8", h);
}

void rotaConfig() {
  if (!autenticado()) return;
  // segredo = campo de senha: vazio no formulário significa "manter a atual"
  auto pega = [](const char* nome, String& dest, bool segredo) {
    String v = server.arg(nome); v.trim();
    if (v.length() || !segredo) dest = v;
  };
  pega("ssid", cfg.ssid, false); pega("wsenha", cfg.wsenha, true); pega("host", cfg.host, false);
  pega("mh", cfg.mqttHost, false); pega("mu", cfg.mqttUsuario, false); pega("ms", cfg.mqttSenha, true);
  pega("base", cfg.base, false); pega("wu", cfg.webUsuario, false); pega("ws", cfg.webSenha, true);
  if (server.hasArg("tm")) { String tm = server.arg("tm"); if (tm == "hue" || tm == "wemo" || tm == "luz") cfg.tomadaModo = tm; }
  long porta = server.arg("mp").toInt(); cfg.mqttPorta = (porta > 0 && porta < 65536) ? porta : 1883;
  if (!cfg.host.length()) cfg.host = "z2m-alexa";
  if (!cfg.base.length()) cfg.base = "zigbee2mqtt";
  if (!cfg.webUsuario.length()) cfg.webUsuario = "admin";
  if (!cfg.ssid.length() || !cfg.webSenha.length()) {
    server.send(400, "text/plain; charset=utf-8", "Preencha pelo menos o nome do Wi-Fi e a senha da página."); return;
  }
  if (!salvaCfg()) { server.send(500, "text/plain; charset=utf-8", "Falha ao gravar na flash."); return; }
  server.send(200, "text/html; charset=utf-8", String(FPSTR(CSS)) + "<h1>Salvo</h1><p>Reiniciando… Em alguns segundos acesse o aparelho no seu Wi-Fi.</p></body></html>");
  delay(500);
  ESP.restart();
}

void iniciaModoAP() {
  if (modoAP) return;
  modoAP = true;
  String nome = "z2m-alexa-" + String(ESP.getChipId() & 0xFFFF, HEX);
  WiFi.mode(cfg.ssid.length() ? WIFI_AP_STA : WIFI_AP);   // continua tentando o Wi-Fi salvo enquanto isso
  WiFi.softAP(nome.c_str(), AP_SENHA);
  server.begin();
  Serial.printf("Modo configuração: rede '%s' (senha '%s'), abra http://%s/\n", nome.c_str(), AP_SENHA, WiFi.softAPIP().toString().c_str());
}

void responde303() { server.sendHeader("Location", "/"); server.send(303, "text/plain", ""); }

void reiniciaDepois(const String& msg) {
  // não redireciona direto: o ESP está reiniciando e o navegador mostraria erro (e a pessoa reenviaria o formulário)
  server.send(200, "text/html; charset=utf-8",
    "<!doctype html><html lang=pt-BR><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<meta http-equiv=refresh content='30;url=/'><title>Reiniciando</title></head>"
    "<body style='font-family:system-ui,sans-serif;background:#111;color:#eee;padding:16px'><h2>" + esc(msg) + "</h2>"
    "<p id=t>Reiniciando… volta para a lista assim que o ESP responder. Não reenvie o formulário.</p>"
    "<p>Depois diga <b>“Alexa, descobrir dispositivos”</b>.</p>"
    // espera o reinício começar (3 s) e consulta a cada 1 s; o refresh de 30 s acima é a rede de segurança
    "<script>var n=0;function t(){fetch('/estado.json',{cache:'no-store'}).then(function(r){if(r.ok)location.replace('/');else throw 0})"
    ".catch(function(){document.getElementById('t').textContent='Reiniciando… aguardando o ESP ('+(++n)+' s)';setTimeout(t,1000)})}"
    "setTimeout(t,3000)</script></body></html>");
  delay(500);
  ESP.restart();
}

void erroForm(const String& msg) {
  server.send(400, "text/html; charset=utf-8",
    "<!doctype html><html lang=pt-BR><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'><title>Erro</title></head>"
    "<body style='font-family:system-ui,sans-serif;background:#111;color:#eee;padding:16px'><h2>Não foi possível salvar</h2><p>" + esc(msg) +
    "</p><p><a href=/ style=color:#90caf9>← voltar para a lista</a></p></body></html>");
}

void rotaAdd() {
  if (!autenticado()) return;
  String n = server.arg("n"), z = server.arg("z"), p = server.arg("p");
  n.trim(); z.trim(); p.trim(); if (!p.length()) p = "state";
  uint8_t t = server.arg("t").toInt();
  if (!textoValido(n, false) || !textoValido(z, true) || !textoValido(p, false) || t > 2) {
    erroForm("Dados inválidos: até 40 caracteres em cada campo; o nome no Z2M não pode ter # ou +."); return;
  }
  for (uint8_t i = 0; i < nDisps; i++) {
    if (!disps[i].nome.equalsIgnoreCase(n)) continue;
    if (disps[i].z2m == z && disps[i].prop == p && disps[i].tipo == (Tipo)t) { responde303(); return; }   // reenvio do mesmo formulário: já está salvo
    erroForm("Este ESP já tem um dispositivo chamado \"" + disps[i].nome + "\" (ligado a \"" + disps[i].z2m +
             "\" no Z2M). Use outro nome ou remova o existente na lista."); return;
  }
  if (nDisps >= ESPALEXA_MAXDEVICES) { erroForm("Limite de " + String(ESPALEXA_MAXDEVICES) + " dispositivos atingido."); return; }
  disps[nDisps].nome = n; disps[nDisps].z2m = z; disps[nDisps].prop = p; disps[nDisps].tipo = (Tipo)t;
  nDisps++;
  if (!salvaDisps()) { nDisps--; erroForm("Falha ao gravar na flash."); return; }
  reiniciaDepois("Adicionado: " + n);   // a Espalexa não remove/insere aparelhos com o servidor rodando
}

void rotaDel() {
  if (!autenticado()) return;
  int i = server.arg("i").toInt();
  if (i < 0 || i >= nDisps) { responde303(); return; }
  String removido = disps[i].nome;
  for (uint8_t j = i; j + 1 < nDisps; j++) {
    disps[j].nome = disps[j + 1].nome; disps[j].z2m = disps[j + 1].z2m; disps[j].prop = disps[j + 1].prop; disps[j].tipo = disps[j + 1].tipo;
  }
  nDisps--;
  salvaDisps();
  reiniciaDepois("Removido: " + removido);
}

void rotaCmd() {
  if (!autenticado()) return;
  int i = server.arg("i").toInt();
  if (i >= 0 && i < nDisps) enviaZ2M(disps[i], server.arg("v") == "1", -1);
  if (server.arg("ajax") == "1") { server.send(204, "text/plain", ""); return; }
  delay(150);   // dá tempo do Z2M responder antes de redesenhar
  mqtt.loop();
  responde303();
}

void rotaEstado() {
  JsonDocument doc;
  doc["mqtt"] = mqtt.connected(); doc["alexa"] = alexaIniciada; doc["ip"] = WiFi.localIP().toString();
  doc["heap"] = ESP.getFreeHeap(); doc["uptime"] = millis() / 1000; doc["versao"] = VERSAO;
  JsonArray a = doc["dispositivos"].to<JsonArray>();
  for (uint8_t i = 0; i < nDisps; i++) {
    JsonObject o = a.add<JsonObject>();
    o["nome"] = disps[i].nome; o["z2m"] = disps[i].z2m; o["prop"] = disps[i].prop; o["tipo"] = TIPO_NOME[disps[i].tipo]; o["alexa"] = comoNaAlexa(i);
    o["ligado"] = disps[i].ligado; o["conhecido"] = disps[i].conhecido; o["online"] = disps[i].online;
    if (disps[i].tipo == T_DIMMER) o["brilho"] = disps[i].brilho;
  }
  String s; serializeJson(doc, s);
  server.send(200, "application/json", s);
}

void naoEncontrado() {
  // tudo que não é página nossa vai para a API Hue (é assim que a Alexa conversa com a "ponte")
  if (!espalexa.handleAlexaApiCall(server.uri(), server.arg(0)))
    server.send(404, "text/plain", "Not found");
}

// ------------------------------------------------------------------ Wi-Fi
void verificaWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    if (modoAP) {                       // conectou enquanto a rede de configuração estava aberta: recomeça limpo
      Serial.println("Wi-Fi conectou: saindo do modo configuração");
      delay(100); ESP.restart();
    }
    if (!wifiJaConectou) {
      wifiJaConectou = true;
      Serial.printf("Wi-Fi ok: %s\n", WiFi.localIP().toString().c_str());
      iniciaAlexa();
    } else if (caiuEm) {
      // voltou depois de cair: reinicia para refazer o multicast (SSDP) no IP atual
      Serial.println("Wi-Fi voltou: reiniciando para refazer a descoberta da Alexa");
      delay(100); ESP.restart();
    }
    caiuEm = 0;
    return;
  }
  if (!wifiJaConectou && !modoAP && (!cfg.ssid.length() || millis() - inicioWifi > 60000UL)) iniciaModoAP();
  if (wifiJaConectou && !caiuEm) caiuEm = millis();
  if (caiuEm && millis() - caiuEm > 120000UL) ESP.restart();   // 2 min sem Wi-Fi: recomeça do zero
}

// ------------------------------------------------------------------ setup / loop
void setup() {
  Serial.begin(115200);
  delay(50);
  Serial.printf("\n\nZ2M → Alexa v%s\n", VERSAO);
  if (!LittleFS.begin()) { Serial.println("LittleFS: formatando"); LittleFS.format(); LittleFS.begin(); }
  carregaCfg();
  carregaDisps();
  Serial.printf("%u dispositivo(s) cadastrado(s)\n", nDisps);

  topicoStatus = cfg.host + "/status";
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.hostname(cfg.host);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);   // a Alexa espera resposta rápida
  WiFi.setAutoReconnect(true);
  inicioWifi = millis();
  if (cfg.ssid.length()) WiFi.begin(cfg.ssid.c_str(), cfg.wsenha.c_str());

  mqtt.setServer(cfg.mqttHost.c_str(), cfg.mqttPorta);
  mqtt.setCallback(recebeMqtt);
  mqtt.setBufferSize(2048);             // mensagens de tomadas com medição de energia são grandes
  mqtt.setKeepAlive(30);
  mqtt.setSocketTimeout(5);

  server.on("/", HTTP_GET, enviaPagina);
  server.on("/add", HTTP_POST, rotaAdd);
  server.on("/del", HTTP_POST, rotaDel);
  server.on("/cmd", HTTP_POST, rotaCmd);
  server.on("/estado.json", HTTP_GET, rotaEstado);
  server.on("/config", HTTP_GET, enviaConfig);
  server.on("/config", HTTP_POST, rotaConfig);
  server.onNotFound(naoEncontrado);
  atualizador.setup(&server, "/update", cfg.webSenha.length() ? cfg.webUsuario : String(""), cfg.webSenha);
  // o servidor é iniciado pela Espalexa (espalexa.begin) quando o Wi-Fi conectar
}

void loop() {
  verificaWifi();
  if (!wifiJaConectou) {
    if (modoAP) server.handleClient();
    delay(10); return;
  }
  espalexa.loop();          // atende HTTP (página + API Hue) e SSDP
  for (uint8_t k = 0; k < nWemo; k++) wemos[k].srv->handleClient();
  conectaMqtt();
  mqtt.loop();
  delay(1);
}
