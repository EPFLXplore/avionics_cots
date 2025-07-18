/**
 * @file main.cpp
 * @author Eliot Abramo
 */

#include <Arduino.h>
#include <Seeed_HM330X.h>
#include <GyverHX711.h>
#include "soc/rtc.h"
#include "packet_definition.hpp"
#include "Cosco.hpp"
#include "Servo.hpp"
#include "Dust_Driver.hpp"

/**
 * servo id 1 = cam front
 * servo id 2 = drill clapet 
 * mass id 3 = mass drill
 * mass id 4 = mass rover
*/

// Pin definition
#define DRILL_DOUT 16
#define DRILL_SCK   4
#define HD_DOUT    15
#define HD_SCK      2

#define SERVO_DRILL_PIN 13
#define SERVO_DRILL_CHAN 1
#define SERVO_CAM_PIN   12
#define SERVO_CAM_CHAN  0


// Type definitions
Cosco cosco;
Servo_Driver* servo_cam = new Servo_Driver();
Servo_Driver* servo_drill = new Servo_Driver();
Dust* dust = new Dust();

constexpr uint8_t AVG_SIZE = 10;
// LoadCellHX lcDrill(DRILL_DOUT, DRILL_SCK);
// LoadCellHX lcHD   (HD_DOUT,    HD_SCK);

GyverHX711 mass_drill(DRILL_DOUT, DRILL_SCK, HX_GAIN64_A);
GyverHX711 mass_hd   (HD_DOUT,    HD_SCK,   HX_GAIN64_A);

float drillBuf[AVG_SIZE] = {0};
float hdBuf[AVG_SIZE]    = {0};

float offset_drill = 0.0f;
float offset_hd    = 0.0f;

float slope_drill  = 0.01028f;   // g per count
float slope_hd     = 0.01028f;

float weight_drill = 0.0f;
float weight_hd    = 0.0f;

void shift(float *array , int N, float valueIn){  //shifts all array values left and adds valueIn at position N-1
  for(int i = 1; i<N-1 ; i++){
    array[i-1] = array[i];
  }
  array[N-1] = valueIn;
}

float movingAverage(const float *arr, uint8_t n) {
  if(n<=0){return 0;}
  float sum = 0.0f;
  for (uint8_t i = 0; i < n; i++) sum += arr[i];
  return sum / n;
}

void updateDrill() {
    if (!mass_drill.available()) return;
    long raw = mass_drill.read();
    shift(drillBuf, AVG_SIZE, (float) raw);
    float avg = movingAverage(drillBuf, AVG_SIZE);
    weight_drill = (avg - offset_drill) * slope_drill;
}

void updateHD() {
    if (!mass_hd.available()) return;
    long raw = mass_hd.read();
    shift(hdBuf, AVG_SIZE, (float) raw);
    float avg = movingAverage(hdBuf, AVG_SIZE);
    weight_hd = (avg - offset_hd) * slope_hd;
}

void tareScales() {
    offset_drill = mass_drill.read();
    offset_hd    = mass_hd.read();
    mass_drill.tare();
    mass_hd.tare();
    for (uint8_t i = 0; i < AVG_SIZE; ++i) {
        drillBuf[i] = offset_drill;
        hdBuf[i]    = offset_hd;
    }
    delay(100);
}

void setup() {
  // lower CPU clock to 80 MHz (saves power, reduces noise)
  rtc_cpu_freq_config_t cfg;
  rtc_clk_cpu_freq_get_config(&cfg);
  rtc_clk_cpu_freq_to_config(RTC_CPU_FREQ_80M, &cfg);
  rtc_clk_cpu_freq_set_config_fast(&cfg);

  Serial.begin(115200);

  // lcDrill.begin();
  mass_drill.tare();  
  offset_drill = mass_drill.read();

  // lcHD.begin();
  mass_hd.tare();  
  offset_hd = mass_hd.read();

  servo_cam->init(SERVO_CAM_PIN, SERVO_CAM_CHAN);
  servo_drill->init(SERVO_DRILL_PIN, SERVO_DRILL_CHAN);
  dust->init();
}

void loop() {
  Change changeMass = cosco.receive(servo_cam, servo_drill);

  switch (changeMass.id) {

    case MassDrill_Request_ID:
    {
      offset_drill = mass_drill.read();
      mass_drill.tare();
      for (uint8_t i = 0; i < AVG_SIZE; ++i) {
          drillBuf[i] = offset_drill;
      }

      MassPacket drill_change = {
        MassDrill_ID,
        weight_drill
      };

      cosco.sendMassPacket(&drill_change, MassDrill_ID);
      delay(100);
      
      break;
    }

    case MassHD_Request_ID:
    {
      offset_hd    = mass_hd.read();
      mass_hd.tare();
      for (uint8_t i = 0; i < AVG_SIZE; ++i) {
          hdBuf[i]    = offset_hd;
      }
      delay(100);

      MassPacket hd_change = {
        MassHD_ID,
        weight_drill
      };

      cosco.sendMassPacket(&hd_change, MassDrill_ID);

      break;
    }
  }

  updateDrill();
  updateHD();

  static uint32_t lastMass = 0;    // ms
  static uint32_t last_send_dust = 0;

  if (millis() - lastMass >= 1000) {
    lastMass = millis();
    MassPacket drill = {
      MassDrill_ID,
      weight_drill
    };
    cosco.sendMassPacket(&drill, MassDrill_ID);

    MassPacket hd = {
      MassHD_ID,
      weight_hd
    };
    cosco.sendMassPacket(&hd, MassHD_ID);

    if(drill.mass >= 200){
      ServoRequest request = {
        ServoDrill_ID,
        300,
        false
      };
      servo_drill->set_request(request);
      servo_drill->handle_servo();   
    }

    // Serial.printf("Drill: %.2f g | HD: %.2f g\n", drill.mass, hd.mass);
  }

  if (millis() - last_send_dust >= 1000) {
    last_send_dust = millis();
    if(dust->is_alive()){
      DustData dust_packet;
      dust->loop(&dust_packet);
      cosco.sendDustDataPacket(&dust_packet);
    }
  }

  cosco.sendHeartbeat();

    // // ---- Serial helpers ----
    // if (Serial.available()) {
    //     char c = Serial.read();
    //     if (c == 't') { tareScales(); Serial.println(F("Tared.")); }
    //     // if (c == 'd') { Serial.printf("Drill: %.2f g | HD: %.2f g\n", weight_drill, weight_hd); }
    // }
}
