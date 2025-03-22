// Fill-in information from your Blynk Template here
#define BLYNK_TEMPLATE_ID "YOUR_TEMPLATE_ID"
#define BLYNK_DEVICE_NAME "Plant Watering System"

#define BLYNK_FIRMWARE_VERSION "1.0.0"

#define BLYNK_PRINT Serial
//#define BLYNK_DEBUG

#include "BlynkEdgent.h"
#include <driver/adc.h>

// Conversion constants
#define SECONDS_TO_MICROSECONDS 1000000LL
#define MINUTES_TO_SECONDS 60

// Pin Configuration
// Note: Use ADC1 pins (32,33,34,35,36,39) for analog readings as ADC2 conflicts with WiFi
const byte BUTTON_PIN = 0;
const byte BATTERY_LEVEL_PIN = 32;  // Voltage divider with 1M and 30K ohm resistors
const byte PUMP_POWER_PIN = 23;
const byte MOISTURE_SENSOR_POWER_PIN = 19;
const byte MOISTURE_SENSOR_SIGNAL_PIN = 33;
const byte WATER_LEVEL_GROUND_PIN = 35; // Needs 100k pulldown resistor
const byte WATER_LEVEL_PINS[] = {13, 14, 27, 26, 25}; // 100%, 75%, 50%, 25%, 10%

// RTC Memory values (preserved during deep sleep)
RTC_DATA_ATTR int bootCount = 0;
RTC_DATA_ATTR int sleepDuration = 30;  // Sleep time in minutes
RTC_DATA_ATTR int soilMoistureCritical = 25;  // Critical soil moisture % to trigger watering
RTC_DATA_ATTR int waterAmount = 20;  // Amount of water to dispense in mL
RTC_DATA_ATTR int pumpPowerMin = 150;
RTC_DATA_ATTR int pumpPowerMax = 230;
RTC_DATA_ATTR int waterFlowCalibration = 250;  // mL per minute at max power
RTC_DATA_ATTR int soilMoistureCalibrationAir = 3180;
RTC_DATA_ATTR int soilMoistureCalibrationWater = 1320;
RTC_DATA_ATTR int waterLevelSensorThreshold = 400;

// Flag values from Blynk app (reset after reboot/sleep)
int pumpPowerMinCalibrationFlag = 0;
int pumpPowerMaxCalibrationFlag = 0;
int waterFlowCalibrationFlag = 0;
int soilMoistureCalibrationAirFlag = 0;
int soilMoistureCalibrationWaterFlag = 0;
int waterLevelSensorRawReadingsFlag = 0;

// State machine variables
byte downloadBlynkState = 0;
byte uploadBlynkState = 0;
byte batteryLevelMeasureState = 0;
byte waterLevelMeasureState = 0;
byte soilMoistureMeasureState = 0;
byte pumpOperationState = 0;
byte routineState = 0;

// Blynk related variables
int blynkSyncCounter = 0;
int blynkSyncNumber = 15;  // Number of values to download from Blynk server
boolean blynkSyncRequired = false;
boolean BlynkInitialized = false;

// Sensor reading variables
esp_sleep_wakeup_cause_t wakeupReason;
const int MEASURE_WAIT_TIME = 200;  // ms between measurements
int batteryLevelReading[10];
int batteryLevelAverage = 0;
float batteryLevelVoltage = 0;
int batteryLevelPercentage = 0;
int waterLevelSensorReading[5];
int waterLevelPercentage = 0;
const int WATER_LEVEL_VALUES[] = {100, 75, 50, 25, 10, 0};  // Water levels in % for each pin
int soilMoistureReading[10];
int soilMoistureAverage = 0;
int soilMoistureCalibrated = 0;
int soilMoisturePercentage = 0;
int pumpActivityFlag = 0;

// Pump PWM settings
const int PUMP_PWM_FREQUENCY = 490;
const int PUMP_PWM_CHANNEL = 0;
const int PUMP_PWM_RESOLUTION = 8;  // 8-bit = 0-255

// BlynkTimer for safety timeout
BlynkTimer timer;

void setup() {
  delay(500);  // Required for reliable wake-up
  
  // Initialize pins
  pinMode(BUTTON_PIN, INPUT);
  pinMode(BATTERY_LEVEL_PIN, INPUT);
  pinMode(PUMP_POWER_PIN, OUTPUT);
  pinMode(MOISTURE_SENSOR_SIGNAL_PIN, INPUT);
  pinMode(MOISTURE_SENSOR_POWER_PIN, OUTPUT);
  pinMode(WATER_LEVEL_GROUND_PIN, INPUT);
  
  for (int i = 0; i < 5; i++) {
    pinMode(WATER_LEVEL_PINS[i], INPUT);
  }
  
  pinMode(LED_BUILTIN, OUTPUT);
  
  Serial.begin(115200);
  delay(100);  // Wait for serial monitor
  
  bootCount++;
  Serial.printf("Boot count: %d\n", bootCount);
  
  // Setup safety timeout of 2 minutes
  timer.setTimeout(120000L, enterDeepSleep);
  
  // Start the main routine
  routineState = 1;
}

void loop() {
  // State machine execution
  downloadFromBlynk();
  uploadToBlynk();
  measureBatteryLevel();
  measureWaterLevel();
  measureSoilMoisture();
  operatePump();
  executeRoutine();
  
  if (BlynkInitialized) {
    BlynkEdgent.run();
  }
  
  timer.run();  // Run timer for safety timeout
}

void executeRoutine() {
  switch (routineState) {
    case 1:  // Download config from Blynk
      downloadBlynkState = 1;
      routineState++;
      break;
      
    case 2:  // Wait for download to complete, then start measurements
      if (downloadBlynkState == 100) {
        batteryLevelMeasureState = 1;
        waterLevelMeasureState = 1;
        soilMoistureMeasureState = 1;
        routineState++;
      }
      break;
      
    case 3:  // Wait for measurements to complete, then operate pump if needed
      if (batteryLevelMeasureState == 100 && waterLevelMeasureState == 100 && soilMoistureMeasureState == 100) {
        printSensorValues();
        pumpOperationState = 1;
        routineState++;
      }
      break;
      
    case 4:  // Wait for pump operation to complete, then upload data
      if (pumpOperationState == 100) {
        uploadBlynkState = 1;
        routineState++;
      }
      break;
      
    case 5:  // Wait for upload to complete, then enter deep sleep
      if (uploadBlynkState == 100) {
        enterDeepSleep();
      }
      break;
  }
}

void downloadFromBlynk() {
  switch (downloadBlynkState) {
    case 1:
      wakeupReason = esp_sleep_get_wakeup_cause();
      
      // First boot or button wakeup requires Blynk sync
      if (wakeupReason == ESP_SLEEP_WAKEUP_EXT0 || bootCount == 1) {
        Serial.println("First boot or manual wakeup - synchronizing with Blynk server");
        blynkSyncRequired = true;
        BlynkEdgent.begin();
        BlynkInitialized = true;
        downloadBlynkState++;
      } else {
        Serial.println("Timer wakeup - no synchronization needed");
        downloadBlynkState = 100;  // Mark as finished
      }
      break;
      
    case 2:
      if (blynkSyncCounter == blynkSyncNumber) {  // All values updated
        Serial.println("Synchronization complete");
        blynkSyncRequired = false;
        blynkSyncCounter = 0;
        Blynk.disconnect();
        WiFi.disconnect();
        BlynkInitialized = false;
        downloadBlynkState = 100;  // Mark as finished
      } else {
        Serial.println("Waiting for synchronization to complete...");
      }
      break;
  }
}

void uploadToBlynk() {
  static unsigned long previousUploadCheckTime = 0;
  
  switch (uploadBlynkState) {
    case 1:
      Serial.println("Uploading data to Blynk...");
      BlynkEdgent.begin();
      BlynkInitialized = true;
      uploadBlynkState++;
      break;
      
    case 2:
      if (millis() - previousUploadCheckTime >= 3000) {  // Check every 3 seconds
        previousUploadCheckTime = millis();
        Blynk.syncVirtual(V104);  // Triggers BLYNK_WRITE(V104)
      }
      break;
  }
}

void measureBatteryLevel() {
  static int readingIndex = 0;
  static unsigned long previousMeasureTime = 0;
  
  switch (batteryLevelMeasureState) {
    case 1:  // Initialize
      batteryLevelAverage = 0;
      readingIndex = 0;
      batteryLevelMeasureState++;
      break;
      
    case 2:  // Take 10 readings
      if (readingIndex < 10 && millis() - previousMeasureTime >= MEASURE_WAIT_TIME) {
        previousMeasureTime = millis();
        batteryLevelReading[readingIndex] = analogRead(BATTERY_LEVEL_PIN);
        batteryLevelAverage += batteryLevelReading[readingIndex];
        readingIndex++;
      }
      
      if (readingIndex == 10) {
        batteryLevelMeasureState++;
      }
      break;
      
    case 3:  // Calculate battery level
      batteryLevelAverage /= 10;
      
      // Convert ADC value to voltage (adjust multiplier based on your voltage divider)
      batteryLevelVoltage = batteryLevelAverage / 4096.0 * 4.62;
      
      // Convert voltage to percentage using LiPo discharge curve
      batteryLevelPercentage = calculateBatteryPercentage(batteryLevelVoltage);
      
      batteryLevelMeasureState = 100;  // Mark as finished
      break;
  }
}

int calculateBatteryPercentage(float voltage) {
  // Simplified battery percentage calculation
  if (voltage > 4.2) return 100;
  if (voltage < 3.5) return 0;
  
  // Linear approximation between 3.5V (0%) and 4.2V (100%)
  return (voltage - 3.5) * 142.85;
}

void measureWaterLevel() {
  static int pinIndex = 0;
  static unsigned long previousMeasureTime = 0;
  
  switch (waterLevelMeasureState) {
    case 1:  // Initialize
      pinIndex = 0;
      for (int i = 0; i < 5; i++) {
        waterLevelSensorReading[i] = 0;
      }
      waterLevelPercentage = 0;
      waterLevelMeasureState++;
      break;
      
    case 2:  // Test each water level pin
      if (pinIndex < 5) {
        pinMode(WATER_LEVEL_PINS[pinIndex], OUTPUT);
        digitalWrite(WATER_LEVEL_PINS[pinIndex], HIGH);
        
        if (millis() - previousMeasureTime >= MEASURE_WAIT_TIME) {
          previousMeasureTime = millis();
          waterLevelSensorReading[pinIndex] = analogRead(WATER_LEVEL_GROUND_PIN);
          digitalWrite(WATER_LEVEL_PINS[pinIndex], LOW);
          pinMode(WATER_LEVEL_PINS[pinIndex], INPUT);
          
          // If this pin is underwater and we haven't found the water level yet
          if (waterLevelSensorReading[pinIndex] >= waterLevelSensorThreshold && waterLevelPercentage == 0) {
            waterLevelPercentage = WATER_LEVEL_VALUES[pinIndex];
          }
          
          // Skip rest of pins if not collecting raw readings and level found
          if (waterLevelSensorRawReadingsFlag == 0 && waterLevelPercentage != 0) {
            waterLevelMeasureState = 100;  // Mark as finished
            return;
          }
          
          pinIndex++;
        }
      } else {
        waterLevelMeasureState = 100;  // Mark as finished
      }
      break;
  }
}

void measureSoilMoisture() {
  static int readingIndex = 0;
  static unsigned long previousMeasureTime = 0;
  
  switch (soilMoistureMeasureState) {
    case 1:  // Initialize
      digitalWrite(MOISTURE_SENSOR_POWER_PIN, HIGH);  // Power up sensor
      readingIndex = 0;
      soilMoistureAverage = 0;
      previousMeasureTime = millis();
      soilMoistureMeasureState++;
      break;
      
    case 2:  // Take 10 readings
      if (readingIndex < 10 && millis() - previousMeasureTime >= MEASURE_WAIT_TIME) {
        previousMeasureTime = millis();
        soilMoistureReading[readingIndex] = analogRead(MOISTURE_SENSOR_SIGNAL_PIN);
        soilMoistureAverage += soilMoistureReading[readingIndex];
        readingIndex++;
      }
      
      if (readingIndex == 10) {
        soilMoistureMeasureState++;
      }
      break;
      
    case 3:  // Calculate soil moisture
      digitalWrite(MOISTURE_SENSOR_POWER_PIN, LOW);  // Power down sensor
      soilMoistureAverage /= 10;
      
      // Update calibration if flags are set
      if (soilMoistureCalibrationAirFlag == 1) {
        soilMoistureCalibrationAir = soilMoistureAverage;
      }
      if (soilMoistureCalibrationWaterFlag == 1) {
        soilMoistureCalibrationWater = soilMoistureAverage;
      }
      
      // Map sensor reading to calibrated range
      soilMoistureCalibrated = map(soilMoistureAverage, soilMoistureCalibrationWater, soilMoistureCalibrationAir, 1320, 3173);
      
      // Calculate percentage using simplified formula
      soilMoisturePercentage = map(soilMoistureCalibrated, soilMoistureCalibrationAir, soilMoistureCalibrationWater, 0, 100);
      
      // Constrain to valid range
      soilMoisturePercentage = constrain(soilMoisturePercentage, 0, 100);
      
      soilMoistureMeasureState = 100;  // Mark as finished
      break;
  }
}

void operatePump() {
  static float batteryCompensation;
  static int pumpDuration = 10000;  // Default: 10 seconds
  static unsigned long pumpStartTime;
  static int dutyCycle;
  static int minDutyCycle;
  static int maxDutyCycle;
  static int dutyCycleStepTime;
  static unsigned long lastDutyCycleStepTime;
  
  switch (pumpOperationState) {
    case 1:  // Check if pump should run
      if (soilMoisturePercentage <= soilMoistureCritical || 
          pumpPowerMinCalibrationFlag == 1 || 
          pumpPowerMaxCalibrationFlag == 1 || 
          waterFlowCalibrationFlag == 1) {
            
        // Check water and battery levels
        if (waterLevelPercentage < 10) {
          Serial.println("Water level too low - minimum 10% required");
          pumpOperationState = 100;  // Skip pump operation
        }
        else if (batteryLevelPercentage < 10) {
          Serial.println("Battery level too low - minimum 10% required");
          pumpOperationState = 100;  // Skip pump operation
        }
        else {
          pumpOperationState++;  // Continue to pump setup
        }
      }
      else {
        Serial.println("No need to start water pump");
        pumpOperationState = 100;  // Skip pump operation
      }
      break;
      
    case 2:  // Configure pump
      // Setup PWM for pump control
      ledcSetup(PUMP_PWM_CHANNEL, PUMP_PWM_FREQUENCY, PUMP_PWM_RESOLUTION);
      ledcAttachPin(PUMP_POWER_PIN, PUMP_PWM_CHANNEL);
      
      // Calculate voltage compensation based on battery level
      batteryCompensation = -0.238 * batteryLevelVoltage + 1.833;
      
      // Set PWM duty cycle range based on calibration flags
      if (pumpPowerMinCalibrationFlag == 1) {
        // Fixed duty cycle for min power calibration
        minDutyCycle = pumpPowerMin * batteryCompensation;
        maxDutyCycle = minDutyCycle + 1;
      }
      else if (pumpPowerMaxCalibrationFlag == 1) {
        // Fixed duty cycle for max power calibration
        minDutyCycle = pumpPowerMax * batteryCompensation;
        maxDutyCycle = minDutyCycle + 1;
      }
      else {
        // Normal operation with ramp-up
        minDutyCycle = pumpPowerMin * batteryCompensation;
        maxDutyCycle = pumpPowerMax * batteryCompensation;
        
        // Set pump duration based on water amount
        if (waterFlowCalibrationFlag == 1) {
          pumpDuration = 60000;  // 60 seconds for calibration
        }
        else {
          // Calculate duration based on desired water amount
          pumpDuration = 60000 * waterAmount / waterFlowCalibration;
          Serial.printf("Amount of water to pump: %d mL\n", waterAmount);
        }
      }
      
      // Calculate time between duty cycle steps
      dutyCycleStepTime = pumpDuration / (maxDutyCycle - minDutyCycle);
      
      Serial.printf("Pump duration: %d ms\n", pumpDuration);
      Serial.printf("Starting PWM duty cycle: %d\n", minDutyCycle);
      Serial.printf("Final PWM duty cycle: %d\n", maxDutyCycle);
      
      dutyCycle = minDutyCycle;
      pumpStartTime = millis();
      lastDutyCycleStepTime = millis();
      pumpActivityFlag = 1;
      
      Serial.println("Starting water pump... (Push button to cancel)");
      pumpOperationState++;
      break;
      
    case 3:  // Run pump with gradual power increase
      if (millis() - pumpStartTime < pumpDuration) {
        // Run pump at current duty cycle
        ledcWrite(PUMP_PWM_CHANNEL, dutyCycle);
        
        // Gradually increase power
        if (dutyCycle < maxDutyCycle && millis() - lastDutyCycleStepTime >= dutyCycleStepTime) {
          lastDutyCycleStepTime = millis();
          dutyCycle++;
        }
      }
      
      // Stop pump when duration reached or button pressed
      if (millis() - pumpStartTime >= pumpDuration || digitalRead(BUTTON_PIN) == LOW) {
        ledcWrite(PUMP_PWM_CHANNEL, 0);  // Turn off pump
        Serial.println("Pump operation complete or manually cancelled");
        pumpOperationState = 100;  // Mark as finished
      }
      break;
  }
}

void enterDeepSleep() {
  Blynk.disconnect();
  WiFi.mode(WIFI_OFF);
  adc_power_off();
  
  // Enable wakeup sources
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, LOW);  // Button press wakeup
  esp_sleep_enable_timer_wakeup(sleepDuration * MINUTES_TO_SECONDS * SECONDS_TO_MICROSECONDS);
  
  Serial.printf("Going to sleep for %d minutes or until button press...\n", sleepDuration);
  esp_deep_sleep_start();
}

void printSensorValues() {
  Serial.printf("Battery: %d%% (%.2fV)\n", batteryLevelPercentage, batteryLevelVoltage);
  Serial.printf("Water level: %d%%\n", waterLevelPercentage);
  Serial.printf("Soil moisture: %d%%\n", soilMoisturePercentage);
  
  if (waterLevelSensorRawReadingsFlag == 1) {
    Serial.println("Water level raw readings:");
    for (int i = 0; i < 5; i++) {
      Serial.printf("  %d%%: %d\n", WATER_LEVEL_VALUES[i], waterLevelSensorReading[i]);
    }
  }
}

// Blynk connection handler
BLYNK_CONNECTED() {
  if (blynkSyncRequired) {
    Serial.println("Syncing with Blynk server");
    // Download all config values from Blynk
    Blynk.syncVirtual(V105, V106, V107, V0, V1, V2, V10, V3, V4, V5, V6, V7, V8, V9, V11);
  } 
  else {
    Serial.println("Uploading values to Blynk server");
    // Upload sensor readings to Blynk
    Blynk.virtualWrite(V102, batteryLevelPercentage);  // Battery level
    Blynk.virtualWrite(V101, waterLevelPercentage);    // Water level
    Blynk.virtualWrite(V100, soilMoisturePercentage);  // Soil moisture
    
    // Upload raw water level readings if requested
    if (waterLevelSensorRawReadingsFlag == 1) {
      for (int i = 0; i < 5; i++) {
        Blynk.virtualWrite(V12 + i, waterLevelSensorReading[i]);
      }
      Blynk.virtualWrite(V11, 0);  // Reset flag
    }
    
    // Upload calibration values if flags are set
    if (soilMoistureCalibrationAirFlag == 1) {
      Blynk.virtualWrite(V5, soilMoistureCalibrationAir);
      Blynk.virtualWrite(V7, 0);  // Reset flag
    }
    
    if (soilMoistureCalibrationWaterFlag == 1) {
      Blynk.virtualWrite(V6, soilMoistureCalibrationWater);
      Blynk.virtualWrite(V8, 0);  // Reset flag
    }
    
    // Upload pump activity status
    Blynk.virtualWrite(V103, pumpActivityFlag);
    
    // Reset calibration flags on server
    if (pumpPowerMinCalibrationFlag == 1 || pumpPowerMaxCalibrationFlag == 1 || waterFlowCalibrationFlag == 1) {
      Blynk.virtualWrite(V4, 0);
      Blynk.virtualWrite(V2, 0);
      Blynk.virtualWrite(V10, 0);
    }
    
    // Upload boot count
    Blynk.virtualWrite(V104, bootCount);
  }
}

// Blynk handlers for receiving configuration values
BLYNK_WRITE(V0) {
  pumpPowerMin = param.asInt();
  Serial.printf("Updated min pump power: %d\n", pumpPowerMin);
  blynkSyncCounter++;
}

BLYNK_WRITE(V1) {
  pumpPowerMax = param.asInt();
  Serial.printf("Updated max pump power: %d\n", pumpPowerMax);
  blynkSyncCounter++;
}

BLYNK_WRITE(V2) {
  pumpPowerMinCalibrationFlag = param.asInt();
  Serial.printf("Updated min pump power calibration flag: %d\n", pumpPowerMinCalibrationFlag);
  blynkSyncCounter++;
}

BLYNK_WRITE(V3) {
  waterFlowCalibration = param.asInt();
  Serial.printf("Updated water flow calibration: %d\n", waterFlowCalibration);
  blynkSyncCounter++;
}

BLYNK_WRITE(V4) {
  waterFlowCalibrationFlag = param.asInt();
  Serial.printf("Updated water flow calibration flag: %d\n", waterFlowCalibrationFlag);
  blynkSyncCounter++;
}

BLYNK_WRITE(V5) {
  soilMoistureCalibrationAir = param.asInt();
  Serial.printf("Updated soil moisture calibration (air): %d\n", soilMoistureCalibrationAir);
  blynkSyncCounter++;
}

BLYNK_WRITE(V6) {
  soilMoistureCalibrationWater = param.asInt();
  Serial.printf("Updated soil moisture calibration (water): %d\n", soilMoistureCalibrationWater);
  blynkSyncCounter++;
}

BLYNK_WRITE(V7) {
  soilMoistureCalibrationAirFlag = param.asInt();
  Serial.printf("Updated soil moisture calibration (air) flag: %d\n", soilMoistureCalibrationAirFlag);
  blynkSyncCounter++;
}

BLYNK_WRITE(V8) {
  soilMoistureCalibrationWaterFlag = param.asInt();
  Serial.printf("Updated soil moisture calibration (water) flag: %d\n", soilMoistureCalibrationWaterFlag);
  blynkSyncCounter++;
}

BLYNK_WRITE(V9) {
  waterLevelSensorThreshold = param.asInt();
  Serial.printf("Updated water level sensor threshold: %d\n", waterLevelSensorThreshold);
  blynkSyncCounter++;
}

BLYNK_WRITE(V10) {
  pumpPowerMaxCalibrationFlag = param.asInt();
  Serial.printf("Updated max pump power calibration flag: %d\n", pumpPowerMaxCalibrationFlag);
  blynkSyncCounter++;
}

BLYNK_WRITE(V11) {
  waterLevelSensorRawReadingsFlag = param.asInt();
  Serial.printf("Updated water level raw readings flag: %d\n", waterLevelSensorRawReadingsFlag);
  blynkSyncCounter++;
}

BLYNK_WRITE(V104) {
  int bootCountServer = param.asInt();
  if (bootCount == bootCountServer) {
    Serial.println("Upload to Blynk server confirmed");
    uploadBlynkState = 100;  // Mark upload as complete
  }
}

BLYNK_WRITE(V105) {
  sleepDuration = param.asInt();
  Serial.printf("Updated sleep duration: %d minutes\n", sleepDuration);
  blynkSyncCounter++;
}

BLYNK_WRITE(V106) {
  soilMoistureCritical = param.asInt();
  Serial.printf("Updated critical soil moisture: %d%%\n", soilMoistureCritical);
  blynkSyncCounter++;
}

BLYNK_WRITE(V107) {
  waterAmount = param.asInt();
  Serial.printf("Updated water amount: %d mL\n", waterAmount);
  blynkSyncCounter++;
}
