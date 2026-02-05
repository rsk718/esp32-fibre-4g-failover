/*************************************************
 ESP32-C5 SURVEILLANCE FIBRE + ROUTEUR 4G
 VERSION COMPLETE + ROBUSTE (COUPURE EDF) + LCD WAKE

 - Relais HL-52S (LOW = ON)
 - 2 relais :
    RELAY_4G_PIN        = 14  (alim box 4G)
    RELAY_FIBRE_RESET   = 26  (contact NC pour reset box fibre 5s)
 - LCD 16x2 I2C SDA=8 / SCL=9 (0x27)
 - LED_INTERNET_OK IO4 / LED_4G_ACTIVE IO5
 - BTN_TEST IO10 : court=4G ON / long=4G OFF
 - BTN_RESET_WIFI IO0 :
      * appui court : réveille LCD
      * appui 5s    : portail Config-Monitor (AP)
      * appui 12s   : efface WiFi + reboot
 - RGB WS2812 1 pixel sur IO27
 - Anti coupure EDF : pas de portail auto, attente WiFi + retry 10s
 - Web UI complète + historique persisté

*************************************************/

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <Wire.h>
#include <HTTPClient.h>
#include <LiquidCrystal_I2C.h>
#include <Adafruit_NeoPixel.h>
#include <time.h>

/* ==================== PINS ==================== */
#define SDA_PIN 8
#define SCL_PIN 9

#define RELAY_4G_PIN        14
#define RELAY_FIBRE_RESET   26
#define RELAY_ON            LOW
#define RELAY_OFF           HIGH

#define LED_INTERNET_OK     4
#define LED_4G_ACTIVE       5

#define BTN_TEST            10
#define BTN_RESET_WIFI      0

#define RGB_PIN             27
#define NB_PIXELS           1

/* ==================== OBJETS ==================== */
Preferences prefs;
WebServer server(80);
LiquidCrystal_I2C lcd(0x27, 16, 2);
Adafruit_NeoPixel rgb(NB_PIXELS, RGB_PIN, NEO_GRB + NEO_KHZ800);

/* ==================== WIFI / CONFIG ==================== */
String wifiSsid = "";
String wifiPass = "";
String boxIP    = "192.168.1.1";

const char* AP_SSID  = "Config-Monitor";
const char* AP_PASS  = "12345678";

bool portalMode = false;

// Anti-coupure EDF : attente + retry si WiFi indisponible au boot
bool wifiPending = false;
unsigned long lastWiFiRetry = 0;
const unsigned long WIFI_RETRY_MS = 10000; // 10s

/* ==================== NTP ==================== */
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec      = 3600;
const int   daylightOffset_sec = 3600;

void setupTime() {
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
}

String getDateTime() {
  struct tm ti;
  if (!getLocalTime(&ti)) return "??/?? ??:??:??";
  char buff[32];
  strftime(buff, sizeof(buff), "%d/%m %H:%M:%S", &ti);
  return String(buff);
}

/* ==================== ETATS ==================== */
bool fibreOK        = true;
bool modeAuto       = true;
bool routeur4GActif = false;

unsigned long dernierCheck = 0;
const unsigned long CHECK_INTERVAL_MS = 10000;

int echecInternetCount = 0;
const int SEUIL_ECHECS = 3;

// Reset fibre 5s
bool fibreResetEnCours = false;
unsigned long fibreResetStartMs = 0;
const unsigned long FIBRE_RESET_DUREE_MS = 5000;

// LCD scroll
String scrollText = "";
int scrollIndex = 0;
unsigned long lastScroll = 0;
const int SCROLL_SPEED = 500;

// Messages temporaires LCD
bool lcdTempMsg = false;
unsigned long lcdTempUntil = 0;

/* ==================== LCD BACKLIGHT (WAKE) ==================== */
bool lcdBacklightOn = true;
unsigned long lastLcdWakeMs = 0;
const unsigned long LCD_BACKLIGHT_TIMEOUT_MS = 180000; // 3 min

void wakeLCD() {
  lastLcdWakeMs = millis();
  if (!lcdBacklightOn) {
    lcd.backlight();
    lcdBacklightOn = true;
  }
}

void updateBacklightTimeout() {
  if (lcdBacklightOn && (millis() - lastLcdWakeMs > LCD_BACKLIGHT_TIMEOUT_MS)) {
    lcd.noBacklight();
    lcdBacklightOn = false;
  }
}

/* ==================== HISTORIQUE ==================== */
String historique = "";

void saveHistory() {
  prefs.begin("history", false);
  prefs.putString("log", historique);
  prefs.end();
}

void loadHistory() {
  prefs.begin("history", true);
  historique = prefs.getString("log", "");
  prefs.end();
}

void addHistory(const String &txt) {
  historique += txt + "\n";
  saveHistory();
}

void clearHistory() {
  historique = "";
  saveHistory();
}

/* ==================== PREFS WIFI/BOX ==================== */
void saveWiFi(const String &s, const String &p) {
  prefs.begin("wifi", false);
  prefs.putString("ssid", s);
  prefs.putString("pass", p);
  prefs.end();
}

void loadWiFi() {
  prefs.begin("wifi", true);
  wifiSsid = prefs.getString("ssid", "");
  wifiPass = prefs.getString("pass", "");
  prefs.end();
}

void clearWiFi() {
  prefs.begin("wifi", false);
  prefs.clear();
  prefs.end();
  wifiSsid = "";
  wifiPass = "";
}

void saveBoxIP(const String &ip) {
  prefs.begin("box", false);
  prefs.putString("box_ip", ip);
  prefs.end();
  boxIP = ip;
}

void loadBoxIP() {
  prefs.begin("box", true);
  boxIP = prefs.getString("box_ip", "192.168.1.1");
  prefs.end();
}

/* ==================== WIFI UTILS ==================== */
String wifiBand() {
  if (WiFi.status() != WL_CONNECTED) return "??";
  int ch = WiFi.channel();
  if (ch >= 1 && ch <= 14) return "2.4G";
  return "5G";
}

/* ==================== TESTS RESEAU ==================== */
bool testBoxFibre() {
  HTTPClient http;
  http.setTimeout(1500);
  if (!http.begin("http://" + boxIP + "/")) return false;
  int code = http.GET();
  http.end();
  return code > 0;
}

bool testInternet() {
  HTTPClient http;
  http.setTimeout(2000);
  http.begin("http://connectivitycheck.gstatic.com/generate_204");
  int code = http.GET();
  http.end();
  return (code == 204);
}

/* ==================== RELAIS ==================== */
void activer4G() {
  digitalWrite(RELAY_4G_PIN, RELAY_ON);
  routeur4GActif = true;
  digitalWrite(LED_4G_ACTIVE, HIGH);
  addHistory(getDateTime() + " -> 4G ACTIVE");
}

void couper4G() {
  digitalWrite(RELAY_4G_PIN, RELAY_OFF);
  routeur4GActif = false;
  digitalWrite(LED_4G_ACTIVE, LOW);
  addHistory(getDateTime() + " -> 4G COUPEE");
}

void resetBoxFibre_start() {
  fibreResetEnCours = true;
  fibreResetStartMs = millis();
  digitalWrite(RELAY_FIBRE_RESET, RELAY_ON);  // ouvre NC => coupe alim box fibre
  addHistory(getDateTime() + " -> RESET BOX FIBRE (5s)");
}

void handleFibreResetProcess() {
  if (!fibreResetEnCours) return;
  if (millis() - fibreResetStartMs >= FIBRE_RESET_DUREE_MS) {
    digitalWrite(RELAY_FIBRE_RESET, RELAY_OFF); // referme NC
    fibreResetEnCours = false;
    addHistory(getDateTime() + " -> FIN RESET BOX FIBRE");
  }
}

/* ==================== RGB ==================== */
void rgbSet(uint8_t r, uint8_t g, uint8_t b) {
  rgb.setPixelColor(0, rgb.Color(r,g,b));
  rgb.show();
}

int breath = 10;
int breathDir = 1;
unsigned long lastBreath = 0;

bool blink = false;
unsigned long lastBlink = 0;

void updateRGB() {
  unsigned long now = millis();

  if (!fibreOK) {
    if (now - lastBlink > 500) { lastBlink = now; blink = !blink; }
    rgbSet(blink ? 255 : 0, 0, 0);
    return;
  }

  if (routeur4GActif) {
    rgbSet(255, 80, 0);
    return;
  }

  if (now - lastBreath > 30) {
    lastBreath = now;
    breath += breathDir * 3;
    if (breath >= 140) { breath = 140; breathDir = -1; }
    if (breath <= 10)  { breath = 10;  breathDir = 1; }
  }
  rgbSet(0, (uint8_t)breath, 0);
}

/* ==================== LCD ==================== */
void lcdTempMessage(const String &l1, const String &l2, unsigned long ms) {
  wakeLCD();
  lcdTempMsg = true;
  lcdTempUntil = millis() + ms;
  lcd.setCursor(0,0); lcd.print("                ");
  lcd.setCursor(0,1); lcd.print("                ");
  lcd.setCursor(0,0); lcd.print(l1.substring(0,16));
  lcd.setCursor(0,1); lcd.print(l2.substring(0,16));
}

void updateScrollingLCD() {
  if (millis() - lastScroll < (unsigned long)SCROLL_SPEED) return;
  lastScroll = millis();

  if (scrollText.length() <= 16) {
    String p = scrollText + "                ";
    lcd.setCursor(0, 1);
    lcd.print(p.substring(0, 16));
    return;
  }

  if (scrollIndex + 16 > (int)scrollText.length()) scrollIndex = 0;
  lcd.setCursor(0, 1);
  lcd.print(scrollText.substring(scrollIndex, scrollIndex + 16));
  scrollIndex++;
}

void lcdNormalDisplay() {
  String l1 = (fibreOK ? String("Fibre OK ") : String("Fibre HS "));
  l1 += "[" + wifiBand() + "]";
  lcd.setCursor(0,0);
  lcd.print((l1 + "                ").substring(0,16));

  String ssid = (WiFi.status() == WL_CONNECTED) ? WiFi.SSID() : "WiFi...";
  String modeTxt = modeAuto ? "AUTO" : "MAN";
  String g4Txt   = routeur4GActif ? "4G ON" : "4G OFF";

  scrollText = "SSID:" + ssid + " | " + modeTxt + " | " + g4Txt + " | IP:" + WiFi.localIP().toString() + "   ";
  updateScrollingLCD();
}

/* ==================== WEB UI ==================== */
String css = R"=====(
<style>
body{font-family:Arial;background:#f2f2f2;margin:20px;}
h2{text-align:center;}
.card{background:white;padding:15px;margin-bottom:15px;border-radius:10px;
box-shadow:0 2px 5px rgba(0,0,0,0.2);}
.btn{display:block;width:100%;padding:12px;margin:6px 0;background:#1976d2;color:white;border:none;
border-radius:6px;font-size:17px;cursor:pointer;}
.btn-red{background:#c62828;}
.btn-orange{background:#ef6c00;}
.btn-green{background:#2e7d32;}
.status-ok{color:green;font-weight:bold;}
.status-no{color:red;font-weight:bold;}
pre{background:#333;color:#0f0;padding:10px;border-radius:8px;overflow:auto;}
.small{color:#666;font-size:12px;}
</style>
)=====";

void handleRoot() {
  wakeLCD();

  String s = "<html><head><meta charset='UTF-8'>" + css + "</head><body>";
  s += "<h2>Surveillance Fibre / 4G (ESP32-C5)</h2>";

  String ssid = WiFi.SSID();
  int rssi = WiFi.RSSI();
  int ch   = WiFi.channel();

  s += "<div class='card'>";
  s += "<p><b>Fibre :</b> " + String(fibreOK ? "<span class='status-ok'>🟢 OK</span>" : "<span class='status-no'>🔴 HS</span>") + "</p>";
  s += "<p><b>4G :</b> " + String(routeur4GActif ? "🟠 ACTIVE (secours)" : "⚪ OFF") + "</p>";
  s += "<p><b>Mode :</b> " + String(modeAuto ? "AUTO (retour Fibre auto)" : "MANUEL") + "</p>";
  s += "<p><b>IP Box Fibre :</b> " + boxIP + "</p>";
  s += "<p><b>SSID :</b> " + ssid + "</p>";
  s += "<p><b>Bande :</b> " + wifiBand() + " (CH " + String(ch) + ", RSSI " + String(rssi) + " dBm)</p>";
  s += "<p><b>IP ESP32 :</b> " + WiFi.localIP().toString() + "</p>";
  s += "<p><b>Heure :</b> " + getDateTime() + "</p>";
  s += "</div>";

  s += "<div class='card'>";
  s += "<form method='POST' action='/toggle'><button class='btn btn-green'>Basculer AUTO / MAN</button></form>";
  s += "<form method='POST' action='/force4g'><button class='btn btn-orange'>Activer 4G</button></form>";
  s += "<form method='POST' action='/stop4g'><button class='btn btn-red'>Couper 4G (manuel)</button></form>";
  s += "<form method='GET' action='/wifi'><button class='btn'>Configurer WiFi</button></form>";
  s += "<form method='GET' action='/box'><button class='btn'>Configurer IP Box Fibre</button></form>";
  s += "<form method='POST' action='/portal'><button class='btn btn-orange'>Mode Config WiFi (AP)</button></form>";
  s += "<form method='POST' action='/clear'><button class='btn btn-red'>Effacer historique</button></form>";
  s += "<div class='small'>RESET WiFi: 5s=Config AP, 12s=Effacer WiFi. Appui court = réveil écran.</div>";
  s += "</div>";

  s += "<div class='card'><h3>Historique</h3><pre>" + historique + "</pre></div>";
  s += "</body></html>";

  server.send(200,"text/html",s);
}

void handleToggleMode() {
  wakeLCD();
  modeAuto = !modeAuto;
  addHistory(getDateTime() + String(" -> Mode ") + (modeAuto ? "AUTO" : "MAN"));
  server.sendHeader("Location","/");
  server.send(302);
}

void handleForce4G() {
  wakeLCD();
  activer4G();
  server.sendHeader("Location","/");
  server.send(302);
}

void handleStop4G() {
  wakeLCD();
  couper4G();
  server.sendHeader("Location","/");
  server.send(302);
}

void handleClearHist() {
  wakeLCD();
  clearHistory();
  server.sendHeader("Location","/");
  server.send(302);
}

void handleWifiForm() {
  wakeLCD();
  String p =
    "<html><head><meta charset='UTF-8'>" + css + "</head><body>"
    "<div class='card'><h2>Configuration WiFi (STA)</h2>"
    "<form method='POST' action='/savewifi'>"
    "SSID : <input name='ssid' value='" + wifiSsid + "'><br><br>"
    "Mot de passe : <input type='password' name='pass' value='" + wifiPass + "'><br><br>"
    "<button class='btn'>Enregistrer</button></form>"
    "<p><a href='/'><button class='btn btn-red'>Retour</button></a></p>"
    "</div></body></html>";
  server.send(200,"text/html",p);
}

void handleSaveWifi() {
  wakeLCD();
  saveWiFi(server.arg("ssid"), server.arg("pass"));
  addHistory(getDateTime() + " -> WiFi enregistre (STA)");
  server.send(200,"text/html","WiFi enregistré. Redémarrage…");
  delay(800);
  ESP.restart();
}

void handleBoxForm() {
  wakeLCD();
  String p =
    "<html><head><meta charset='UTF-8'>" + css + "</head><body>"
    "<div class='card'><h2>IP Box Fibre</h2>"
    "<form method='POST' action='/savebox'>"
    "<p>IP actuelle : <b>" + boxIP + "</b></p>"
    "<input type='radio' name='preset' value='192.168.1.1'> 192.168.1.1<br>"
    "<input type='radio' name='preset' value='192.168.0.1'> 192.168.0.1<br>"
    "<input type='radio' name='preset' value='192.168.1.254'> 192.168.1.254<br><br>"
    "Autre : <input name='otherip'><br><br>"
    "<button class='btn'>Enregistrer</button></form>"
    "<p><a href='/'><button class='btn btn-red'>Retour</button></a></p>"
    "</div></body></html>";
  server.send(200,"text/html",p);
}

void handleSaveBox() {
  wakeLCD();
  String preset = server.arg("preset");
  String chosen = (preset == "") ? server.arg("otherip") : preset;
  saveBoxIP(chosen);
  addHistory(getDateTime() + " -> IP Box Fibre = " + chosen);
  server.sendHeader("Location","/");
  server.send(302);
}

/* ==================== SERVEUR NORMAL (STA) ==================== */
void setupNormalServer() {
  server.stop();
  delay(20);

  server.on("/",        HTTP_GET,  handleRoot);
  server.on("/toggle",  HTTP_POST, handleToggleMode);
  server.on("/force4g", HTTP_POST, handleForce4G);
  server.on("/stop4g",  HTTP_POST, handleStop4G);
  server.on("/clear",   HTTP_POST, handleClearHist);

  server.on("/wifi",    HTTP_GET,  handleWifiForm);
  server.on("/savewifi",HTTP_POST, handleSaveWifi);

  server.on("/box",     HTTP_GET,  handleBoxForm);
  server.on("/savebox", HTTP_POST, handleSaveBox);

  server.on("/portal",  HTTP_POST, [](){
    wakeLCD();
    addHistory(getDateTime() + " -> Portail config demande WEB");
    server.send(200, "text/plain", "OK - Passage en mode Config (AP)");
    delay(200);
    portalMode = true; // sera lancé dans loop
  });

  server.begin();
}

/* ==================== PORTAIL CONFIG (AP) ==================== */
void startConfigPortal() {
  server.stop();
  delay(50);

  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP);

  IPAddress local_IP(192,168,4,1);
  IPAddress gateway(192,168,4,1);
  IPAddress subnet(255,255,255,0);
  WiFi.softAPConfig(local_IP, gateway, subnet);
  WiFi.softAP(AP_SSID, AP_PASS, 6, false, 4);

  lcdTempMessage("Config WiFi AP", "192.168.4.1", 1500);

  server.on("/", HTTP_GET, []() {
    String p =
      "<html><head><meta charset='UTF-8'>" + css + "</head><body>"
      "<div class='card'><h2>Config WiFi (AP)</h2>"
      "<p>Connecte-toi au SSID <b>Config-Monitor</b> puis renseigne :</p>"
      "<form method='POST' action='/save'>"
      "SSID : <input name='ssid'><br><br>"
      "Mot de passe : <input type='password' name='pass'><br><br>"
      "<button class='btn'>Enregistrer</button></form>"
      "</div></body></html>";
    server.send(200,"text/html",p);
  });

  server.on("/save", HTTP_POST, []() {
    saveWiFi(server.arg("ssid"), server.arg("pass"));
    addHistory(getDateTime() + " -> WiFi enregistre (AP)");
    server.send(200,"text/html","WiFi enregistré. Redémarrage...");
    delay(800);
    ESP.restart();
  });

  server.begin();
  portalMode = true;
  wifiPending = false;
  addHistory(getDateTime() + " -> Mode CONFIG (AP)");
}

/* ==================== CONNEXION WIFI STA ==================== */
bool tryWiFiOnce(unsigned long timeoutMs) {
  if (wifiSsid == "") return false;

  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) {
    delay(100);
  }
  return (WiFi.status() == WL_CONNECTED);
}

void startSTAorPending() {
  bool ok = tryWiFiOnce(10000);
  if (ok) {
    portalMode = false;
    wifiPending = false;
    setupNormalServer();
    setupTime();
    addHistory(getDateTime() + " -> WiFi connecte (boot)");
    lcdTempMessage("WiFi OK", WiFi.localIP().toString(), 1000);
  } else {
    portalMode = false;
    wifiPending = true;
    lastWiFiRetry = millis();
    addHistory(getDateTime() + " -> WiFi absent au boot, attente");
    lcdTempMessage("Attente WiFi", "Box en boot ?", 1200);
  }
}

/* ==================== SETUP ==================== */
void setup() {
  Serial.begin(115200);

  pinMode(RELAY_4G_PIN, OUTPUT);
  pinMode(RELAY_FIBRE_RESET, OUTPUT);
  digitalWrite(RELAY_4G_PIN, RELAY_OFF);
  digitalWrite(RELAY_FIBRE_RESET, RELAY_OFF);

  pinMode(LED_INTERNET_OK, OUTPUT);
  pinMode(LED_4G_ACTIVE, OUTPUT);
  digitalWrite(LED_INTERNET_OK, LOW);
  digitalWrite(LED_4G_ACTIVE, LOW);

  pinMode(BTN_TEST, INPUT_PULLUP);
  pinMode(BTN_RESET_WIFI, INPUT_PULLUP);

  Wire.begin(SDA_PIN, SCL_PIN);
  lcd.init();
  lcd.backlight();
  lcdBacklightOn = true;
  lastLcdWakeMs = millis();

  rgb.begin();
  rgb.setBrightness(60);
  rgb.clear();
  rgb.show();

  loadWiFi();
  loadBoxIP();
  loadHistory();

  startSTAorPending();
}

/* ==================== LOOP ==================== */
void loop() {
  // gestion extinction backlight
  updateBacklightTimeout();

  // si demande portail via WEB (flag)
  if (portalMode && WiFi.getMode() != WIFI_AP) {
    startConfigPortal();
  }

  server.handleClient();
  handleFibreResetProcess();
  updateRGB();

  // --------- BTN_RESET_WIFI : appui court = wake, 5s portail, 12s efface ----------
  static bool btnRstPressed = false;
  static unsigned long btnRstT0 = 0;

  if (digitalRead(BTN_RESET_WIFI) == LOW) {
    if (!btnRstPressed) {
      btnRstPressed = true;
      btnRstT0 = millis();
      wakeLCD(); // APPUI COURT = réveil
    } else {
      unsigned long held = millis() - btnRstT0;

      if (held > 12000) {
        lcdTempMessage("Efface WiFi", "Reboot...", 1200);
        addHistory(getDateTime() + " -> WiFi efface (bouton)");
        clearWiFi();
        delay(1200);
        ESP.restart();
      } else if (held > 5000 && WiFi.getMode() != WIFI_AP) {
        lcdTempMessage("Mode CONFIG", "AP Config-Mon", 1200);
        startConfigPortal();
      }
    }
  } else {
    btnRstPressed = false;
  }

  // --------- Reconnexion WiFi si attente (anti coupure EDF) ----------
  if (!portalMode && wifiPending) {
    if (millis() - lastWiFiRetry > WIFI_RETRY_MS) {
      lastWiFiRetry = millis();
      bool ok = tryWiFiOnce(5000);
      if (ok) {
        wifiPending = false;
        setupNormalServer();
        setupTime();
        addHistory(getDateTime() + " -> WiFi revenu (retry)");
        lcdTempMessage("WiFi OK", WiFi.localIP().toString(), 900);
      } else {
        if (!lcdTempMsg) {
          lcd.setCursor(0,0);
          lcd.print("Attente WiFi... ");
        }
      }
    }

    if (lcdTempMsg && millis() > lcdTempUntil) lcdTempMsg = false;
    if (!lcdTempMsg) lcdNormalDisplay();
    delay(20);
    return;
  }

  // --------- BTN_TEST : appui court = 4G ON, long = 4G OFF (et wake LCD) ----------
  static bool btnWasPressed = false;
  static unsigned long btnPressTime = 0;

  if (digitalRead(BTN_TEST) == LOW) {
    if (!btnWasPressed) {
      btnWasPressed = true;
      btnPressTime = millis();
      wakeLCD(); // APPUI COURT/ LONG = réveil
    }
  } else {
    if (btnWasPressed) {
      unsigned long duration = millis() - btnPressTime;

      if (duration >= 3000) {
        couper4G();
        lcdTempMessage("4G COUPEE", "Bouton long", 1200);
      } else if (duration >= 80) {
        activer4G();
        lcdTempMessage("4G ACTIVE", "Bouton court", 1200);
      }
    }
    btnWasPressed = false;
  }

  // --------- SURVEILLANCE FIBRE / INTERNET ----------
  if (millis() - dernierCheck > CHECK_INTERVAL_MS) {
    dernierCheck = millis();

    bool boxOK  = testBoxFibre();
    bool inetOK = testInternet();
    bool fibreOK_new = (boxOK && inetOK);

    if (!fibreOK_new) {
      echecInternetCount++;
      if (echecInternetCount >= SEUIL_ECHECS && fibreOK) {
        fibreOK = false;
        digitalWrite(LED_INTERNET_OK, LOW);
        addHistory(getDateTime() + " -> Fibre/Internet KO (3 echec)");

        if (modeAuto && !routeur4GActif) {
          resetBoxFibre_start();
          addHistory(getDateTime() + " -> Bascule automatique 4G");
          activer4G();
          lcdTempMessage("Fibre KO", "Bascule 4G", 1500);
        }
      }
    } else {
      if (!fibreOK) {
        fibreOK = true;
        digitalWrite(LED_INTERNET_OK, HIGH);
        addHistory(getDateTime() + " -> Fibre/Internet OK");

        if (modeAuto && routeur4GActif) {
          addHistory(getDateTime() + " -> Retour Fibre, coupe 4G");
          couper4G();
          lcdTempMessage("Retour Fibre", "Coupe 4G", 1500);
        }
      }
      echecInternetCount = 0;
      digitalWrite(LED_INTERNET_OK, HIGH);
    }
  }

  // LCD
  if (lcdTempMsg && millis() > lcdTempUntil) lcdTempMsg = false;
  if (!lcdTempMsg) lcdNormalDisplay();

  delay(20);
}
