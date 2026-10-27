// INCLUDES
#include <SoftwareSerial.h>
// Used to create serial interface to DY-HV20T/DY-SV8F/DY-SV5W/similar audio player
// See https://github.com/SnijderC/dyplayer
#include <DYPlayerArduino.h>
// I2C LCD (install "LiquidCrystal I2C" by Frank de Brabander via Library Manager)
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// CONSTANTS
const int numPlayers = 4; // Number of players
const int buttonPins[numPlayers] = {A0, A1, A2, A3}; // Pins connected to player buttons
const int ledPins[numPlayers] = {5, 4, 3, 2}; // Pins connected to player LEDs

// NOTE: A4 and A5 are the I2C pins (SDA/SCL) on an Uno/Nano, so the host buttons
// have been moved to digital pins 6 and 7.
const int correctButtonPin = 6; // Pin connected to the button for correct answer
const int wrongButtonPin = 7;   // Pin connected to the button for wrong answer

// LCD settings
const uint8_t lcdAddress = 0x27; // Try 0x3F if nothing shows up
const uint8_t lcdColumns = 16;
const uint8_t lcdRows = 2;

// Sound effect file numbers
const int sfxBuzzer[numPlayers] = {4, 4, 4, 4}; // Index of sound fx file to play for each player's buzzer
const int sfxIncorrect = 2; // SFX for incorrect answer
const int sfxCorrect = 3; // SFX for correct answer
const int sfxIntro = 1; // SFX played when game starts

// GLOBALS
// If, and which, player has buzzed in
int8_t playerWhoBuzzed = -1;
// Array to track whether each player is active
bool playerActive[numPlayers] = {true, true, true, true};
int8_t playerScore[numPlayers] = {0, 0, 0, 0};
// Create an object to address the audio player using the secondary serial interface
SoftwareSerial softSerial(8, 9); // RX, TX
DY::Player player(&softSerial);
// LCD object (I2C address, columns, rows)
LiquidCrystal_I2C lcd(lcdAddress, lcdColumns, lcdRows);

void setup() {
  // Initialise a serial connection for debugging
  Serial.begin(115200);
  Serial.println(__FILE__ __DATE__);

  // Start the LCD
  lcd.init();
  lcd.backlight();

  // Configure pins
  for (int i=0; i<numPlayers; i++) {
    pinMode(buttonPins[i], INPUT_PULLUP);
    pinMode(ledPins[i], OUTPUT);
  }
  pinMode(correctButtonPin, INPUT_PULLUP);
  pinMode(wrongButtonPin, INPUT_PULLUP);

  // Start the software interface to the DY audio player
  softSerial.begin(9600);
  // Attach the player object to the serial interface
  player.begin();
  // Set OneOff playback mode (i.e. non-looping)
  player.setCycleMode(DY::PlayMode::OneOff);
  // Volume is a value from 0-30
  player.setVolume(25);
  // Ensure the player is not currently playing anything
  player.stop();
  // Play the intro theme music
  player.playSpecified(sfxIntro);

  // Show the initial scores
  updateScoreDisplay();
}

void loop() {
  // We check for reset on every loop
  if(digitalRead(correctButtonPin) == LOW) {
    while(digitalRead(correctButtonPin) == LOW) { delay(100);}
    Serial.println("Reactivating all players");
    for (int i=0; i<numPlayers; i++) {
        activatePlayer(i);
      }
  }

  // Check if any player buzzes in
  for (int i = 0; i < numPlayers; i++) {
    // Light up the player's button if they are active
    digitalWrite(ledPins[i], playerActive[i] ? HIGH : LOW);
    // If an active player presses their button
    if (digitalRead(buttonPins[i]) == LOW && playerActive[i]) {
      Serial.print("Player ");
      Serial.print(i+1);
      Serial.println(" buzzed in");
      player.playSpecified(sfxBuzzer[i]);
      playerWhoBuzzed = i;
      break;
    }
  }
  // If a player has buzzed in, indicate it and play a sound
  if (playerWhoBuzzed != -1) {

    // Light up only the player who buzzed's LED
    for (int i=0; i<numPlayers; i++) {
      digitalWrite(ledPins[i], (playerWhoBuzzed == i));
    }

    // Show which player buzzed in on the LCD
    showBuzzedMessage(playerWhoBuzzed);

    // Wait for the host to confirm whether the answer is correct or not
    bool isAnswerCorrect = waitForHostInput();

    // If the answer is incorrect
    if (!isAnswerCorrect) {
      // Play the incorrect SFX
      player.playSpecified(sfxIncorrect);
      // Deduct a point for buzzing in incorrectly
      playerScore[playerWhoBuzzed]--;
      // Lock out this player from answering again
      Serial.print("INCORRECT. Deactivating player ");
      Serial.println(playerWhoBuzzed+1);
      deactivatePlayer(playerWhoBuzzed);

    }
    // If correct
    if(isAnswerCorrect) {
      // Play the correct SFX
      player.playSpecified(sfxCorrect);
       // Add a point
      playerScore[playerWhoBuzzed]++;
      // Re-enable all players to buzz for the next question
      Serial.println("CORRECT. Reactivating all players");
      for (int j = 0; j < numPlayers; j++) { activatePlayer(j); }
    }

    // Update the scores
    updateScoreDisplay();

    // Reset the player buzz index
    playerWhoBuzzed = -1;
  }
}

void updateScoreDisplay(){
  // --- Serial output (unchanged) ---
  Serial.print("Player ");
  char buffer[12];
  for(int i=0; i<numPlayers; i++){
    snprintf(buffer, sizeof(buffer), "%5d", i+1);
    Serial.print(buffer);
  }
  Serial.println("");
  Serial.print("Score  ");
  for(int i=0; i<numPlayers; i++){
    snprintf(buffer, sizeof(buffer), "%5d", playerScore[i]);
    Serial.print(buffer);
  }
  Serial.println("");

  // --- LCD output ---
  // 16 columns / 4 players = 4 characters per player.
  // Line 1: "P1  P2  P3  P4  "
  // Line 2: scores, left-aligned in each 4-character slot, e.g. "0   -1  2   0   "
  char line[lcdColumns + 1];

  for (int i = 0; i < numPlayers; i++) {
    snprintf(line + i * 4, 5, "P%-3d", i + 1);
  }
  lcd.setCursor(0, 0);
  lcd.print(line);

  for (int i = 0; i < numPlayers; i++) {
    snprintf(line + i * 4, 5, "%-4d", playerScore[i]);
  }
  lcd.setCursor(0, 1);
  lcd.print(line);
}


// Show "Player X BUZZED!" on the LCD while the host decides.
// Both lines are exactly 16 characters, so they fully overwrite the scores.
// The scores are redrawn by updateScoreDisplay() once the host has answered.
void showBuzzedMessage(int playerIndex) {
  char line[lcdColumns + 1];
  snprintf(line, sizeof(line), "Player %d BUZZED!", playerIndex + 1);
  lcd.setCursor(0, 0);
  lcd.print(line);
  lcd.setCursor(0, 1);
  lcd.print("Correct or wrong");
}

// Function to deactivate buzzer for a specific player
void deactivatePlayer(int player) {
  playerActive[player] = false;
}

// Function to activate buzzer for a specific player
void activatePlayer(int player) {
  playerActive[player] = true;
}

// Function to pause and wait for the host to input whether the answer is correct or not
bool waitForHostInput() {
  while (true) {
    if (digitalRead(correctButtonPin) == LOW) {
      while(digitalRead(correctButtonPin) == LOW) { delay(100);}
      return true;
    } else if (digitalRead(wrongButtonPin) == LOW) {
      while(digitalRead(wrongButtonPin) == LOW) { delay(100);}
      return false;
    }
  }
}
