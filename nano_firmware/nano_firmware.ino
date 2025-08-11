#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Keypad.h>
#include <Adafruit_Fingerprint.h>
#include <SoftwareSerial.h>
#include <avr/sleep.h>
#include <avr/interrupt.h>
#include <avr/wdt.h>


// --- PIN Definitions ---
#define PIR_PIN 2
#define FINGER_RX A0
#define FINGER_TX A1
#define WAKE_PIN 3

// --- LCD ---
LiquidCrystal_I2C lcd(0x27, 16, 2);

// --- Fingerprint Sensor ---
SoftwareSerial fingerSerial(FINGER_RX, FINGER_TX);
Adafruit_Fingerprint finger(&fingerSerial);

// --- ID Storage ---
#define MAX_IDS 127
uint8_t freeIDs[MAX_IDS];
uint8_t freeIDCount = 0;

// --- Keypad ---
const byte ROWS = 4, COLS = 4;
const char keys[ROWS][COLS] = {
  { '1', '2', '3', 'A' },
  { '4', '5', '6', 'B' },
  { '7', '8', '9', 'C' },
  { '*', '0', '#', 'D' }
};
const byte rowPins[ROWS] = { 12, 11, 10, 9 };
const byte colPins[COLS] = { 8, 7, 6, 5 };
Keypad keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// --- Flags ---
volatile bool motionDetected = false;
bool wakeHandled = false;
bool enterAdminMode = false;
unsigned long lastActivityTime = 0;

// --- Helpers ---
void printLcd(const char* line1, const char* line2 = nullptr) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(line1);
  if (line2) {
    lcd.setCursor(0, 1);
    lcd.print(line2);
  }
}

void wakeUp() {
  motionDetected = true;
  // WAKE THE ESP
  // Serial.println("WAKE");
  // printLcd("Voter Detected");
  // delay(1000);
  digitalWrite(WAKE_PIN, HIGH);  // Send wake signal
  delay(100);
  digitalWrite(WAKE_PIN, LOW);
  delay(100);
  lastActivityTime = millis();
}

void enterSleep() {
  lcd.clear();
  lcd.noBacklight();
  //Serial.println("SLEEP");

  set_sleep_mode(SLEEP_MODE_PWR_DOWN);
  sleep_enable();
  attachInterrupt(digitalPinToInterrupt(PIR_PIN), wakeUp, RISING);
  sleep_cpu();
  detachInterrupt(digitalPinToInterrupt(PIR_PIN));
  sleep_disable();
  lcd.backlight();
}

String readSerialCommand(unsigned long timeout = 15000) {
  String command = "";
  unsigned long start = millis();
  while (millis() - start < timeout) {
    if (Serial.available()) {
      char c = Serial.read();
      if (c == '\n') break;
      if (c != '\r') command += c;
      start = millis();  // reset timeout
    }
  }
  command.trim();
  return command;
}

// --- Setup ---
void setup() {
  pinMode(PIR_PIN, INPUT);
  pinMode(WAKE_PIN, OUTPUT);
  Serial.begin(115200);
  lcd.init();
  lcd.backlight();
  //printLcd("Booting...");
  for (int i = 0; i < 4; i++) {
    String dots = "";
    for (int j = 0; j < i; j++) {
      dots += ".";
    }

    String line1 = "Booting" + dots;
    printLcd(line1.c_str());
    delay(750);
  }

  fingerSerial.end();
  delay(50);
  fingerSerial.begin(57600);
  delay(50);
  finger.begin(57600);
  delay(50);

  for (int i = 0; i < 6 && !finger.verifyPassword(); i++) {
    String dots = "";
    for (int j = 0; j < i; j++) {
      dots += ".";
    }

    String line2 = "Loading" + dots;
    printLcd("Sensor", line2.c_str());
    delay(1000);
  }

  if (!finger.verifyPassword()) {
    printLcd("Fingerprint", "sensor failed!");
    delay(10000);
    lcd.noBacklight();
    lcd.noDisplay();
    //while (true);
    wdt_enable(WDTO_15MS);  // enable watchdog timer for 15ms
    while (1)
      ;  // wait to trigger reset
  }

  for (uint8_t i = 1; i <= MAX_IDS; i++) {
    if (finger.loadModel(i) != FINGERPRINT_OK) {
      freeIDs[freeIDCount++] = i;
    }
  }

  Serial.print("Free IDs: ");
  for (uint8_t i = 0; i < freeIDCount; i++) {
    Serial.print(freeIDs[i]);
    Serial.print(" ");
  }
  Serial.println();
  delay(50);  
  motionDetected = true;
  delay(50);
}

// --- Main Loop ---
// void loop() {
//   if (!motionDetected) {
//     wakeHandled = false;
//     enterSleep();
//   }

//   if (motionDetected && !wakeHandled) {
//     // WAKE THE ESP
//     //Serial.println("WAKE");
//     // digitalWrite(WAKE_PIN, HIGH); // Send wake signal
//     // delay(100);
//     // digitalWrite(WAKE_PIN, LOW);

//     printLcd("Voter Detected");
//     delay(1000);
//     printLcd("Look into", "the camera...");
//     delay(50);
//     wakeHandled = true;
//     lastActivityTime = millis();
//   }

//   String serialBuffer = "";
//   while (Serial.available()) {
//     char ch = Serial.read();
//     if (ch == '\n' || ch == '\r') {
//       serialBuffer.trim();

//       if (serialBuffer == "VOTE") {
//         delay(1500);
//         handleVote();
//         printLcd("Look into", "the camera...");
//         delay(1000);
//         lastActivityTime = millis();
//       } else if (serialBuffer == "ADMIN MODE") {
//         enterAdminMode = true;
//       } else if (serialBuffer.startsWith("WIFI CONNECTED: ")) {
//         String ip = serialBuffer.substring(18);  // Corrected index
//         printLcd("Admin Link: ", ip.c_str());
//         delay(5000);
//         printLcd("Look into", "the camera...");
//         lastActivityTime = millis();
//       }

//       if (enterAdminMode) {
//         printLcd("Admin Mode");
//         delay(1000);
//         adminMode();
//         enterAdminMode = false;
//         lastActivityTime = millis();
//       }

//       serialBuffer = "";  // Clear buffer after processing
//     } else {
//       serialBuffer += ch;
//     }
//   }


//   if (millis() - lastActivityTime > 60000 && motionDetected) {
//     motionDetected = false;
//     printLcd("No face", "detected!");
//     delay(1000);
//     printLcd("Sleeping...");
//     delay(1000);
//     Serial.println("SLEEP");
//     delay(100);
//   }
// }

void loop() {
  if (!motionDetected) {
    wakeHandled = false;
    enterSleep();
  }

  if (motionDetected && !wakeHandled) {
    //printLcd("Voter Detected", " ");
    printLcd("Look into", "the camera...");
    delay(50);
    wakeHandled = true;
    lastActivityTime = millis();
  }

  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    lastActivityTime = millis();

    if (cmd == "VOTE") {
      //do {
      delay(1500);
      handleVote();
      printLcd("Look into", "the camera...");
      delay(1000);
      lastActivityTime = millis();
      //} while (!enterAdminMode && askCastVote());
    } else if (cmd == "ADMIN MODE") {
      enterAdminMode = true;
    } else if (cmd.startsWith("WIFI CONNECTED: ")) {
      String ip = cmd.substring(23);  // Extract IP after prefix
      printLcd("Admin Link: ", ip.c_str());
      delay(5000);
      printLcd("Look into", "the camera...");
      lastActivityTime = millis();
    }

    if (enterAdminMode) {
      // if (adminLogin() == "Denied") {
      //   enterAdminMode = false;
      //   printLcd("Access Denied");
      //   delay(1000);
      //   return;
      // }
      printLcd("Admin Mode");
      delay(1000);
      adminMode();
      enterAdminMode = false;
      lastActivityTime = millis();
      return;
    }
  }

  if (millis() - lastActivityTime > 60000 && motionDetected) {
    motionDetected = false;
    printLcd("No face", "detected!");
    delay(1000);
    printLcd("Sleeping...");
    delay(1000);
    Serial.println("SLEEP");
    delay(100);
  }
}

// --- Voting ---

void handleVote() {
  enterAdminMode = false;
  const char* positions[3] = { "Presidential", "Gubernatorial", "Senatorial" };
  const char* parties[10] = {
    "000", "APC", "PDP", "LP", "NNPP",
    "ADC", "APGA", "SDP", "YPP", "AAC"
  };
  int votes[3] = { -1, -1, -1 };

  auto voteForPosition = [&](int index) {
    char line2[17];
    snprintf(line2, sizeof(line2), "%s:", positions[index]);
    printLcd("Vote for", line2);
    delay(2000);
    printLcd("Press 0-9:", "Pick a party");
    while (true) {

      if (Serial.available()) {
        String cmd = Serial.readStringUntil('\n');
        cmd.trim();
        if (cmd.equalsIgnoreCase("ADMIN MODE")) {
          enterAdminMode = true;
          return;  // exit voting process
        }
      }

      char key = keypad.getKey();
      if (key != NO_KEY && key >= '0' && key <= '9') {
        int choice = key - '0';
        printLcd("You picked:", parties[choice]);
        delay(1500);

        printLcd("* to confirm", "D to cancel");
        char confirmKey = NO_KEY;
        while (confirmKey == NO_KEY) {
          if (Serial.available()) {
            String cmd = Serial.readStringUntil('\n');
            cmd.trim();
            if (cmd.equalsIgnoreCase("ADMIN MODE")) {
              enterAdminMode = true;
              return;  // exit voting process
            }
          }
          confirmKey = keypad.getKey();
        }
        if (confirmKey == '*') {
          votes[index] = choice;
          break;
        } else {
          char line2[17];
          snprintf(line2, sizeof(line2), "%s:", positions[index]);
          printLcd("Vote for", line2);
          delay(2000);
          printLcd("Press 0-9:", "Pick a party");
        }
      }
    }
  };

  // Voting loop
  for (int i = 0; i < 3; i++) {
    if (enterAdminMode) return;  // Exit voting immediately
    voteForPosition(i);
    delay(50);
  }


  // Show summary
  delay(750);
  for (int j = 0; j < 3; j++) {
    printLcd((String(positions[j]) + ":").c_str(), parties[votes[j]]);
    delay(2000);
  }

  // Editing phase
  printLcd("# to confirm", "A/B/C to edit");
  delay(1000);
  while (true) {
    if (Serial.available()) {
      String cmd = Serial.readStringUntil('\n');
      cmd.trim();
      if (cmd.equalsIgnoreCase("ADMIN MODE")) {
        enterAdminMode = true;
        return;  // exit voting process
      }
    }

    char key = keypad.getKey();
    if (key == NO_KEY) continue;

    if (key == '#') break;

    int editIndex = -1;
    if (key == 'A') editIndex = 0;
    else if (key == 'B') editIndex = 1;
    else if (key == 'C') editIndex = 2;

    if (editIndex >= 0 && editIndex < 3) {
      voteForPosition(editIndex);

      // Only show summary after editing
      delay(50);
      for (int j = 0; j < 3; j++) {
        printLcd((String(positions[j]) + ":").c_str(), parties[votes[j]]);
        delay(2000);
      }
      printLcd("# to confirm", "A/B/C to edit");
      delay(1000);
    }
  }



  // Fingerprint scan
  printLcd("Scan finger...");
  delay(1000);
  int id = getFingerprintID();
  if (id == -1) {
    //printLcd("Voter not", "found!");
    printLcd("Voting Failed", "Unknown voter");
    delay(2000);
    Serial.println("VOTING FAILED");
    return;
  }

  printLcd("Processing...");
  Serial.print("VOTE,");
  Serial.print(id);
  Serial.print(",");
  Serial.print(parties[votes[0]]);
  Serial.print(",");
  Serial.print(parties[votes[1]]);
  Serial.print(",");
  Serial.println(parties[votes[2]]);

  String ack = readSerialCommand(15000);
  if (ack.equals("VOTED")) {
    printLcd("Voting", "Complete...");
    delay(1500);
    printLcd("Thank you!");
    // } else if (ack.equals("DOUBLE VOTE")) {
    //   printLcd("Voting Failed!", "Previously voted");
    // } else if (ack.equals("UNREGISTERED")) {
    //   printLcd("Voting Failed", "Unknown voter");
  } else {
    printLcd("Network issue!", "Retry Later");
  }
  delay(2000);
}

// Utility: Wait for any key press
char waitForKey() {
  unsigned long startTime = millis();
  char key = NO_KEY;

  while ((millis() - startTime < 10000) && (key == NO_KEY)) {
    key = keypad.getKey();

    if (Serial.available()) {
      String cmd = Serial.readStringUntil('\n');
      cmd.trim();
      if (cmd.equalsIgnoreCase("ADMIN MODE")) {
        enterAdminMode = true;
        return;  // exit waiting process
      }
    }
  }
  return key;  // will return NO_KEY if no key was pressed in 10 seconds
}


// --- Admin Mode ---
// String adminLogin() {
//   const char* correctPin = "1234";
//   uint8_t attempts = 0;

//   while (attempts < 3) {
//     while (keypad.getKey() != NO_KEY);  // Wait for key release

//     printLcd("Enter PIN:");
//     char enteredPin[5] = {0};  // 4 digits + null terminator
//     uint8_t len = 0;

//     lcd.setCursor(0, 1);
//     while (len < 4) {
//       char key = keypad.getKey();
//       if (key >= '0' && key <= '9') {
//         enteredPin[len++] = key;
//         lcd.print('*');
//       }
//     }

//     if (strcmp(enteredPin, correctPin) == 0) {
//       printLcd("Access Granted");
//       delay(1000);
//       return "Granted";
//     } else {
//       attempts++;
//       char msg[17];
//       snprintf(msg, sizeof(msg), "Wrong PIN %d/3", attempts);
//       printLcd(msg);
//       delay(1500);
//     }
//   }
//   return "Denied";
// }

void adminMode() {
  printLcd("Admin Mode");
  delay(1000);
  unsigned long adminStart = millis();

  while (true) {
    if (Serial.available()) {
      String cmd = readSerialCommand();
      cmd.trim();
      adminStart = millis();

      if (cmd == "EXIT ADMIN") {
        printLcd("Exiting Admin");
        delay(1000);
        lastActivityTime = millis();
        printLcd("Look into", "the camera...");
        break;
      } else if (cmd == "ENROL") {
        printLcd("Enrolling...");
        delay(1500);
        int id = enrollFingerprint();
        if (id != -1) {
          char msg[16];
          snprintf(msg, sizeof(msg), "Enrolled ID: %d", id);
          printLcd(msg);
          delay(1500);
          Serial.println("ENROLLED ID: " + String(id));
        } else {
          printLcd("Enrollment Failed!");
          delay(1500);
          Serial.println("NOT ENROLLED");
        }
        printLcd("Admin Mode");
      } else if (cmd.startsWith("DELETE ID: ")) {
        int idToDelete = cmd.substring(11).toInt();
        char buf[16];
        snprintf(buf, sizeof(buf), "Deleting ID: %d", idToDelete);
        printLcd(buf);
        delay(1500);

        int deleted = deleteID(idToDelete);
        if (deleted != -1) {
          snprintf(buf, sizeof(buf), "Deleted ID: %d", idToDelete);
          printLcd(buf);
          Serial.print("DELETED ID: ");
          Serial.println(idToDelete);
        } else {
          printLcd("Deletion Failed!");
          Serial.print("NOT DELETED ID: ");
          Serial.println(idToDelete);
        }
        delay(1500);
        printLcd("Admin Mode");
      } else {
        printLcd("Invalid Cmd");
        delay(1500);
        printLcd("Admin Mode");
      }
    }

    // if (millis() - adminStart > 30000) {
    //   printLcd("Admin Timeout");
    //   delay(1000);
    //   lastActivityTime = millis();
    //   printLcd("Look into", "the camera...");
    //   break;
    // }
  }
}

// --- Fingerprint ---
int enrollFingerprint() {
  if (freeIDCount == 0) return -1;
  int id = freeIDs[0];

  printLcd("Place finger...");
  delay(3000);
  if (finger.getImage() != FINGERPRINT_OK || finger.image2Tz(1) != FINGERPRINT_OK)
    return -1;

  printLcd("Remove finger");
  delay(3000);
  while (finger.getImage() != FINGERPRINT_NOFINGER) delay(500);

  printLcd("Place finger", "again...");
  while (finger.getImage() != FINGERPRINT_OK) delay(500);

  if (finger.image2Tz(2) != FINGERPRINT_OK || finger.createModel() != FINGERPRINT_OK || finger.storeModel(id) != FINGERPRINT_OK)
    return -1;

  for (uint8_t i = 1; i < freeIDCount; i++) freeIDs[i - 1] = freeIDs[i];
  freeIDCount--;
  return id;
}

int getFingerprintID() {
  for (int i = 0; i < 10; i++) {
    if (finger.getImage() == FINGERPRINT_OK && finger.image2Tz() == FINGERPRINT_OK && finger.fingerSearch() == FINGERPRINT_OK)
      return finger.fingerID;
    delay(300);
  }
  return -1;
}

int deleteID(uint8_t id) {
  if (id == 0 || id > MAX_IDS) return -1;
  if (finger.deleteModel(id) != FINGERPRINT_OK) return -1;

  for (uint8_t i = 0; i < freeIDCount; i++) {
    if (freeIDs[i] == id) return id;
  }

  if (freeIDCount < MAX_IDS) {
    freeIDs[freeIDCount++] = id;
    for (uint8_t i = 0; i < freeIDCount - 1; i++) {
      for (uint8_t j = 0; j < freeIDCount - i - 1; j++) {
        if (freeIDs[j] > freeIDs[j + 1]) {
          uint8_t temp = freeIDs[j];
          freeIDs[j] = freeIDs[j + 1];
          freeIDs[j + 1] = temp;
        }
      }
    }
  }

  return id;
}