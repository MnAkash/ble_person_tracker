/*
Comment this file out. It is just a minimal test program for BLE scanning.
 * It is not used in the actual project.
*/

// #include <Arduino.h>
// #include <NimBLEDevice.h>
// #include <math.h>

// // === CONFIG ===
// static const char* TARGET_MAC = "dd:88:00:00:13:07";
// float TXPOWER_AT_1M = -59.0f;
// float PATH_LOSS_N   = 2.2f;
// const float RSSI_ALPHA = 0.3f;
// float rssi_ema = NAN;

// static float rssiToDistance(float rssi) {
//   return powf(10.0f, (TXPOWER_AT_1M - rssi) / (10.0f * PATH_LOSS_N));
// }

// // --- Main continuous callbacks (prints only the target beacon) ---
// class MyScanCallbacks : public NimBLEScanCallbacks {
//   void onResult(const NimBLEAdvertisedDevice* adv) override {
//     std::string addr = adv->getAddress().toString();
//     for (auto &c : addr) c = tolower(c);
//     if (addr == TARGET_MAC) {
//       int rssi = adv->getRSSI();
//       if (isnan(rssi_ema)) rssi_ema = rssi;
//       else rssi_ema = RSSI_ALPHA * rssi + (1.0f - RSSI_ALPHA) * rssi_ema;

//       float d_instant = rssiToDistance((float)rssi);
//       float d_smooth  = rssiToDistance(rssi_ema);

//       Serial.printf("[BEACON %s] RSSI=%d | EMA=%.1f | d=%.2f m (EMA=%.2f m)\n",
//                     addr.c_str(), rssi, rssi_ema, d_instant, d_smooth);
//       Serial.flush();
//     }
//   }
//   void onScanEnd(const NimBLEScanResults& results, int reason) override {
//     (void)results; (void)reason;
//   }
// };

// // --- One-shot startup callback: print EVERY device once ---
// class AllDevicesCB : public NimBLEScanCallbacks {
//   void onResult(const NimBLEAdvertisedDevice* adv) override {
//     std::string addr = adv->getAddress().toString();
//     int rssi = adv->getRSSI();
//     std::string name = adv->getName(); // may be empty
//     if (!name.empty()) {
//       Serial.printf("· %s  RSSI=%d  name=%s\n", addr.c_str(), rssi, name.c_str());
//     } else {
//       Serial.printf("· %s  RSSI=%d\n", addr.c_str(), rssi);
//     }
//   }
// };

// MyScanCallbacks* MAIN_CB = nullptr;
// AllDevicesCB     TEST_CB;   // global so its pointer stays valid

// void setup() {
//   // Bring up Serial (USB-CDC) and wait briefly so early prints appear
//   Serial.begin(115200);
//   unsigned long t0 = millis();
//   while (!Serial && (millis() - t0) < 3000) { delay(10); }
//   delay(200);
//   Serial.println("\nBooting…");

//   NimBLEDevice::init("");
//   NimBLEDevice::setPower(ESP_PWR_LVL_P9);

//   NimBLEScan* scan = NimBLEDevice::getScan();
//   scan->setActiveScan(true);
//   scan->setInterval(45);
//   scan->setWindow(45);

//   // ----- 1) One-shot: list ALL devices for the first 5 seconds -----
//   Serial.println("=== Startup test scan: listing devices for 5 s ===");
//   scan->setDuplicateFilter(false);             // unique devices during test
//   scan->setScanCallbacks(&TEST_CB, false);    // print once per device
//   scan->clearResults();
//   scan->start(5 /*seconds*/, false, false);   // blocking 5s scan
//   Serial.printf("=== Test scan complete. Found %d devices. ===\n\n",
//                 scan->getResults().getCount());
//   Serial.flush();

//   // ----- 2) Switch to your normal continuous scan (target beacon only) -----
//   MAIN_CB = new MyScanCallbacks();
//   scan->setScanCallbacks(MAIN_CB, /*wantDuplicates=*/false);
//   scan->setDuplicateFilter(false);
//   scan->clearResults();
//   scan->start(0, false, false); // forever
//   Serial.println("Now scanning forever for target beacon…");
//   Serial.flush();
// }

// void loop() {
//   delay(1000);
// }