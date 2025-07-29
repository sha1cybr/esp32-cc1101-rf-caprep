#include <CC1101_ESP_Arduino.h>
#include <map>
#include <vector>
#include <string>
#include <WiFi.h>          // For WiFi connectivity
#include <WebServer.h>     // For the web server
#include <SPIFFS.h>        // For file system operations
#include <ArduinoJson.h>   // For JSON serialization/deserialization
#include <ESPmDNS.h>       // For mDNS functionality

// --- WiFi Credentials ---
const char* ssid = "YOUR_SSID";     // <<<<<<<<<<< CHANGE THIS
const char* password = "YOUR_PASSWORD"; // <<<<<<<<<<< CHANGE THIS

// --- ESP32 PINs ---
const int SPI_SCK = 18;
const int SPI_MISO = 19;
const int SPI_MOSI = 23;
const int SPI_CS = 5;
const int RADIO_INPUT_PIN = 25;   // TX pin
const int RADIO_OUTPUT_PIN = 26;  // RX pin

// --- Global Instances ---
CC1101 cc1101(SPI_SCK, SPI_MISO, SPI_MOSI, SPI_CS, RADIO_INPUT_PIN, RADIO_OUTPUT_PIN);
WebServer server(80); // Web server on port 80

// --- CustomBitBanger Definition ---
class CustomBitBanger {
private:
    int _pin;
    int _currentPinState;

public:
    CustomBitBanger() : _pin(-1), _currentPinState(LOW) {}

    void begin(int pin) {
        _pin = pin;
        pinMode(_pin, OUTPUT);
        digitalWrite(_pin, LOW);
        _currentPinState = LOW;
    }

    void transmit(const std::vector<int>& timings, int repetitions) {
        if (_pin == -1) {
            Serial.println("CustomBitBanger not initialized. Call begin() first.");
            return;
        }
        if (timings.empty()) {
            Serial.println("Warning: Attempted to transmit an empty signal.");
            return;
        }

        for (int r = 0; r < repetitions; r++) {
            for (size_t i = 0; i < timings.size(); i++) {
                digitalWrite(_pin, (r + i) % 2 == 0 ? HIGH : LOW); // Original alternating logic
                delayMicroseconds(timings[i]);
            }
            digitalWrite(_pin, LOW); // Ensure pin returns to LOW after transmission
            delay(10); // Small delay between repetitions
        }
    }
};

CustomBitBanger customBitBanger; // Instantiate our custom bit banger
// --- End CustomBitBanger Definition ---


// --- RF Signal Recording Variables ---
volatile long lastMicros = 0;
#define MAX_CHANGES 512
volatile int timings[MAX_CHANGES];
volatile int changeCount = 0;
volatile bool signalDetected = false;
volatile bool recordingInProgress = false; // Flag to control ISR

// Our main storage for signals
std::map<String, std::vector<int>> recordedSignals;

// --- SPIFFS File Path ---
const char* SIGNALS_FILE_PATH = "/signals.json";

// --- ISR for RF Signal Detection ---
ICACHE_RAM_ATTR void radioISR() {
  if (changeCount >= MAX_CHANGES || !recordingInProgress) return;
  long now = micros();
  int delta = now - lastMicros;
  lastMicros = now;

  timings[changeCount++] = delta;
  signalDetected = true;
}

// --- Signal Recording and Transmission Functions ---
String performRecordSignal(const String& name) {
  if (name.length() == 0) {
      return "Error: Signal name cannot be empty.";
  }
  if (recordingInProgress) {
      return "Error: Recording already in progress. Please wait.";
  }
  if (recordedSignals.count(name)) {
      return "Error: Signal '" + name + "' already exists. Please choose a different name or delete the existing one.";
  }

  cc1101.setRx();
  signalDetected = false;
  changeCount = 0;
  lastMicros = micros();
  recordingInProgress = true; // Enable recording in ISR

  attachInterrupt(digitalPinToInterrupt(RADIO_OUTPUT_PIN), radioISR, CHANGE);

  Serial.println("Listening for RF signal... Press remote now.");
  unsigned long recordStartTime = millis();
  const unsigned long RECORD_TIMEOUT_MS = 10000; // 10 second timeout

  while (!signalDetected && (millis() - recordStartTime < RECORD_TIMEOUT_MS)) {
    server.handleClient(); // Keep the web server responsive
    delay(1);
  }

  if (signalDetected) {
      delay(200); // Allow full sequence
  } else {
      Serial.println("No signal detected within timeout. Recording aborted.");
  }

  recordingInProgress = false; // Disable recording in ISR
  cc1101.setIdle();
  detachInterrupt(digitalPinToInterrupt(RADIO_OUTPUT_PIN));

  if (changeCount < 20) {
    return "Error: Too few changes detected. Signal not saved. Try again.";
  }

  Serial.printf("Signal captured (%d timings). Saving as '%s'\n", changeCount, name.c_str());
  std::vector<int> signal(timings, timings + changeCount);
  recordedSignals[name] = signal;

  memset((void*)timings, 0, sizeof(timings));
  changeCount = 0;

  return "Success: Signal '" + name + "' captured (" + String(signal.size()) + " timings).";
}


void transmitSignal(const String& name) {
  if (recordedSignals.find(name) == recordedSignals.end()) {
    Serial.println("Signal not found.");
    return;
  }

  const std::vector<int>& signal = recordedSignals[name];

  cc1101.setTx();
  customBitBanger.begin(RADIO_INPUT_PIN);

  Serial.printf("Transmitting '%s' (%d timings)...\n", name.c_str(), signal.size());
  customBitBanger.transmit(signal, 1); // Transmit 5 times

  cc1101.setIdle();
  Serial.println("Transmission complete.");
}

void clearSignals() {
  recordedSignals.clear();
  Serial.println("All stored signals cleared.");
}

// --- NEW: Rename Signal Function ---
String renameSignal(const String& oldName, const String& newName) {
    if (oldName.length() == 0 || newName.length() == 0) {
        return "Error: Old and new signal names cannot be empty.";
    }
    if (oldName == newName) {
        return "Error: New name is the same as the old name.";
    }
    
    // Check if the old signal exists
    auto it = recordedSignals.find(oldName);
    if (it == recordedSignals.end()) {
        return "Error: Signal '" + oldName + "' not found.";
    }

    // Check if the new name already exists
    if (recordedSignals.count(newName)) {
        return "Error: Signal '" + newName + "' already exists. Choose a different name.";
    }

    // Perform the rename: copy data to new key, then erase old key
    recordedSignals[newName] = it->second; // Copy vector to new key
    recordedSignals.erase(it);             // Erase old key

    Serial.printf("Signal '%s' renamed to '%s'.\n", oldName.c_str(), newName.c_str());
    return "Success: Signal '" + oldName + "' renamed to '" + newName + "'.";
}

// --- JSON & SPIFFS Functions ---

// Convert recordedSignals map to JSON string
String serializeSignalsToJson() {
    DynamicJsonDocument doc(4096); // Adjust size as needed, 4KB is a reasonable start

    for (const auto& pair : recordedSignals) {
        JsonArray signalArray = doc.createNestedArray(pair.first);
        for (int timing : pair.second) {
            signalArray.add(timing);
        }
    }

    String jsonString;
    serializeJson(doc, jsonString);
    return jsonString;
}

// Parse JSON string into recordedSignals map
bool deserializeJsonToSignals(const String& jsonString) {
    DynamicJsonDocument doc(4096); // Must match or be larger than serialization buffer

    DeserializationError error = deserializeJson(doc, jsonString);

    if (error) {
        Serial.print(F("deserializeJson() failed: "));
        Serial.println(error.c_str());
        return false;
    }

    recordedSignals.clear(); // Clear existing signals before loading new ones

    for (JsonPair pair : doc.as<JsonObject>()) {
        String name = pair.key().c_str();
        JsonArray timingsArray = pair.value().as<JsonArray>();
        
        std::vector<int> signalTimings;
        for (int timing : timingsArray) {
            signalTimings.push_back(timing);
        }
        recordedSignals[name] = signalTimings;
    }
    return true;
}

// Save signals to SPIFFS
bool saveSignalsToFile() {
  String json = serializeSignalsToJson();
  File file = SPIFFS.open(SIGNALS_FILE_PATH, FILE_WRITE);
  if (!file) {
    Serial.println("Failed to open file for writing.");
    return false;
  }
  if (file.print(json)) {
    Serial.println("Signals saved to file.");
    file.close();
    return true;
  } else {
    Serial.println("Failed to write to file.");
    file.close();
    return false;
  }
}

// Load signals from SPIFFS
bool loadSignalsFromFile() {
  if (!SPIFFS.exists(SIGNALS_FILE_PATH)) {
    Serial.println("Signals file not found. Starting with empty signals.");
    recordedSignals.clear();
    return false;
  }

  File file = SPIFFS.open(SIGNALS_FILE_PATH, FILE_READ);
  if (!file) {
    Serial.println("Failed to open file for reading.");
    return false;
  }

  String jsonString = "";
  while (file.available()) {
    jsonString += (char)file.read();
  }
  file.close();

  if (deserializeJsonToSignals(jsonString)) {
    Serial.println("Signals loaded from file.");
    return true;
  } else {
    Serial.println("Failed to parse signals from file. Data might be corrupted.");
    recordedSignals.clear(); // Clear potentially corrupt data
    return false;
  }
}

// --- Web Server Handlers ---

// Main HTML page for the web UI
void handleRoot() {
  String html = "";
  html += "<!DOCTYPE html>";
  html += "<html>";
  html += "<head>";
  html += "    <title>ESP32 RF Controller</title>";
  html += "    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">";
  html += "    <style>";
  html += "        body { font-family: Arial, sans-serif; margin: 20px; background: #f0f0f0; }";
  html += "        .container { max-width: 800px; margin: 0 auto; background: white; padding: 20px; border-radius: 10px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }";
  html += "        h1 { color: #333; text-align: center; }";
  html += "        .section { margin: 20px 0; padding: 15px; border: 1px solid #ddd; border-radius: 5px; }";
  html += "        .section h3 { margin-top: 0; color: #555; }";
  html += "        button { background: #4CAF50; color: white; padding: 10px 15px; border: none; border-radius: 4px; cursor: pointer; margin: 5px; }";
  html += "        button.red { background: #f44336; }";
  html += "        button.red:hover { background: #da190b; }";
  html += "        button.blue { background: #2196F3; }"; // New style for Rename button
  html += "        button.blue:hover { background: #0b7dda; }";
  html += "        button:hover { background: #45a049; }";
  html += "        input[type=\"text\"] { padding: 8px; margin: 5px; border: 1px solid #ddd; border-radius: 4px; width: calc(100% - 130px); }";
  html += "        .signal-list { margin-top: 10px; }";
  html += "        .signal-item { display: flex; justify-content: space-between; align-items: center; padding: 10px; margin: 5px 0; background: #f9f9f9; border-radius: 5px; flex-wrap: wrap; }";
  html += "        .signal-item span { flex-basis: 50%; }"; // Adjusted for more buttons
  html += "        .signal-item div { flex-basis: 48%; text-align: right; }"; // Adjusted for more buttons
  html += "        @media (max-width: 600px) { .signal-item { flex-direction: column; align-items: flex-start; } .signal-item span, .signal-item div { flex-basis: 100%; text-align: left; } }";
  html += "        .status { margin: 10px 0; padding: 10px; border-radius: 4px; }";
  html += "        .status.success { background: #d4edda; color: #155724; border: 1px solid #c3e6cb; }";
  html += "        .status.error { background: #f8d7da; color: #721c24; border: 1px solid #f5c6cb; }";
  html += "        .status.info { background: #d1ecf1; color: #0c5460; border: 1px solid #bee5eb; }";
  html += "        #recordingStatus { margin-top: 10px; font-weight: bold; color: #0c5460; }";
  html += "    </style>";
  html += "</head>";
  html += "<body>";
  html += "    <div class=\"container\">";
  html += "        <h1>ESP32 RF Controller</h1>";
  html += "        ";
  html += "        <div class=\"section\">";
  html += "            <h3>📡 Record New Signal</h3>";
  html += "            <input type=\"text\" id=\"recordName\" placeholder=\"Enter signal name\" />";
  html += "            <button onclick=\"startRecording()\">Record Signal</button>";
  html += "            <div id=\"recordingStatus\"></div>";
  html += "        </div>";
  html += "        ";
  html += "        <div class=\"section\">";
  html += "            <h3>Actions</h3>";
  html += "            <button onclick=\"saveSignals()\">Save Signals</button>";
  html += "            <button onclick=\"loadSignals()\">Load Signals</button>";
  html += "            <button class=\"red\" onclick=\"clearAllSignals()\">Clear All Signals</button>";
  html += "            <button class=\"blue\" onclick=\"window.open('/dump_signals', '_blank')\">Show All Signals</button>"; // ADDED BUTTON
  html += "        </div>";
  html += "        ";
  html += "        <div class=\"section\">";
  html += "            <h3>Stored Signals</h3>";
  html += "            <div id=\"signalsList\" class=\"signal-list\"></div>";
  html += "        </div>";
  html += "        ";
  html += "        <div id=\"statusMessage\"></div>";
  html += "    </div>";

  html += "    <script>";
  html += "        function showStatus(message, type) {";
  html += "            if (!type) type = 'info';";
  html += "            const statusDiv = document.getElementById('statusMessage');";
  html += "            statusDiv.innerHTML = '<div class=\\\"status ' + type + '\\\">' + message + '</div>';";
  html += "            setTimeout(function() { statusDiv.innerHTML = ''; }, 3000);"; // Clear status after 3 seconds
  html += "        }";

  html += "        function startRecording() {";
  html += "            const name = document.getElementById('recordName').value.trim();";
  html += "            if (!name) {";
  html += "                showStatus('Please enter a signal name to record.', 'error');";
  html += "                return;";
  html += "            }";
  html += "            ";
  html += "            document.getElementById('recordingStatus').innerHTML = 'Listening... Press remote button now! (Timeout 10s)';";
  html += "            fetch('/record?name=' + encodeURIComponent(name), {method: 'POST'})";
  html += "                .then(function(response) { return response.text(); })";
  html += "                .then(function(data) {";
  html += "                    document.getElementById('recordingStatus').innerHTML = '';";
  html += "                    const type = data.includes('Success') ? 'success' : 'error';";
  html += "                    showStatus(data, type);";
  html += "                    if (type === 'success') {";
  html += "                        document.getElementById('recordName').value = '';";
  html += "                        refreshSignals();"; // Refresh list after successful record
  html += "                    }";
  html += "                })";
  html += "                .catch(function(error) {";
  html += "                    document.getElementById('recordingStatus').innerHTML = '';";
  html += "                    showStatus('Recording request failed: ' + error, 'error');";
  html += "                });";
  html += "        }";


  html += "        function transmitSignal(name) {";
  html += "            showStatus('Transmitting ' + name + '...', 'info');";
  html += "            fetch('/transmit?name=' + encodeURIComponent(name), {method: 'POST'})";
  html += "                .then(function(response) { return response.text(); })";
  html += "                .then(function(data) { showStatus(data, 'success'); })";
  html += "                .catch(function(error) { showStatus('Transmission failed: ' + error, 'error'); });";
  html += "        }";

  html += "        function deleteSignal(name) {";
  html += "            if (!confirm('Are you sure you want to delete ' + name + '?')) return;";
  html += "            showStatus('Deleting ' + name + '...', 'info');";
  html += "            fetch('/delete?name=' + encodeURIComponent(name), {method: 'POST'})";
  html += "                .then(function(response) { return response.text(); })";
  html += "                .then(function(data) { showStatus(data, 'success'); refreshSignals(); })";
  html += "                .catch(function(error) { showStatus('Deletion failed: ' + error, 'error'); });";
  html += "        }";

  html += "        function renameSignal(oldName) {";
  html += "            const newName = prompt('Enter new name for ' + oldName + ':');";
  html += "            if (newName === null || newName.trim() === '') {";
  html += "                showStatus('Rename cancelled or new name is empty.', 'info');";
  html += "                return;";
  html += "            }";
  html += "            const trimmedNewName = newName.trim();";
  html += "            if (trimmedNewName === oldName) {";
  html += "                showStatus('New name is the same as the old name.', 'info');";
  html += "                return;";
  html += "            }";
  html += "            ";
  html += "            showStatus('Renaming ' + oldName + ' to ' + trimmedNewName + '...', 'info');";
  html += "            fetch('/rename?old=' + encodeURIComponent(oldName) + '&new=' + encodeURIComponent(trimmedNewName), {method: 'POST'})";
  html += "                .then(function(response) { return response.text(); })";
  html += "                .then(function(data) {";
  html += "                    const type = data.includes('Success') ? 'success' : 'error';";
  html += "                    showStatus(data, type);";
  html += "                    if (type === 'success') {";
  html += "                        refreshSignals();"; // Refresh list after successful rename
  html += "                    }";
  html += "                })";
  html += "                .catch(function(error) { showStatus('Rename failed: ' + error, 'error'); });";
  html += "        }";


  html += "        function saveSignals() {";
  html += "            showStatus('Saving signals...', 'info');";
  html += "            fetch('/save', {method: 'POST'})";
  html += "                .then(function(response) { return response.text(); })";
  html += "                .then(function(data) { showStatus(data, 'success'); })";
  html += "                .catch(function(error) { showStatus('Save failed: ' + error, 'error'); });";
  html += "        }";

  html += "        function loadSignals() {";
  html += "            showStatus('Loading signals...', 'info');";
  html += "            fetch('/load', {method: 'POST'})";
  html += "                .then(function(response) { return response.text(); })";
  html += "                .then(function(data) { showStatus(data, 'success'); refreshSignals(); })";
  html += "                .catch(function(error) { showStatus('Load failed: ' + error, 'error'); });";
  html += "        }";

  html += "        function clearAllSignals() {";
  html += "            if (!confirm('Are you sure you want to clear ALL signals? This cannot be undone from the UI.')) return;";
  html += "            showStatus('Clearing all signals...', 'info');";
  html += "            fetch('/clearall', {method: 'POST'})";
  html += "                .then(function(response) { return response.text(); })";
  html += "                .then(function(data) { showStatus(data, 'success'); refreshSignals(); })";
  html += "                .catch(function(error) { showStatus('Clear all failed: ' + error, 'error'); });";
  html += "        }";

  html += "        function refreshSignals() {";
  html += "            fetch('/list')";
  html += "                .then(function(response) { return response.json(); })";
  html += "                .then(function(signals) {";
  html += "                    const listDiv = document.getElementById('signalsList');";
  html += "                    if (signals.length === 0) {";
  html += "                        listDiv.innerHTML = '<p>No signals stored.</p>';";
  html += "                        return;";
  html += "                    }";
  html += "                    ";
  html += "                    let html = '';";
  html += "                    signals.forEach(function(signal) {";
  html += "                        html += '<div class=\\\"signal-item\\\">' +";
  html += "                               '<span><strong>' + signal.name + '</strong> (' + signal.size + ' timings)</span>' +";
  html += "                               '<div>' +";
  html += "                               '<button onclick=\\\"transmitSignal(\\\'' + signal.name + '\\\')\\\">Transmit</button>' +";
  html += "                               '<button class=\\\"blue\\\" onclick=\\\"renameSignal(\\\'' + signal.name + '\\\')\\\">Rename</button>' +";
  html += "                               '<button class=\\\"red\\\" onclick=\\\"deleteSignal(\\\'' + signal.name + '\\\')\\\">Delete</button>' +";
  html += "                               '</div></div>';";
  html += "                    });";
  html += "                    listDiv.innerHTML = html;";
  html += "                })";
  html += "                .catch(function(error) { showStatus('Failed to load signals for UI: ' + error, 'error'); });";
  html += "        }";

  html += "        // Auto-refresh signals list on page load";
  html += "        refreshSignals();";
  html += "    </script>";
  html += "</body>";
  html += "</html>";
  server.send(200, "text/html", html);
}

// Handles a POST request to start recording a new signal
void handleRecord() {
  if (!server.hasArg("name")) {
    server.send(400, "text/plain", "Error: Missing signal name in request.");
    return;
  }
  
  String name = server.arg("name");
  String result = performRecordSignal(name); // Call the core record logic
  
  if (result.startsWith("Success")) {
      // If recording was successful, save to file immediately
      if (saveSignalsToFile()) {
          server.send(200, "text/plain", result + " Saved to file.");
      } else {
          server.send(500, "text/plain", result + " Failed to save to file.");
      }
  } else {
      server.send(400, "text/plain", result); // Send back the error message
  }
}

// Handles a POST request to transmit a selected signal
void handleTransmit() {
  if (!server.hasArg("name")) {
    server.send(400, "text/plain", "Error: Missing signal name.");
    return;
  }
  String name = server.arg("name");
  
  if (recordedSignals.find(name) == recordedSignals.end()) {
    server.send(404, "text/plain", "Error: Signal '" + name + "' not found.");
    return;
  }
  
  transmitSignal(name); // Use the existing transmit function
  server.send(200, "text/plain", "Transmission of '" + name + "' initiated.");
}

// Handles a GET request to list all stored signals in JSON format
void handleList() {
  String response = "[";
  bool first = true;
  
  for (const auto& pair : recordedSignals) {
    if (!first) response += ",";
    response += "{\"name\":\"" + pair.first + "\",\"size\":" + String(pair.second.size()) + "}";
    first = false;
  }
  
  response += "]";
  server.send(200, "application/json", response);
}

// Handles saving signals to file
void handleSave() {
    if (saveSignalsToFile()) {
        server.send(200, "text/plain", "Signals successfully saved to file!");
    } else {
        server.send(500, "text/plain", "Failed to save signals to file.");
    }
}

// Handles loading signals from file
void handleLoad() {
    if (loadSignalsFromFile()) {
        server.send(200, "text/plain", "Signals successfully loaded from file!");
    } else {
        server.send(500, "text/plain", "Failed to load signals from file or file not found.");
    }
}

// Handles deleting a single signal
void handleDeleteSignal() {
    if (!server.hasArg("name")) {
        server.send(400, "text/plain", "Error: Missing signal name to delete.");
        return;
    }
    String name = server.arg("name");
    
    if (recordedSignals.erase(name)) { // erase returns 1 if element was found and erased, 0 otherwise
        Serial.printf("Signal '%s' deleted from memory.\n", name.c_str());
        // Immediately save the changes to file
        if (saveSignalsToFile()) {
            server.send(200, "text/plain", "Signal '" + name + "' deleted and changes saved.");
        } else {
            server.send(500, "text/plain", "Signal '" + name + "' deleted from memory, but failed to save changes to file.");
        }
    } else {
        server.send(404, "text/plain", "Error: Signal '" + name + "' not found for deletion.");
    }
}

// --- NEW: Handle Rename Signal ---
void handleRenameSignal() {
    if (!server.hasArg("old") || !server.hasArg("new")) {
        server.send(400, "text/plain", "Error: Missing old or new signal name for rename.");
        return;
    }
    String oldName = server.arg("old");
    String newName = server.arg("new");

    String result = renameSignal(oldName, newName); // Call the core rename logic

    if (result.startsWith("Success")) {
        // If rename was successful, save to file immediately
        if (saveSignalsToFile()) {
            server.send(200, "text/plain", result + " Saved to file.");
        } else {
            server.send(500, "text/plain", result + " Failed to save to file.");
        }
    } else {
        server.send(400, "text/plain", result); // Send back the error message
    }
}

// Handles clearing all signals
void handleClearAll() {
    clearSignals(); // Use existing clear function
    if (saveSignalsToFile()) { // Save the empty state
        server.send(200, "text/plain", "All signals cleared and file updated.");
    } else {
        server.send(500, "text/plain", "All signals cleared from memory, but failed to update file.");
    }
}

// Handles dumping all signals as plain text/JSON for debugging/analysis
void handleDumpSignals() {
    String output = "";
    if (recordedSignals.empty()) {
        output = "No signals recorded yet.";
    } else {
        output += "--- All Recorded Signals ---\n\n";
        for (const auto& pair : recordedSignals) {
            output += "Signal Name: " + pair.first + "\n";
            output += "  Timings (" + String(pair.second.size()) + "): [";
            bool firstTiming = true;
            for (int timing : pair.second) {
                if (!firstTiming) {
                    output += ", ";
                }
                output += String(timing);
                firstTiming = false;
            }
            output += "]\n\n";
        }
        output += "--------------------------\n";
    }
    server.send(200, "text/plain", output); // Send as plain text
}


// --- Setup Function ---
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n--- CC1101 RF Controller (Web UI Only) ---");

  // Initialize CC1101 module
  Serial.println("Initializing CC1101...");
  cc1101.init();
  Serial.printf("CC1101 Partnum: 0x%02x, Version: 0x%02x\n", cc1101.getPartnum(), cc1101.getVersion());
  cc1101.setMHZ(433.92);
  cc1101.setTXPwr(TX_PLUS_10_DBM);
  cc1101.setDataRate(2400); // Set to 2.4 kbps
  cc1101.setRxBW(RX_BW_162_KHZ);
  cc1101.setModulation(ASK_OOK); // Reverted to ASK_OOK
  Serial.println("CC1101 initialized.");

  // Initialize SPIFFS
  Serial.println("Initializing SPIFFS...");
  if (!SPIFFS.begin(true)) { // true will format SPIFFS if mount fails
    Serial.println("SPIFFS Mount Failed! Please check your partition scheme.");
    while(true) delay(1000); // Halt if SPIFFS cannot be initialized
  }
  Serial.println("SPIFFS initialized.");
  
  // Load signals from file at startup
  loadSignalsFromFile();

  // Connect to WiFi
  Serial.print("Connecting to WiFi...");
  WiFi.begin(ssid, password);
  unsigned long wifiConnectStart = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (millis() - wifiConnectStart > 30000) { // 30 second timeout for WiFi connection
        Serial.println("\nFATAL: WiFi connection timed out! Check SSID/Password or network.");
        Serial.println("Halting execution.");
        while(true) delay(1000); // Halt if WiFi cannot connect
    }
  }
  Serial.println("\nWiFi connected!");
  Serial.printf("IP address: %s\n", WiFi.localIP().toString().c_str());

  // Initialize mDNS
  if (!MDNS.begin("esp32-rf")) { // Set your desired hostname here
    Serial.println("Error starting mDNS");
  } else {
    Serial.println("mDNS responder started: esp32-rf.local");
    // Optionally add service type for easier discovery
    MDNS.addService("http", "tcp", 80);
  }

  // Set up web server routes
  Serial.println("Setting up web server routes...");
  server.on("/", handleRoot);
  server.on("/record", HTTP_POST, handleRecord);
  server.on("/transmit", HTTP_POST, handleTransmit);
  server.on("/list", HTTP_GET, handleList);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/load", HTTP_POST, handleLoad);
  server.on("/delete", HTTP_POST, handleDeleteSignal);
  server.on("/rename", HTTP_POST, handleRenameSignal); 
  server.on("/clearall", HTTP_POST, handleClearAll);
  server.on("/dump_signals", HTTP_GET, handleDumpSignals); // Route for dumping all signals

  // Start the web server
  Serial.println("Starting web server...");
  server.begin();
  Serial.println("Web server started!");
  Serial.printf("Access the Web UI at: http://%s or http://esp32-rf.local\n", WiFi.localIP().toString().c_str());

}

// --- Main Loop Function ---
void loop() {
  server.handleClient(); // Process incoming web client requests
  delay(100); // Small delay to prevent watchdog timer from triggering
}