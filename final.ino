#include "Wire.h"
#include <U8g2lib.h>
#include <ESP32Encoder.h>

// ==================== 引脚定义 ====================
#define OLED_SCL    15
#define OLED_SDA    16
#define SERVO1_PIN  10
#define SERVO2_PIN  11
#define BUTTON_PIN   2
#define ENCODER_A    4
#define ENCODER_B    5
#define LIGHT_PIN   20

// ==================== 常量 ====================
#define CPR         80          // 编码器一圈脉冲数
#define LIGHT_THRESHOLD  2000   // 低光照阈值（需实测调整）
#define DEBOUNCE_DELAY   200    // 按钮消抖 (ms)
#define PET_COOLDOWN    10    // 摸头冷却 (秒)
#define FULL_DURATION    120   // 吃饱持续 (秒)
#define MOOD_DECAY       30    // 心情衰减间隔 (秒)
#define STATUS_INTERVAL  5     // 状态打印间隔 (秒)

// ==================== 全局状态 ====================
// 心情系统
int  emotion = 5;                    // 心情值 1~10
bool isFull   = false;               // 饱食状态
//unsigned long fullStartTime = 0;     // 吃饱开始时刻
int fullRemainingSeconds = 0;        // 吃饱剩余时间（秒），仅在 isFull == true 时有意义
unsigned long lastFullTick = 0;      // 上次递减剩余时间的时刻（用于每秒更新）
unsigned long lastMoodDecay   = 0;   // 上次心情衰减时刻

// 睡眠系统
volatile bool isSleeping = false;
unsigned long lowLightStart = 0;     // 开始低光照的时刻

// 运动系统
int  currentMotion = 0;              // 0~4 动作编号
bool motionRunning  = false;         // 是否正在执行动作
unsigned long motionStartTime = 0;   // 动作开始时刻
unsigned long lastAutoCheck = 0;     //上次尝试自动触发时刻

// 摸头冷却
unsigned long lastHeadTouch = 0;

// 串口状态打印
unsigned long lastStatusPrint = 0;

// ==================== 对象 ====================
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, OLED_SCL, OLED_SDA);
ESP32Encoder encoder;

// ==================== 中断相关 ====================
volatile bool turnDetected = false;      // 喂食触发
volatile bool buttonPressed = false;    // 按钮按下标志
volatile long lastEncoderCount = 0;
volatile unsigned long lastEncoderTime = 0;
const unsigned long ENCODER_TIMEOUT = 200; // 编码器停顿超时 (ms)

// 编码器中断
void IRAM_ATTR encoderISR() {
  long count = encoder.getCount();
  lastEncoderTime = millis();
  long diff = count - lastEncoderCount;
  if (diff >= 0.9*CPR) {
    turnDetected = true;
    lastEncoderCount += CPR;
  }
}

// 按钮中断
void IRAM_ATTR buttonISR() {
  static unsigned long lastDebounce = 0;
  unsigned long now = millis();
  if (now - lastDebounce > DEBOUNCE_DELAY) {
    buttonPressed = true;
    lastDebounce = now;
  }
}

// ==================== 初始化 ====================
void setup() {
  Serial.begin(115200);
  
  // OLED
  u8g2.begin();
  u8g2.enableUTF8Print();
  u8g2.setFont(u8g2_font_wqy12_t_gb2312);
  u8g2.clear();
  
  // ADC
  //analogReadResolution(12);
  pinMode(LIGHT_PIN, INPUT_PULLUP);
  
  // 舵机
  ledcAttach(SERVO1_PIN, 50, 14);
  ledcAttach(SERVO2_PIN, 50, 14);
  servoNeutral();  // 中立位
  
  // 编码器
  encoder.attachFullQuad(ENCODER_A, ENCODER_B);
  encoder.clearCount();
  lastEncoderCount = 0;
  attachInterrupt(digitalPinToInterrupt(ENCODER_A), encoderISR, CHANGE);
  
  // 按钮
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), buttonISR, FALLING);
  
  // 时间初始化
  lastMoodDecay = millis();
  Serial.println("[0 ms] 智能桌宠启动");
  randomSeed(esp_random());
}

// ==================== 舵机控制 ====================
void servoWrite(int pin, int angle) {
  angle = constrain(angle, 0, 180);
  int duty = map(angle, 0, 180, 410, 2048);
  ledcWrite(pin, duty);
}

void servoNeutral() {
  servoWrite(SERVO1_PIN, 90);
  servoWrite(SERVO2_PIN, 90);
}

void stopMotion() {
  motionRunning = false;
  currentMotion = 0;
  servoNeutral();
}

// ==================== 动作执行（非阻塞） ====================
void updateMotion() {
  if (!motionRunning) return;
  
  unsigned long elapsed = millis() - motionStartTime;
  
  switch (currentMotion) {
    case 1: // 玩耍 - 快速摆动 3 次，2 秒
      playMotion(elapsed);
      break;
    case 2: // 闲逛 - 缓慢摆动 3 次，4 秒
      idleMotion(elapsed);
      break;
    case 3: // 疲惫 - 单臂摆动 2 次，3 秒
      tireMotion(elapsed);
      break;
    case 4: // 前进 - 反向摆动 3 次，4 秒
      fowdMotion(elapsed);
      break;
  }
}

void playMotion(unsigned long t) {
  if (t > 2000) { stopMotion(); return; }
  float phase = t / 2000.0 * 6 * PI;  // 3 个完整周期
  int angle = 90 + 45 * sin(phase);
  servoWrite(SERVO1_PIN, angle);
  servoWrite(SERVO2_PIN, 180-angle);
}

void idleMotion(unsigned long t) {
  if (t > 4000) { stopMotion(); return; }
  float phase = t / 4000.0 * 6 * PI;
  int angle = 90 - 30 * (1 - cos(phase));  // 映射 -60°~0° 即 30°~90°
  servoWrite(SERVO1_PIN, angle);
  servoWrite(SERVO2_PIN, 180-angle);
}

void tireMotion(unsigned long t) {
  if (t > 3000) { stopMotion(); return; }
  float phase = t / 3000.0 * 4 * PI;
  int angle = 90 + 40 + 10 * sin(phase);  // 30°~50° 即 120°~140°
  servoWrite(SERVO1_PIN, constrain(angle, 120, 140));
  servoWrite(SERVO2_PIN, 90);
}

void fowdMotion(unsigned long t) {
  if (t > 4000) { stopMotion(); return; }
  float phase = t / 4000.0 * 6 * PI;
  int angleA = 90 - 35 * (1 - cos(phase));
  int angleB = 90 - 35 * (1 + cos(phase));
  servoWrite(SERVO1_PIN, angleA);
  servoWrite(SERVO2_PIN, 180-angleB);
}

void startMotion(int mot) {
  stopMotion();
  currentMotion = mot;
  motionRunning = true;
  motionStartTime = millis();
  Serial.printf("[%lu ms] Motion %d\n", millis(), mot);
}

// ==================== 摸头处理 ====================
void headTouch() {
  unsigned long now = millis();
  if (!isSleeping) {
    // 清醒状态：正常摸头
    if (now - lastHeadTouch > PET_COOLDOWN * 1000) {
      emotion = constrain(emotion + 1, 1, 10);
      lastHeadTouch = now;
      startMotion(1);  // 玩耍
      Serial.printf("[%lu ms] Head Touch\n", millis());
    }
  } else {
    // 睡眠状态：唤醒
    isSleeping = false;
    lowLightStart = 0;
    lastMoodDecay = millis();
    servoNeutral();
    Serial.printf("[%lu ms] Sleep Mode Off\n", millis());
  }
}

// ==================== 喂食处理 ====================
void handleFeed() {
  isFull = true;
  fullRemainingSeconds = FULL_DURATION;   // 重置为 120 秒
  lastFullTick = millis();
  Serial.printf("[%lu ms] Feed\n", millis());
}

// ==================== 睡眠检测 ====================
void checkSleep() {
  int light = analogRead(LIGHT_PIN);
  
  if (light > LIGHT_THRESHOLD) {
    if (lowLightStart == 0) {
      lowLightStart = millis();
    } else if (millis() - lowLightStart > 5000 && !isSleeping) {
      enterSleep();
    }
  } else {
    lowLightStart = 0;  // 重置计时
  }
}

void enterSleep() {
  isSleeping = true;
  u8g2.clear();
  servoWrite(SERVO1_PIN, 90 + 80);  // +80° = 170°
  servoWrite(SERVO2_PIN, 90 - 80);
  Serial.printf("[%lu ms] Sleep Mode On\n", millis());
}

// ==================== OLED 显示 ====================
void updateDisplay() {
  u8g2.firstPage();
  do {
    u8g2.setCursor(0, 15);
    u8g2.print("EMOTION:");
    u8g2.print(emotion);
    
    u8g2.setCursor(0, 35);
    if (isFull && !isSleeping) {
      int mins = fullRemainingSeconds / 60;
      int secs = fullRemainingSeconds % 60;
      u8g2.print("FULL ");
      u8g2.print(mins);
      u8g2.print("Min ");
      u8g2.print(secs);
      u8g2.print("s");
    } else {
      u8g2.print("HUNGRY");
    }
    
    u8g2.setCursor(0, 55);
    const char* motNames[] = {"NULL", "PLAY", "IDLE", "TIRE", "FOWD"};
    u8g2.print(motNames[currentMotion]);
  } while (u8g2.nextPage());
}

// ==================== 串口指令解析 ====================
void checkSerial() {
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if(isSleeping){
      if (cmd.startsWith("setslp false")) {
        isSleeping = false;
        lowLightStart = 0;
        lastMoodDecay = millis();
        servoNeutral();
        Serial.printf("[%lu ms] Sleep Mode Off\n", millis());
      }
      return;
    }
    if (cmd.startsWith("setemo ")) {
      int val = cmd.substring(7).toInt();
      emotion = constrain(val, 1, 10);
    } else if (cmd.startsWith("setful ")) {
      String arg = cmd.substring(7);
      arg.trim();
      if (arg == "true") {
        isFull = true;
        fullRemainingSeconds = FULL_DURATION;   // 重置为 120 秒
        lastFullTick = millis();
        }else if (arg == "false") isFull = false;
    } else if (cmd.startsWith("setmot ")) {
      int val = cmd.substring(7).toInt();
      if (val >= 0 && val <= 4) startMotion(val);
    } else if (cmd.startsWith("setslp true")) {
      enterSleep();
    }
  }
}

// ==================== 状态打印 ====================
void printStatus() {
  Serial.printf("[%lu ms] EMOTION:%d %s MOT:%d\n",
    millis(), emotion,
    isFull ? "FULL" : "HUNGRY",
    currentMotion);
}

// ==================== 主循环 ====================
void loop() {
  unsigned long now = millis();
  
  // --- 编码器超时清零（停顿检测） ---
  if (now - lastEncoderTime > ENCODER_TIMEOUT) {
    lastEncoderCount = encoder.getCount();
  }
  
  // --- 处理中断标志 ---
  if (buttonPressed) {
    Serial.println("Button pressed");
    buttonPressed = false;
    headTouch();
  }
  if (turnDetected && !isSleeping) {
    turnDetected = false;
    handleFeed();
  }
  
  // --- 睡眠检测 ---
  if (!isSleeping) {
    checkSleep();
  }
  
  // --- 如果睡眠，仅响应按钮唤醒和 setslp 指令 ---
  if (isSleeping) {
    checkSerial();  // 只处理串口（包含 setslp）
    delay(50);
    return;
  }
  
  // --- 以下是正常清醒状态的逻辑 ---
  
  // 心情衰减（每30秒）
  if (!isSleeping && !isFull && now - lastMoodDecay >= MOOD_DECAY * 1000) {
    emotion = constrain(emotion - 1, 1, 10);
    lastMoodDecay = now;
  }

  // 吃饱计时（2分钟后转饥饿,每秒递减，仅清醒时）
  if (isFull && !isSleeping) {
    if (now - lastFullTick >= 1000) {
      fullRemainingSeconds--;
      lastFullTick = now;}
    if (fullRemainingSeconds <= 0) {
      isFull = false;
      lastMoodDecay = now;
    }
  }

  
  // 自动动作触发（每秒 3.33% 概率）
  if (!motionRunning && now-lastAutoCheck>=1000) {
    lastAutoCheck = now;
    if (random(10000) < 333) {
      if (emotion >= 5) startMotion(2);   // 闲逛
      else startMotion(3);                // 疲惫
    }
  }
  
  // 更新舵机动作
  updateMotion();
  
  // 更新 OLED（睡眠时不清屏）
  updateDisplay();
  
  // 状态打印（每5秒）
  if (now - lastStatusPrint >= STATUS_INTERVAL * 1000) {
    printStatus();
    lastStatusPrint = now;
  }
  
  // 串口指令
  checkSerial();
  
  delay(20);  // 主循环刷新率 ~50Hz
}