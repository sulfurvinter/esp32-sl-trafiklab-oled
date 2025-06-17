#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <U8g2lib.h>
#include <SPI.h>
#include "qrcode2.h"

const char* ssid = "guest";
const char* password = "87GreenBluebirds?";

WebServer server(80);
Preferences prefs;

#define CLK 20
#define DATA 19
#define DC 18 
#define RESET 15
#define CS 14

U8G2_SSD1322_NHD_256X64_F_4W_HW_SPI u8g2(U8G2_R0, CS, DC, RESET);

unsigned long lastFetchTime = 0;
String savedId = "";
String savedName = "";

void drawQrCode(int x_offset, int y_offset, const char* text) {
  QRCode qrcode;
  const uint8_t QRcode_Version = 3;
  const uint8_t QRcode_ECC = ECC_LOW;

  uint8_t qrcodeData[qrcode_getBufferSize(QRcode_Version)];
  qrcode_initText(&qrcode, qrcodeData, QRcode_Version, QRcode_ECC, text);

  u8g2.clearBuffer();

  // Fill entire display white
  for (uint8_t y = 0; y < 63; y++) {
    for (uint8_t x = 0; x < 255; x++) {
      u8g2.setDrawColor(1);
      u8g2.drawPixel(x, y);
    }
  }

  // Draw QR code modules as black
  for (uint8_t y = 0; y < qrcode.size; y++) {
    for (uint8_t x = 0; x < qrcode.size; x++) {
      if (qrcode_getModule(&qrcode, x, y) == 1) {
        u8g2.setDrawColor(0);
        u8g2.drawPixel(x_offset + x, y_offset + y);
      }
    }
  }

  u8g2.sendBuffer();
}

const char HTML_TEMPLATE[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><title>SL Search</title><meta name="viewport" content="width=device-width, initial-scale=1"></head>
<body><h2>Search SL Sites</h2>
<input type="text" id="searchTerm" placeholder="Enter name...">
<button onclick="search()">Search</button><br><br>
<select id="results"></select><br><br>
<button onclick="save()">Save Selection</button>
<script>
let defaultName = "%NAME%";
let defaultId = "%ID%";
async function search() {
  const term = document.getElementById("searchTerm").value.toLowerCase();
  const response = await fetch("http://transport.integration.sl.se/v1/sites?expand=false");
  const data = await response.json();
  const results = document.getElementById("results");
  results.innerHTML = "";
  for (const entry of data) {
    if (entry.name && entry.name.toLowerCase().includes(term)) {
      const option = document.createElement("option");
      option.text = `${entry.name} (id: ${entry.id})`;
      option.value = JSON.stringify({ name: entry.name, id: entry.id });
      if (entry.name === defaultName && entry.id.toString() === defaultId) option.selected = true;
      results.appendChild(option);
    }
  }
  if (results.options.length === 0) {
    const none = document.createElement("option");
    none.text = "No matches found";
    none.disabled = true;
    results.appendChild(none);
  }
}
async function save() {
  const results = document.getElementById("results");
  if (results.selectedIndex === -1) return;
  const selected = JSON.parse(results.options[results.selectedIndex].value);
  await fetch("/save", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(selected)
  });
  alert(`Saved: ${selected.name} (id: ${selected.id})`);
}
</script></body></html>
)rawliteral";

void setup() {
  Serial.begin(115200);
  delay(100);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected!");

  SPI.begin(CLK, -1, DATA, CS);
  u8g2.setBusClock(25000000);
  u8g2.begin();
  u8g2.clearBuffer();
  u8g2.sendBuffer();

  prefs.begin("slprefs", false);
  savedId = prefs.getString("id", "");
  savedName = prefs.getString("name", "");

  if (savedId == "") {
    server.on("/", []() {
      String html = HTML_TEMPLATE;
      html.replace("%NAME%", savedName);
      html.replace("%ID%", savedId);
      server.send(200, "text/html", html);
    });

    server.on("/save", HTTP_POST, []() {
      if (server.hasArg("plain")) {
        String body = server.arg("plain");
        int nameIndex = body.indexOf("\"name\":\"");
        int idIndex = body.indexOf("\"id\":");

        if (nameIndex >= 0 && idIndex >= 0) {
          int nameStart = nameIndex + 8;
          int nameEnd = body.indexOf("\"", nameStart);
          int idStart = idIndex + 5;
          int idEnd = body.indexOf("}", idStart);
          savedName = body.substring(nameStart, nameEnd);
          savedId = body.substring(idStart, idEnd);
          savedId.trim();

          prefs.putString("name", savedName);
          prefs.putString("id", savedId);

          server.send(200, "text/plain", "Saved");
          return;
        }
      }
      server.send(400, "text/plain", "Invalid Data");
    });
    String url = "http://" + WiFi.localIP().toString();
    drawQrCode((256 - 29) / 2, (64 - 29) / 2, url.c_str());
    server.begin();
  }
}

void loop() {
  if (savedId == "") {
    server.handleClient();
    return;
  }

  if (millis() - lastFetchTime > 60000) {
    lastFetchTime = millis();
    String url = "http://transport.integration.sl.se/v1/sites/" + savedId + "/departures?transport=METRO&forecast=30";

    HTTPClient http;
    http.useHTTP10(true);
    http.begin(url);
    int httpCode = http.GET();

    if (httpCode == HTTP_CODE_OK) {
      WiFiClient* stream = http.getStreamPtr();
      char* buffer = (char*)malloc(26001);
      if (!buffer) return;
      int index = 0;
      while (stream->available() && index < 26000)
        buffer[index++] = stream->read();
      buffer[index] = '\0';

      StaticJsonDocument<26000> doc;
      DeserializationError err = deserializeJson(doc, buffer);
      free(buffer);
      http.end();

      if (err) return;

      JsonArray departures = doc["departures"];
      if (departures.size() > 0) {
        const size_t maxDepartures = 20;
        String depList[maxDepartures];
        size_t count = 0;
        for (JsonObject departure : departures) {
          const char* destination = departure["destination"];
          const char* display = departure["display"];
          if (destination && display && count < maxDepartures) {
            char line[21];
            snprintf(line, sizeof(line), "%-14s%6s", destination, display);
            depList[count++] = String(line);
          }
        }

        // Print and display the first two departures
        for (size_t i = 0; i < count; i++)
          Serial.println(depList[i]);

        u8g2.clearBuffer();
        u8g2.setDrawColor(1);
        u8g2.setFont(u8g2_font_10x20_tf);
        if (count > 0) u8g2.drawStr(2, 28, depList[0].c_str());
        if (count > 1) u8g2.drawStr(2, 60, depList[1].c_str());
        u8g2.sendBuffer();
      }
    }
  }
}
