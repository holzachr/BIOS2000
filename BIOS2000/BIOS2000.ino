#include <Arduino.h>            // From Arduino
#include <queue>                // From Arduino

#include <Wire.h>               // From Arduino / ESP8266 board package
#include <LittleFS.h>           // From Arduino / ESP8266 board package
#include <ESP8266WiFi.h>        // From Arduino / ESP8266 board package

#include <PCF8574.h>            // https://github.com/RobTillaart/PCF8574
#include <ESPAsyncWebServer.h>  // https://github.com/ESP32Async/ESPAsyncWebServer
#include <ArduinoJson.h>        // https://github.com/bblanchon/ArduinoJson
#include <MFRC522_I2C.h>        // https://github.com/arozcan/MFRC522-I2C-Library
#include <FastLED.h>            // https://github.com/FastLED/FastLED



// --- Pin configuration ---
#define RFID_RESET_PIN     D0   // RFID module reset pin. Required for library, but not used.
#define I2C_SDA_PIN        D1   // I2C bus SDA
#define I2C_SCL_PIN        D2   // I2C bus SCL
#define LED_STRIP_DATA_PIN D4   // WS2812B LED data  
#define ACTIVITY_LED_PIN   D5   // Activity LED
#define SHOT_BUTTON_PIN    D6   // Instant shot button, needs internal pull-up

// --- I2C bus device configuration ---
#define I2C_ADDRESS_PCF8474_IO_EXTENDER_1  0x20
#define I2C_ADDRESS_PCF8474_IO_EXTENDER_2  0x21
#define I2C_ADDRESS_MFRC522_RFID_READER    0x28

// --- WiFi AP configuration ---
const char *SSID = "PromilleProfi";
const char *WPA_PASSWORD = "12345679";

// IO extenders
PCF8574 Extender1(I2C_ADDRESS_PCF8474_IO_EXTENDER_1, &Wire);
PCF8574 Extender2(I2C_ADDRESS_PCF8474_IO_EXTENDER_2, &Wire);

// Pump hardware configuration struct
typedef struct
{
  PCF8574 * pPCF;               // Pointer to PCF8574 IO extender
  byte pin1;                    // Pin 1 of H-bridge, pin number on IO extender [0-7]
  byte pin2;                    // Pin 2 of H-bridge, pin number on IO extender [0-7]
  float mlPerSec;               // [ml/s] Flow rate
} tPumpHwConfig;

// --- Pump configuration ---
#define PUMP_COUNT              (6) // Number of pumps
#define ANTI_DRIP_REVERSE_MS   (30) // [ms] How long to pull back liquid to avoid dripping
String Ingredients[PUMP_COUNT];     
tPumpHwConfig PumpHwConfig[PUMP_COUNT] = 
{
// IO Extender  Pin1  Pin2  ml/s
  {&Extender1,  0,    1,    3.1000},   // Pump 0
  {&Extender1,  2,    3,    1.9024},   // Pump 1
  {&Extender1,  4,    5,    1.6946},   // ...
  {&Extender1,  6,    7,    2.6083},   // ...
  {&Extender2,  0,    1,    1.9389},   // ...
  {&Extender2,  2,    3,    1.8174},   // Pump 5
};

// --- Instant shot push button ---
#define SHOT_AMOUNT_ML     20   // [ml] Amount to dispense for instant shot
const char RFID_CARD_UIDs[PUMP_COUNT][7] =
{
  // Card UID
  {0x04, 0x31, 0xDB, 0x03, 0xC1, 0x2A, 0x81},  // This card UID selects pump 0
  {0x04, 0x32, 0xDB, 0x03, 0xC1, 0x2A, 0x81},  // This card UID selects pump 1
  {0x04, 0x33, 0xDB, 0x03, 0xC1, 0x2A, 0x81},  // This card UID selects pump 2
  {0x04, 0x34, 0xDB, 0x03, 0xC1, 0x2A, 0x81},  // This card UID selects pump 3
  {0x04, 0x28, 0xDB, 0x03, 0xC1, 0x2A, 0x81},  // This card UID selects pump 4
  {0x04, 0x29, 0xDB, 0x03, 0xC1, 0x2A, 0x81},  // This card UID selects pump 5
};



AsyncWebServer server(80);  // Web server
AsyncWebSocket ws("/ws");   // WebSocket endpoint under "/ws"

// Struct for mix command
struct MixCommand 
{
  int pump;
  int amount;
};

// Queue for mix commands
std::queue<MixCommand> WorkQueue;

// Pump operation state struct
typedef struct
{
  unsigned long startTimeMs;
  unsigned long durationMs;  
  bool isMixing;
} tPumpState;

// Pump state
tPumpState PumpState[PUMP_COUNT] = {0, 0, false};

// RFID reader
MFRC522 mfrc522(I2C_ADDRESS_MFRC522_RFID_READER, RFID_RESET_PIN);

// --- LED strip configuration ---
#define LED_STRIP_TYPE           WS2812B
#define LED_STRIP_NUM_LEDS       34
#define LED_STRIP_COLOR_ORDER    GRB
#define LED_STRIP_BRIGHTNESS     90             // [0..255] Brightness
#define LED_STRIP_DELAY_IDLE_MS  50             // [ms] Time from change to change when idle, higher = slower
#define LED_STRIP_DELAY_BUSY_MS  6              // [ms] Time from change to change when mixing, higher = slower
CRGB LED_STRIP_LEDS[LED_STRIP_NUM_LEDS];
CRGBPalette16 LedStripPalette;
TBlendType    LedStripBlending;



void loadIngredientsFromFile() 
{
  File file = LittleFS.open("/ingredients.json", "r");
  if (!file) 
  {
    Serial.println("Error: ingredients.json not found! Setting default values...");
       
    // Default values
    for (int p = 0; p < PUMP_COUNT; p++)
      Ingredients[p] = "Empty";

    return;
  }

  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, file);
  file.close();

  if (error) 
  {
    Serial.println("Error reading ingredients.json! Setting default values...");
    return;
  }

  // Apply values from JSON
  for (int i = 0; i < PUMP_COUNT; i++) 
  {
    Ingredients[i] = doc["pumps"][String(i + 1)].as<String>();
  }

  Serial.println("Ingredients configuration loaded:");
  for (int i = 0; i < PUMP_COUNT; i++) 
  {
    Serial.printf("Pumpe %d: %s\n", i + 1, Ingredients[i].c_str());
  }
} // loadIngredientsFromFile()

void saveIngredientsToFile() 
{
  StaticJsonDocument<256> doc;
    
  for (int i = 0; i < PUMP_COUNT; i++) 
  {
    doc["pumps"][String(i + 1)] = Ingredients[i];
  }

  File file = LittleFS.open("/ingredients.json", "w");
  if (!file) 
  {
    Serial.println("Error saving ingredients configuration!");
    return;
  }

  serializeJson(doc, file);
  file.close();
  Serial.println("Ingredients configuration saved successfully!");
} // saveIngredientsToFile()

String loadRecipesFromFile() 
{
  if (!LittleFS.exists("/recipes.json")) 
  {
    return "{}";  // Return empty JSON if file does not exist
  }
  File file = LittleFS.open("/recipes.json", "r");
  if (!file) 
  {
    return "{}";
  }
  String json = file.readString();
  file.close();
  return json;
} // loadRecipesFromFile()

void saveRecipesToFile(String json) 
{
  File file = LittleFS.open("/recipes.json", "w");
  if (!file) 
  {
    Serial.println("Error saving recipes!");
    return;
  }
  file.print(json);
  file.close();
} // saveRecipesToFile()

void handleSaveRecipe(AsyncWebServerRequest *request) 
{
  if (request->hasParam("data", true)) 
  {
    String json = request->getParam("data", true)->value();
    saveRecipesToFile(json);
    request->send(200, "text/plain", "Saved");
  } 
  else 
  {
    request->send(400, "text/plain", "Missing data");
  }
} // handleSaveRecipe()

// Send message to all WebSocket clients
void notifyClients(String message) 
{
    ws.textAll(message);
}

void onWebSocketEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) 
{
    if (type == WS_EVT_CONNECT) 
    {
        Serial.println("WebSocket client connected!");
    }
} // onWebSocketEvent()

void handleSaveIngredients(AsyncWebServerRequest *request) 
{
  for (int i = 0; i < PUMP_COUNT; i++) 
  {
    String paramName = "p" + String(i + 1);
    if (request->hasParam(paramName)) 
    {
      Ingredients[i] = request->getParam(paramName)->value();
    }
  }

  saveIngredientsToFile();

  // Send WebSocket notification to clients
  notifyClients("update");

  request->send(200, "text/plain", "Ingredients configuration saved!");
} // handleSaveIngredients()

void handleGetIngredients(AsyncWebServerRequest *request) 
{
  StaticJsonDocument<512> doc;
  doc["pump_count"] = PUMP_COUNT;

  for (int i = 0; i < PUMP_COUNT; i++) 
  {
    doc["pumps"][String(i + 1)] = Ingredients[i];  // Ingredients-to-pump assignment
  }

  String jsonResponse;
  serializeJson(doc, jsonResponse);
    
  request->send(200, "application/json", jsonResponse);
} // handleGetIngredients()

void handleGetCalibration(AsyncWebServerRequest *request) 
{
  String jsonString = "[";
  for (int i = 0; i < PUMP_COUNT; i++) 
  {
    jsonString += String(PumpHwConfig[i].mlPerSec);
    if (i < PUMP_COUNT - 1) 
      jsonString += ", ";
  }
  jsonString += "]";
  request->send(200, "application/json", jsonString);
} // handleGetCalibration()

void pumpStop(int pump)
{
  if (pump >= PUMP_COUNT)
    return;

  PumpHwConfig[pump].pPCF->write(PumpHwConfig[pump].pin1, LOW);
  PumpHwConfig[pump].pPCF->write(PumpHwConfig[pump].pin2, LOW);
} // pumpStop()

void pumpForward(int pump)
{
  if (pump >= PUMP_COUNT)
    return;

  PumpHwConfig[pump].pPCF->write(PumpHwConfig[pump].pin1, LOW);
  PumpHwConfig[pump].pPCF->write(PumpHwConfig[pump].pin2, LOW);
  
  // Safety delay to not short-circuit the H-bridge
  delay(2); 
  
  PumpHwConfig[pump].pPCF->write(PumpHwConfig[pump].pin1, HIGH);
  PumpHwConfig[pump].pPCF->write(PumpHwConfig[pump].pin2, LOW);
} // pumpForward()

void pumpReverse(int pump)
{
  if (pump >= PUMP_COUNT)
    return;

  PumpHwConfig[pump].pPCF->write(PumpHwConfig[pump].pin1, LOW);
  PumpHwConfig[pump].pPCF->write(PumpHwConfig[pump].pin2, LOW);
  
  // Safety delay to not short-circuit the H-bridge
  delay(2); 
  
  PumpHwConfig[pump].pPCF->write(PumpHwConfig[pump].pin1, LOW);
  PumpHwConfig[pump].pPCF->write(PumpHwConfig[pump].pin2, HIGH);
} // pumpReverse()

bool isMixing()
{
  for (int p = 0; p < PUMP_COUNT; p++)
  {
    if (PumpState[p].isMixing)
    {
      return true;
    }
  }

  return false;
} // isMixing()

void mixCocktail(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) 
{
  DynamicJsonDocument doc(512);
  DeserializationError error = deserializeJson(doc, data, len);

  if (error) 
  {
    request->send(400, "application/json", "{\"status\": \"JSON error\"}");
    return;
  }

  if (isMixing())
  {
    request->send(400, "application/json", "{\"status\": \"Busy\"}");
    return;
  }

  // Parse recipe and enqueue pump work
  for (JsonObject elem : doc.as<JsonArray>()) 
  {
    int pump = elem["pump"];
    int amount = elem["amount"];        
    WorkQueue.push({pump - 1, amount});  // Pumps count from 0 in C++
  }

  request->send(200, "application/json", "{\"status\": \"Mixing cocktail\"}");
}

void handleStop(AsyncWebServerRequest *request) 
{
  for (int p = 0; p < PUMP_COUNT; p++)
  {
    if (PumpState[p].isMixing)
    {
      PumpState[p].durationMs = 0;
    }
  }
  
  request->send(200, "text/plain", "Stopping");
} // mixCocktail()

void showActivityLed()
{
  static unsigned long startTimeMs = 0;
  static unsigned long durationMs = 0;
  static bool ledOn = false;
  static bool lastIsMixing = false;

  if (isMixing() && !lastIsMixing)        // Mixing just started:
  {
    lastIsMixing = true;
    digitalWrite(ACTIVITY_LED_PIN, HIGH);   
    durationMs = random(300);
    ledOn = true;
    startTimeMs = millis();     
  }
  else if (isMixing() && lastIsMixing)    // Still mixing:
  {
    if (ledOn && (millis() - startTimeMs >= durationMs))
    {
      digitalWrite(ACTIVITY_LED_PIN, LOW);    // Turn off for random time 0..80 ms
      durationMs = random(200);
      ledOn = false;
      startTimeMs = millis();
    }
    else if (!ledOn && (millis() - startTimeMs >= durationMs))
    {
      digitalWrite(ACTIVITY_LED_PIN, HIGH);   // Turn on for random time 0..300 ms
      durationMs = random(300);
      ledOn = true;
      startTimeMs = millis();
    }
    else
    {
      // Stay in on/off state
    }
  }
  else if (!isMixing() && lastIsMixing)   // Mixing just finished:
  {
    // Turn everything off
    digitalWrite(ACTIVITY_LED_PIN, LOW);      // Turn off
    ledOn = false;
  }
  else
  {
    // Never the case
  }
} // showActivityLed()

void showRFIDReaderDetails() 
{
  // Get the MFRC522 software version
  byte version = mfrc522.PCD_ReadRegister(mfrc522.VersionReg);
  Serial.printf("MFRC522 software version: 0x%2.2X\n", version);

  // When 0x00 or 0xFF is returned, communication probably failed
  if ((version == 0x00) || (version == 0xFF)) 
  {
    Serial.println(F("WARNING: Communication failure, is the MFRC522 properly connected?"));
  }
} // showRFIDReaderDetails() 
  
void readRFIDCardAndDispenseInstantShot()
{
  if (!isMixing() && (digitalRead(SHOT_BUTTON_PIN) == LOW))
  {
    // Look for new cards, and select one if present
    if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) 
    {
      int pump = PUMP_COUNT;
      bool pumpFound = false;

      Serial.println("RFID card found.");

      for (int p = 0; p < PUMP_COUNT; p++)
      {
        for (int i = 0; i < mfrc522.uid.size; i++)
        {
          if (RFID_CARD_UIDs[p][i] != mfrc522.uid.uidByte[i])
          {
            break;
          }
          else if (i == (mfrc522.uid.size-1) && RFID_CARD_UIDs[p][i] == mfrc522.uid.uidByte[i])
          {
            pumpFound = true;
            pump = p;
          }
        }
      }

      if (pumpFound)   
      {
        Serial.printf("Dispensing shot from pump %d\n", pump);
        WorkQueue.push({pump, SHOT_AMOUNT_ML});
      }
    }
  }
} // readRFIDCardAndDispenseInstantShot()

void setLedStrip()
{
  static unsigned long startTimeMs = millis();
  static unsigned long durationMs = LED_STRIP_DELAY_IDLE_MS;
  static bool lastIsMixing = false;

  static uint8_t startIndex = 0;
  if ((millis() - startTimeMs) >= durationMs)
  {
    startTimeMs = millis();
    startIndex = startIndex + 1; /* motion speed */
    FillLEDsFromPaletteColors(startIndex, !isMixing());
  }  
  
  FastLED.show();

  if (isMixing() && !lastIsMixing)        // Mixing just started:
  {
    lastIsMixing = true;
    durationMs = LED_STRIP_DELAY_BUSY_MS; 
    LedStripSetMixingPalette();
  }
  else if (!isMixing() && lastIsMixing)   // Mixing just finished:
  {
    lastIsMixing = false;
    durationMs = LED_STRIP_DELAY_IDLE_MS; 
    LedStripSetIdlePalette();
  }
  else
  {
    // Never the case
  }
} // setLedStrip()

// This function sets the next LED strip state
void FillLEDsFromPaletteColors(uint8_t colorIndex, bool lightTheBase)
{ 
  // Set palette for all LEDs
  for (int i = 0; i < LED_STRIP_NUM_LEDS; i++) 
  {
    LED_STRIP_LEDS[i] = ColorFromPalette(LedStripPalette, colorIndex, LED_STRIP_BRIGHTNESS, LedStripBlending);
    colorIndex += 3;
  }

  if (lightTheBase)
  {
    // First and last LED pure white to light the case neutrally
    LED_STRIP_LEDS[0] = CRGB::White;
    LED_STRIP_LEDS[1] = CRGB::White;
    LED_STRIP_LEDS[LED_STRIP_NUM_LEDS-1] = CRGB::White;
  }
}

void LedStripSetIdlePalette()
{
  CRGB yellow = CHSV(HUE_YELLOW, 255, 255);
  CRGB orange = CHSV(HUE_ORANGE, 255, 255);
  CRGB red  = CHSV(HUE_RED, 255, 255);
    
  LedStripPalette = CRGBPalette16(
                                 yellow, orange, red, orange,
                                 yellow, orange, red, orange,
                                 yellow, orange, red, orange,
                                 yellow, orange, red, orange );

  LedStripBlending = LINEARBLEND;  
}

void LedStripSetMixingPalette()
{
  // This function fills the LED strip palette with totally random colors.
  for (int i = 0; i < 16; i++) 
  {
    LedStripPalette[i] = CHSV(random8(), 255, random8());
  }

  LedStripBlending = NOBLEND;
}

void setup() 
{
	Serial.begin(115200);

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

  pinMode(ACTIVITY_LED_PIN, OUTPUT);
  digitalWrite(ACTIVITY_LED_PIN, LOW);

  pinMode(SHOT_BUTTON_PIN, INPUT_PULLUP);

  mfrc522.PCD_Init();
  showRFIDReaderDetails();

  for (int p = 0; p < PUMP_COUNT; p++)
  {
    if (!PumpHwConfig[p].pPCF->begin(0x00))
    {
      Serial.println("could not initialize...");
    }
    if (!PumpHwConfig[p].pPCF->isConnected())
    {
      Serial.println("=> not connected");
      //while(1);
    }
  }

  // Start Access Point
  WiFi.softAP(SSID, WPA_PASSWORD);
  Serial.println("Access Point started");

  if (!LittleFS.begin()) 
  {
    Serial.println("Error loading filesystem");
    return;
  }
  else 
  {
    Serial.println("LittleFS loaded successfully");
  }

  Dir dir = LittleFS.openDir("/");
  while (dir.next()) 
  {
    Serial.println("Found file: " + dir.fileName());
  }

  loadIngredientsFromFile();

  // Cocktails user page
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) 
  {
    request->send(LittleFS, "/cocktails.html", "text/html");
  });
  server.on("/cocktails", HTTP_GET, [](AsyncWebServerRequest *request) 
  {
    request->send(LittleFS, "/cocktails.html", "text/html");
  });

  // Shots user page
  server.on("/shots", HTTP_GET, [](AsyncWebServerRequest *request) 
  {
    request->send(LittleFS, "/shots.html", "text/html");
  });
  
  // Admin page (Ingredients configuration)
  server.on("/zutaten", HTTP_GET, [](AsyncWebServerRequest *request) 
  {
    request->send(LittleFS, "/admin.html", "text/html");
  });
  server.serveStatic("/positions.png", LittleFS, "/positions.png");

  // Calibration page
  server.on("/calibration", HTTP_GET, [](AsyncWebServerRequest *request) 
  {
    request->send(LittleFS, "/calibration.html", "text/html");
  });

  // Load saved recipes JSON
  server.on("/getRecipes", HTTP_GET, [](AsyncWebServerRequest *request)
  {
    request->send(200, "application/json", loadRecipesFromFile());
  });

  // Show and upload files to LittleFS
  server.on("/files", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(LittleFS, "/upload.html", "text/html");
  });

  server.on("/saveRecipe", HTTP_POST, handleSaveRecipe);
  server.on("/getCalibration", HTTP_GET, handleGetCalibration);
  server.on("/getIngredients", HTTP_GET, handleGetIngredients);
  server.on("/saveIngredients", HTTP_GET, handleSaveIngredients);    
  server.on("/mixCocktail", HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL, mixCocktail);
  server.on("/stop", HTTP_POST, handleStop);
  
  // Upload file to LittleFS
  server.on(
    "/uploadFile", HTTP_POST,
    [](AsyncWebServerRequest *request) 
    {
      request->send(200, "text/plain", "Upload successful!");
    },
    [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) 
    {
      static File uploadFile;
      
      if (index == 0) 
      {
        Serial.printf("Upload started: %s\n", filename.c_str());
        uploadFile = LittleFS.open("/" + filename, "w");
      }

      if (uploadFile) 
      {
        uploadFile.write(data, len);
      }

      if (final) 
      {
        Serial.printf("Upload finished: %s (%u Bytes)\n", filename.c_str(), index + len);
        uploadFile.close();
      }
    }
  );

  // List LittleFS files
  server.on("/listFiles", HTTP_GET, [](AsyncWebServerRequest *request) {
    String output = "<h3>Inhalt von LittleFS:</h3><ul>";
    File root = LittleFS.open("/", "r");
    File file = root.openNextFile();
    while (file) 
    {
      output += "<li>" + String(file.name()) + " (" + String(file.size()) + " Bytes)</li>";
      file = root.openNextFile();
    }
    output += "</ul>";
    request->send(200, "text/html", output);
  });

  // Start WebSocket
  ws.onEvent(onWebSocketEvent);
  server.addHandler(&ws);

  // Start webserver  
  server.begin();

  // Start LED strip
  FastLED.addLeds<LED_STRIP_TYPE, LED_STRIP_DATA_PIN, LED_STRIP_COLOR_ORDER>(LED_STRIP_LEDS, LED_STRIP_NUM_LEDS).setCorrection(TypicalLEDStrip);
  FastLED.setBrightness(LED_STRIP_BRIGHTNESS);
  LedStripSetIdlePalette();
} // setup()

void loop() 
{
  // --- Handle new dispense commands ---
  // One pump is started per loop() run.
  // This is to distribute the startup current.
  if (!WorkQueue.empty())
  {
    MixCommand cmd = WorkQueue.front();
    Serial.printf("Request: Pump %d with %d ml\n", cmd.pump, cmd.amount);

    // Checks
    if (cmd.pump >= PUMP_COUNT)         // Invalid pump number
    {
      WorkQueue.pop();                  // Remove illegal command anyway
      return;
    }
    if (PumpState[cmd.pump].isMixing)   // Pump already busy
    {
      WorkQueue.pop();                  // Remove illegal command anyway
      return;
    }
    
    // Start dispensing
    PumpState[cmd.pump].durationMs = (int)(((float)cmd.amount * 1000) / (PumpHwConfig[cmd.pump].mlPerSec));

    Serial.printf("Pump %d forward for %d ms\n", cmd.pump, PumpState[cmd.pump].durationMs);

    PumpState[cmd.pump].startTimeMs = millis();
    pumpForward(cmd.pump);
    PumpState[cmd.pump].isMixing = true;
    
    WorkQueue.pop();                  // Remove successfully started item from work queue
  }

  // --- Watch and end ongoing dispenses ---
  for (int p = 0; p < PUMP_COUNT; p++)
  {
    if (PumpState[p].isMixing)
    {
      if (millis() - PumpState[p].startTimeMs >= PumpState[p].durationMs)
      {
        pumpStop(p);
        delay(5);
            
        // Pull back some liquid to avoid dripping
        pumpReverse(p);
        delay(ANTI_DRIP_REVERSE_MS);
        pumpStop(p);

        PumpState[p].isMixing = false;

        Serial.printf("Pump %d done.\n", p);
      }
    }
  }

  // --- Handle activity LED ---
  showActivityLed();

  // --- Watch instant shot button and RFID card reader ---
  readRFIDCardAndDispenseInstantShot();

  // --- Handle WebSocket clients ---
  ws.cleanupClients();

  // --- Handle LED strip ---
  setLedStrip();
} // loop()
