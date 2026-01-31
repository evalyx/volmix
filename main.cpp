#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>

#define TFT_CS     7
#define TFT_RST    8
#define TFT_DC     9
#define TFT_LED    6 

#define ST7735_GRAY  0x7BEF
#define ST7735_PINK  0xF81F 

const int layerPins[3] = {3, 4, 5};
int currentLayer = 0;
uint16_t layerColors[3] = {ST7735_GREEN, ST7735_CYAN, ST7735_ORANGE};

// Memory
int virtualValues[3][7] = {0};
int peakValues[3][7] = {0};      // Store peak per layer
int startPhysicalPos[7] = {0};
bool layerLocked[7] = {true, true, true, true, true, true, true};

const int NUM_CHANNELS = 7;
const int channelPins[NUM_CHANNELS] = {A4, A5, A0, A1, A2, A3, A6};
const int MASTER_PIN = A7;
const int UNLOCK_THRESHOLD = 30; 

Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);

// UI Positioning
const int CH_BAR_WIDTH = 12;
const int CH_BAR_MAX_H = 90;     // Slightly shorter to ensure clearance
const int CH_Y_BOTTOM = 125;     
const int MASTER_BAR_Y = 145;    
const int MASTER_BAR_H = 12;
const int SEPARATOR_Y = 32;      // The Gray Line position

void drawUIFrame() {
  tft.fillScreen(ST7735_BLACK);
  
  // Header
  tft.setCursor(5, 5);
  tft.setTextColor(ST7735_WHITE);
  tft.setTextSize(1);
  tft.print("LAYER");
  
  tft.setCursor(5, 15);
  tft.setTextColor(layerColors[currentLayer]);
  tft.setTextSize(2);
  tft.print(currentLayer + 1);

  tft.setCursor(85, 134);
  tft.setTextColor(ST7735_PINK);
  tft.setTextSize(1);
  tft.print("MASTER");

  // Draw the separator line once
  tft.drawFastHLine(0, SEPARATOR_Y, 128, ST7735_GRAY); 
}

void setup() {
  Serial.begin(115200);
  pinMode(TFT_LED, OUTPUT);
  digitalWrite(TFT_LED, HIGH);

  for(int i = 0; i < 3; i++) pinMode(layerPins[i], INPUT_PULLUP);

  tft.initR(INITR_BLACKTAB); 
  tft.setRotation(2); 
  drawUIFrame();

  for(int i=0; i<NUM_CHANNELS; i++) {
    startPhysicalPos[i] = analogRead(channelPins[i]);
  }
}

void loop() {
  // 1. Layer Switching
  int oldLayer = currentLayer;
  for (int i = 0; i < 3; i++) {
    if (digitalRead(layerPins[i]) == LOW) currentLayer = i;
  }

  if (currentLayer != oldLayer) {
    for(int i = 0; i < NUM_CHANNELS; i++) {
      layerLocked[i] = true;
      startPhysicalPos[i] = analogRead(channelPins[i]);
    }
    drawUIFrame();
  }

  // 2. Vertical Channels
  for (int i = 0; i < NUM_CHANNELS; i++) {
    int physicalPos = analogRead(channelPins[i]);
    
    if (layerLocked[i]) {
      if (abs(physicalPos - startPhysicalPos[i]) > UNLOCK_THRESHOLD) {
        layerLocked[i] = false;
      }
    }

    if (!layerLocked[i]) {
      virtualValues[currentLayer][i] = physicalPos;
    }

    // Update Peak
    if (virtualValues[currentLayer][i] > peakValues[currentLayer][i]) {
      peakValues[currentLayer][i] = virtualValues[currentLayer][i];
    }

    int xPos = 6 + (i * 17);
    int barHeight = map(virtualValues[currentLayer][i], 0, 1023, 0, CH_BAR_MAX_H);
    int peakHeight = map(peakValues[currentLayer][i], 0, 1023, 0, CH_BAR_MAX_H);
    
    uint16_t color = layerLocked[i] ? ST7735_GRAY : layerColors[currentLayer];
    
    // Draw Bar
    tft.fillRect(xPos, CH_Y_BOTTOM - barHeight, CH_BAR_WIDTH, barHeight, color);
    
    // Erase space between Bar Top and Peak (or Separator)
    // We start erasing 1 pixel BELOW the separator to keep the line clean
    int eraseStart = SEPARATOR_Y + 1;
    int eraseHeight = (CH_Y_BOTTOM - barHeight) - eraseStart;
    tft.fillRect(xPos, eraseStart, CH_BAR_WIDTH, eraseHeight, ST7735_BLACK);

    // Draw Red Peak Line (2 pixels high)
    tft.drawFastHLine(xPos, CH_Y_BOTTOM - peakHeight, CH_BAR_WIDTH, ST7735_RED);
    tft.drawFastHLine(xPos, CH_Y_BOTTOM - peakHeight + 1, CH_BAR_WIDTH, ST7735_RED);
  }

  // 3. Horizontal Master
  int masterVal = analogRead(MASTER_PIN);
  int masterWidth = map(masterVal, 0, 1023, 0, 118);
  tft.drawRect(4, MASTER_BAR_Y, 120, MASTER_BAR_H, ST7735_GRAY);
  tft.fillRect(5, MASTER_BAR_Y + 1, masterWidth, MASTER_BAR_H - 2, ST7735_PINK);
  tft.fillRect(5 + masterWidth, MASTER_BAR_Y + 1, 118 - masterWidth, MASTER_BAR_H - 2, ST7735_BLACK);
  // 4. --- SERIAL PRINTER FOR LINUX HOST ---
  // Format: DATA,Layer,Master,Ch0,Ch1,Ch2,Ch3,Ch4,Ch5,Ch6
  Serial.print("DATA,");
  Serial.print(currentLayer);
  Serial.print(",");
  Serial.print(masterVal);
  for(int i = 0; i < NUM_CHANNELS; i++) {
    Serial.print(",");
    Serial.print(virtualValues[currentLayer][i]);
  }
  Serial.println(); // Ends with \r\n for easy getline() in C++
  delay(10);
}