#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <U8g2lib.h>
#include <SPI.h>

// WiFi credentials
const char* ssid = "guest";
const char* password = "87GreenBluebirds?";

// Web server and preferences
WebServer server(80);
Preferences prefs;

// Display pin setup
#define CLK 20
#define DATA 19
#define DC 18 
#define RESET 15
#define CS 14

// U8G2 display object
U8G2_SSD1322_NHD_256X64_F_4W_HW_SPI u8g2(U8G2_R0, CS, DC, RESET);

// SL selection info
unsigned long lastFetchTime = 0;
String savedId = "";
String savedName = "";

// HTML Web Interface
const char HTML_TEMPLATE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <title>SL Search</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
</head>
<body>
  <h2>Search SL Sites</h2>
  <input type="text" id="searchTerm" placeholder="Enter name...">
  <button onclick="search()">Search</button>
  <br><br>
  <select id="results"></select>
  <br><br>
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
          if (entry.name === defaultName && entry.id.toString() === defaultId) {
            option.selected = true;
          }
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
  </script>
</body>
</html>
)rawliteral";

void setup() {
  Serial.begin(115200);
  delay(100);

  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected!");
  Serial.print("ESP32 IP address: ");
  Serial.println(WiFi.localIP());

  // Setup display
  SPI.begin(CLK, -1, DATA, CS);  // MISO not used
  u8g2.setBusClock(25000000);    // 25 MHz
  u8g2.begin();
  u8g2.setContrast(128); 
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_ncenB08_tr);	// choose a suitable font
  u8g2.drawStr(0,10,"Connecting to WiFi");	// write something to the internal memory
  u8g2.sendBuffer();

  // Load saved preferences
  prefs.begin("slprefs", false);
  savedId = prefs.getString("id", "");
  savedName = prefs.getString("name", "");

  // Web server endpoints
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

        Serial.println("[INFO] Saved preferences:");
        Serial.println("  Name: " + savedName);
        Serial.println("  ID:   " + savedId);

        server.send(200, "text/plain", "Saved");
        return;
      }
    }
    server.send(400, "text/plain", "Invalid Data");
  });

  server.begin();
  Serial.println("Web server started");
  lastFetchTime = millis()+20000;  //first time quick update
}

void loop() {
  server.handleClient();

  if (millis() - lastFetchTime > 20000 && savedId.length() > 0) {
    lastFetchTime = millis();

    Serial.println("getting departure");

    String url = "http://transport.integration.sl.se/v1/sites/" + savedId + "/departures?transport=METRO&forecast=30";

    HTTPClient http;
    http.useHTTP10(true);
    http.begin(url);
    int httpCode = http.GET();

    if (httpCode == HTTP_CODE_OK) {
      WiFiClient* stream = http.getStreamPtr();
      char* buffer = (char*)malloc(26001);
      if (!buffer) {
        Serial.println("[ERROR] Failed to allocate 26K buffer");
        http.end();
        return;
      }

      int index = 0;
      while (stream->available() && index < 26000) {
        buffer[index++] = stream->read();
      }
      buffer[index] = '\0';

      StaticJsonDocument<26000> doc;
      DeserializationError err = deserializeJson(doc, buffer);
      free(buffer);
      http.end();

      if (err) {
        Serial.print("[ERROR] JSON parsing failed: ");
        Serial.println(err.c_str());
        return;
      }

      JsonArray departures = doc["departures"];
      if (departures.size() > 0) {
        Serial.println("[DEPARTURES]");

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

        for (size_t i = 0; i < count; i++) {
          Serial.printf("  %2d: %s\n", i + 1, depList[i].c_str());
        }

        // Display only first 2 entries using Unicode font, offset x=2
        u8g2.clearBuffer();
        u8g2.setFont(u8g2_font_10x20_tf);  // Supports åäö
        if (count > 0) u8g2.drawStr(2, 28, depList[0].c_str());  // Line 1
        if (count > 1) u8g2.drawStr(2, 60, depList[1].c_str());  // Line 2
        u8g2.sendBuffer();
      } else {
        Serial.println("[ERROR] 'departures' array is empty");
      }
    } else {
      Serial.printf("[ERROR] HTTP status %d fetching departures\n", httpCode);
    }
  }
}
