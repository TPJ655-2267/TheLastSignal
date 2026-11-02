#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <ESP32Servo.h>

// ==========================================
// INPUTS
// ==========================================
#define ENC_CLK_PIN     18
#define ENC_DT_PIN      19
#define ENC_SW_PIN      21
#define IR_BEAM_PIN     25
#define FSR_PIN         34

// ==========================================
// TFT
// ==========================================
#define TFT_CS          5
#define TFT_RST         4
#define TFT_DC          2
#define TFT_SCK         14
#define TFT_MOSI        23

// ==========================================
// RGB LED — COMMON ANODE
// ==========================================
#define RGB_R_PIN       32
#define RGB_G_PIN       26
#define RGB_B_PIN       27
#define RGB_COMMON_ANODE  true

#define RGB_CH_R        2
#define RGB_CH_G        3
#define RGB_CH_B        4
#define RGB_PWM_FREQ    5000
#define RGB_PWM_RES     8

// ==========================================
// VIBRATION MOTOR + SERVO
// ==========================================
#define VIB_PIN         22
#define SERVO_PIN       33
#define SERVO_CLOSED_DEG   0
#define SERVO_OPEN_DEG     90      

// ==========================================
// Tuning
// ==========================================
#define IR_ACTIVE_HIGH  false
#define TARGET_FREQ     100.0      
#define FREQ_TOLERANCE  0.25       
#define FSR_THRESHOLD   800
#define FSR_STABLE_MS   200
#define FAILED_SHOW_MS  1500
#define BEAM_STABLE_MS  50
#define VIB_PULSE_MS    200
#define IR_VIB_MS       2000

Adafruit_ST7735 tft(TFT_CS, TFT_DC, TFT_RST);
Servo doorServo;

// --- ENCODER VARIABLES (Volatile for Interrupts) ---
volatile float frequency = 103.7;
volatile int encState = 0;
volatile unsigned long lastEncTime = 0;
volatile bool encChanged = false; // Tells the loop to update the screen

bool lastKnobPressed = false;

bool fsrActive    = false;
bool fsrStable    = false;
unsigned long fsrStartTime = 0;
unsigned long fsrFailedAt  = 0;
bool fsrShowFailed         = false;

bool stableBeamBroken  = false;
bool lastRawBeamBroken = false;
unsigned long lastBeamChange = 0;

unsigned long irVibStart  = 0;
bool          irVibActive = false;

bool doorIsOpen = false;
bool missionUnlocked = false;

String lastLine1 = "";
String lastLine2 = "";
String lastLine3 = "";

inline bool irBroken(bool raw, bool activeHigh) {
    return activeHigh ? (raw == HIGH) : (raw == LOW);
}

// ==========================================
// HARDWARE INTERRUPT FOR ENCODER
// ==========================================
void IRAM_ATTR readEncoder() {
    unsigned long now = millis();
    
    // 50ms lockout: physically ignores static and metal bounce
    if (now - lastEncTime > 50) {
        int clk = digitalRead(ENC_CLK_PIN);
        int dt  = digitalRead(ENC_DT_PIN);
        int currentState = (clk << 1) | dt;

        // Clockwise
        if ((encState == 0 && currentState == 1) || 
            (encState == 1 && currentState == 3) ||
            (encState == 3 && currentState == 2) ||
            (encState == 2 && currentState == 0)) {
            frequency += 0.1;
        }
        // Counter-Clockwise
        else if ((encState == 0 && currentState == 2) || 
                 (encState == 2 && currentState == 3) ||
                 (encState == 3 && currentState == 1) ||
                 (encState == 1 && currentState == 0)) {
            frequency -= 0.1;
        }

        if (frequency > 200.0) frequency = 200.0;
        if (frequency < 50.0)  frequency = 50.0;
        
        encState = currentState;
        lastEncTime = now;
        encChanged = true;
    }
}

// ==========================================
// Helpers
// ==========================================
void vibOn()  { digitalWrite(VIB_PIN, HIGH); }
void vibOff() { digitalWrite(VIB_PIN, LOW);  }

void doorOpen() {
    if (!doorIsOpen) { doorServo.write(SERVO_OPEN_DEG); doorIsOpen = true; }
}
void doorClose() {
    if (doorIsOpen) { doorServo.write(SERVO_CLOSED_DEG); doorIsOpen = false; }
}

void rgbSet(uint8_t r, uint8_t g, uint8_t b) {
    if (RGB_COMMON_ANODE) {
        ledcWrite(RGB_CH_R, 255 - r); ledcWrite(RGB_CH_G, 255 - g); ledcWrite(RGB_CH_B, 255 - b);
    } else {
        ledcWrite(RGB_CH_R, r); ledcWrite(RGB_CH_G, g); ledcWrite(RGB_CH_B, b);
    }
}
void rgbOff()    { rgbSet(0,   0,   0);   }
void rgbRed()    { rgbSet(255, 0,   0);   }
void rgbGreen()  { rgbSet(0,   255, 0);   }
void rgbBlue()   { rgbSet(0,   0,   255); }
void rgbYellow() { rgbSet(255, 255, 0);   }
void rgbCyan()   { rgbSet(0,   255, 255); }

void rgbInit() {
    ledcSetup(RGB_CH_R, RGB_PWM_FREQ, RGB_PWM_RES);
    ledcSetup(RGB_CH_G, RGB_PWM_FREQ, RGB_PWM_RES);
    ledcSetup(RGB_CH_B, RGB_PWM_FREQ, RGB_PWM_RES);
    ledcAttachPin(RGB_R_PIN, RGB_CH_R);
    ledcAttachPin(RGB_G_PIN, RGB_CH_G);
    ledcAttachPin(RGB_B_PIN, RGB_CH_B);
    rgbOff();
}

void tftClearLine(int y, int h) { tft.fillRect(0, y, tft.width(), h, ST77XX_BLACK); }
void tftPrintLine(int y, const String& text, uint16_t color, uint8_t size) {
    int h = (size == 2) ? 16 : 8;
    tftClearLine(y, h);
    tft.setCursor(2, y);
    tft.setTextColor(color);
    tft.setTextSize(size);
    tft.print(text);
}

void updateScreen() {
    String line1, line2, line3;
    uint8_t line2Size = 2;

    if (digitalRead(ENC_SW_PIN) == LOW) line1 = "FREQUENCY LOCKED";
    else line1 = "FREQUENCY: " + String(frequency, 1) + " MHz";

    bool beamBroken = stableBeamBroken;
    if (beamBroken) {
        line2 = "SECURITY BREACH! AUTHORIZATION FAILED";
        line2Size = 1;
    } else {
        line2 = "SECURITY BEAM: ACTIVE";
    }

    if (missionUnlocked)      line3 = "DOOR OPEN - ACCESS GRANTED";
    else if (fsrShowFailed)   line3 = "STABILIZATION FAILED";
    else if (!fsrActive)      line3 = "PRESSURE: NOT DETECTED";
    else if (fsrStable)       line3 = "PRESSURE STABLE";
    else                      line3 = "PRESSURE DETECTED";

    if (missionUnlocked)     rgbCyan();   
    else if (beamBroken)     rgbRed();    
    else                     rgbGreen();  

    String key2 = line2 + "|" + line2Size;
    if (line1 != lastLine1) {
        tftPrintLine(4,  line1, ST77XX_CYAN, 2);
        lastLine1 = line1;
    }
    if (key2 != lastLine2) {
        uint16_t c = beamBroken ? ST77XX_RED : ST77XX_GREEN;
        tftPrintLine(40, line2, c, line2Size);
        lastLine2 = key2;
    }
    if (line3 != lastLine3) {
        uint16_t c;
        if      (line3.startsWith("DOOR OPEN"))     c = ST77XX_CYAN;
        else if (line3.startsWith("STABILIZATION")) c = ST77XX_RED;
        else if (line3.startsWith("PRESSURE: NOT")) c = ST77XX_WHITE;
        else if (line3 == "PRESSURE STABLE")        c = ST77XX_GREEN;
        else                                        c = ST77XX_YELLOW;
        tftPrintLine(80, line3, c, 2);
        lastLine3 = line3;
    }
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n=== The Last Signal — Booting ===");

    ESP32PWM::allocateTimer(0);
    ESP32PWM::allocateTimer(1);
    doorServo.setPeriodHertz(50);
    doorServo.attach(SERVO_PIN, 500, 2400);
    doorServo.write(SERVO_CLOSED_DEG);
    doorIsOpen = false;

    pinMode(ENC_CLK_PIN, INPUT_PULLUP);
    pinMode(ENC_DT_PIN,  INPUT_PULLUP);
    pinMode(ENC_SW_PIN,  INPUT_PULLUP);
    pinMode(IR_BEAM_PIN, INPUT_PULLUP);
    pinMode(FSR_PIN,     INPUT);

    // Set initial encoder state so it doesn't jump on boot
    int clk = digitalRead(ENC_CLK_PIN);
    int dt = digitalRead(ENC_DT_PIN);
    encState = (clk << 1) | dt;

    // ATTACH THE HARDWARE INTERRUPT
    attachInterrupt(digitalPinToInterrupt(ENC_CLK_PIN), readEncoder, CHANGE);

    pinMode(VIB_PIN, OUTPUT);
    vibOff();

    rgbInit();
    SPI.begin(TFT_SCK, -1, TFT_MOSI, -1);
    tft.initR(INITR_BLACKTAB);
    tft.setRotation(1);
    tft.fillScreen(ST77XX_BLACK);
    tft.setTextWrap(false);

    rgbRed();   delay(400);
    rgbGreen(); delay(400);
    rgbBlue();  delay(400);
    rgbOff();

    vibOn();  delay(VIB_PULSE_MS); vibOff();

    doorServo.write(SERVO_OPEN_DEG);   delay(400);
    doorServo.write(SERVO_CLOSED_DEG); delay(400);
    doorIsOpen = false;

    stableBeamBroken   = irBroken(digitalRead(IR_BEAM_PIN), IR_ACTIVE_HIGH);
    lastRawBeamBroken  = stableBeamBroken;
    lastBeamChange     = millis();

    updateScreen();
    Serial.println("=== System Ready ===\n");
}

void loop() {
    unsigned long now = millis();

    // ----- 1. PRINT FREQUENCY -----
    static unsigned long lastFreqPrint = 0;
    if (now - lastFreqPrint > 500) {
        Serial.print("Frequency: "); Serial.print(frequency, 1); Serial.println(" MHz");
        lastFreqPrint = now;
    }

    // ----- 2. Knob press -----
    bool knobPressed = (digitalRead(ENC_SW_PIN) == LOW);
    if (knobPressed && !lastKnobPressed)      Serial.println("Encoder : LOCKED");
    else if (!knobPressed && lastKnobPressed) Serial.println("Encoder : UNLOCKED");
    lastKnobPressed = knobPressed;

    // ----- 3. Beam -----
    bool rawBroken = irBroken(digitalRead(IR_BEAM_PIN), IR_ACTIVE_HIGH);
    if (rawBroken != lastRawBeamBroken) {
        lastBeamChange = now;
        lastRawBeamBroken = rawBroken;
    }
    if (now - lastBeamChange > BEAM_STABLE_MS) {
        if (stableBeamBroken != rawBroken) {
            stableBeamBroken = rawBroken;
            lastLine2 = "";
            if (stableBeamBroken) {
                Serial.println("Beam    : BREACH");
                irVibStart = now;
                irVibActive = true;
                vibOn();
                doorClose();             
                missionUnlocked = false; 
            } else {
                Serial.println("Beam    : RESTORED");
            }
        }
    }

    if (irVibActive && (now - irVibStart > IR_VIB_MS)) {
        vibOff();
        irVibActive = false;
    }

    // ----- 4. FSR -----
    int fsr = analogRead(FSR_PIN);
    if (fsr > FSR_THRESHOLD && !fsrActive) {
        fsrActive = true;
        fsrStable = false;
        fsrShowFailed = false;
        fsrStartTime = now;
        Serial.println("FSR     : DETECTED");
    } else if (fsr <= FSR_THRESHOLD && fsrActive) {
        if (!fsrStable) {
            fsrShowFailed = true;
            fsrFailedAt = now;
            lastLine3 = "";
            Serial.println("FSR     : FAILED");
            vibOn();
        } else {
            Serial.println("FSR     : RELEASED");
            vibOff();
        }
        fsrActive = false;
        fsrStable = false;
    }
    
    if (fsrActive && !fsrStable && (now - fsrStartTime > FSR_STABLE_MS)) {
        fsrStable = true;
        Serial.println("FSR     : STABLE");
        vibOff();
    }
    
    if (fsrShowFailed && (now - fsrFailedAt > FAILED_SHOW_MS)) {
        fsrShowFailed = false;
        lastLine3 = "";
        vibOff();
    }

    // ==========================================
    // THE MISSION UNLOCK LOGIC
    // ==========================================
    bool atTargetFreq = (frequency >= (TARGET_FREQ - FREQ_TOLERANCE) && frequency <= (TARGET_FREQ + FREQ_TOLERANCE));
    
    if (atTargetFreq && !missionUnlocked) {
        Serial.println("SYSTEM          : Access Granted! Opening cassette door.");
        missionUnlocked = true;
        doorOpen();    
        lastLine3 = ""; 
    } 

    // ----- 5. Screen + RGB refresh -----
    static unsigned long lastRefresh = 0;
    // Only update the screen if the encoder moved, or if the game state changed
    if (encChanged || (now - lastRefresh > 100)) {
        encChanged = false;
        updateScreen();
        lastRefresh = now;
    }

    delay(2);
}