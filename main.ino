#include "Particle.h"

#include "LIS3DH.h"
#include "TinyGPS++.h"

SYSTEM_THREAD(ENABLED);

// Global objects
FuelGauge batteryMonitor;
LIS3DHSPI accel(SPI, A2, WKP);
TinyGPSPlus gps;

void displayInfo();

// This is the name of the Particle event to publish for battery or movement detection events
// It is a private event.
const char * eventName = "accel";

// Various timing constants
const unsigned long MAX_TIME_TO_PUBLISH_MS = 60000; // Only stay awake for 60 seconds trying to connect to the cloud and publish
const unsigned long MAX_TIME_FOR_GPS_FIX_MS = 180000; // Only stay awake for 3 minutes trying to get a GPS fix
const unsigned long TIME_AFTER_PUBLISH_MS = 4000; // After publish, wait 4 seconds for data to go out
const unsigned long TIME_AFTER_BOOT_MS = 5000; // At boot, wait 5 seconds before going to sleep again (after coming online)
const unsigned long TIME_PUBLISH_BATTERY_SEC = 240 * 60; // every 4 hours send a battery update
const unsigned long SERIAL_PERIOD = 5000;
const int MOVEMENT_PUBLISH_DELAY = 4; // number of minutes that must pass before a movement event is published
const int MOVEMENT_SLEEP_DELAY = 4; // number of minutes to sleep after a publish has been sent less than MOVEMENT_PUBLISH_DELAY ago, ignoring movement to wake.

// default 16, 1 is ultra sensitive
const uint8_t movementThreshold = 1;

// Stuff for the finite state machine
enum State {
    ONLINE_WAIT_STATE,
    RESET_STATE,
    RESET_WAIT_STATE,
    PUBLISH_STATE,
    SLEEP_STATE,
    SLEEP_WAIT_STATE,
    BOOT_WAIT_STATE,
    GPS_WAIT_STATE
};
State state = ONLINE_WAIT_STATE;
unsigned long stateTime = 0;
int awake = 0;
int ignoreAccel = 0;
unsigned long lastSerial = 0;
unsigned long startFix = 0;
bool gettingFix = false;
int lastPublish = 0;

void setup() {
    Serial.begin(9600);

    // The GPS module on the AssetTracker is connected to Serial1 and D6
    Serial1.begin(9600);

    // Settings D6 LOW powers up the GPS module
    pinMode(D6, OUTPUT);
    digitalWrite(D6, LOW);
    startFix = millis();
    gettingFix = true;
}

void loop() {
    while (Serial1.available() > 0) {
        if (gps.encode(Serial1.read())) {
            displayInfo();
        }
    }

    switch (state) {
        case ONLINE_WAIT_STATE:
            if (Particle.connected()) {
                state = RESET_STATE;
            }
            if (millis() - stateTime > 5000) {
                stateTime = millis();
                Serial.println("waiting to come online");
            }
            break;

        case RESET_STATE:
            {
                Serial.println("resetting accelerometer");

                LIS3DHConfig config;
                config.setLowPowerWakeMode(16);

                if (!accel.setup(config)) {
                    Serial.println("accelerometer not found");
                    state = SLEEP_STATE;
                    break;
                }

                state = BOOT_WAIT_STATE;
            }
            break;

        case GPS_WAIT_STATE:
            if (gps.location.isValid()) {
                // Got a GPS fix
                state = PUBLISH_STATE;
                break;
            }
            if (millis() - stateTime >= MAX_TIME_FOR_GPS_FIX_MS) {
                Serial.println("failed to get GPS fix");
                state = SLEEP_STATE;
                break;

            }
            break;

        case PUBLISH_STATE:
            if (Particle.connected()) {
                // The publish data contains 3 comma-separated values:
                // whether movement was detected (1) or not (0) The not detected publish is used for battery status updates
                // cell voltage (decimal)
                // state of charge (decimal)
                char data[64];
                float cellVoltage = batteryMonitor.getVCell();
                float stateOfCharge = batteryMonitor.getSoC();
                snprintf(data, sizeof(data), "%d,%.02f,%.02f,%f,%f",
                    awake, cellVoltage, stateOfCharge, gps.location.lat(), gps.location.lng());

                Particle.publish(eventName, data, 60, PRIVATE);
                lastPublish = Time.now();
                Serial.println(data);

                // Wait for the publish to go out
                stateTime = millis();
                state = SLEEP_WAIT_STATE;
            } else {
                // Haven't come online yet
                if (millis() - stateTime >= MAX_TIME_TO_PUBLISH_MS) {
                    // Took too long to publish, just go to sleep
                    state = SLEEP_STATE;
                }
            }
            break;

        case SLEEP_WAIT_STATE:
            if (millis() - stateTime >= TIME_AFTER_PUBLISH_MS) {
                state = SLEEP_STATE;
            }
            break;

        case BOOT_WAIT_STATE:
            if (millis() - stateTime >= TIME_AFTER_BOOT_MS) {
                // To publish the battery stats after boot, set state to PUBLISH_STATE
                // To go to sleep immediately, set state to SLEEP_STATE
                state = GPS_WAIT_STATE;
                stateTime = millis();
            }
            break;

        case SLEEP_STATE:
            // Wait for Electron to stop moving for 2 seconds so we can recalibrate the accelerometer
            accel.calibrateFilter(2000);

            // Sleep
            if (ignoreAccel == 1) {
                Serial.printlnf("Sleeping %d minutes.", MOVEMENT_SLEEP_DELAY);
                delay(500);
                System.sleep(WKP, FALLING, MOVEMENT_SLEEP_DELAY * 60, SLEEP_NETWORK_STANDBY);
            } else {
                Serial.printlnf("Sleeping until movement or timeout.");
                delay(500);
                System.sleep(WKP, RISING, TIME_PUBLISH_BATTERY_SEC, SLEEP_NETWORK_STANDBY);
            }
            ignoreAccel = 0;

            // Woke up
            delay(5000);

            awake = ((accel.clearInterrupt() & LIS3DH::INT1_SRC_IA) != 0);

            // Movement detected, see if we allow publish
            if (awake == 1) {
                Serial.printlnf("Woke from movement.");
                Serial.printlnf("Last publish was at %d", lastPublish);
                Serial.printlnf("Time is now %d", Time.now());
                // if it has been more than 4 minutes allow
                if ((Time.now() - lastPublish) > (MOVEMENT_PUBLISH_DELAY * 60)) {
                    Serial.printlnf("Has been %d minutes. Allow.", MOVEMENT_PUBLISH_DELAY);
                    digitalWrite(D6, LOW);
                    startFix = millis();
                    gettingFix = true;
                    state = GPS_WAIT_STATE;
                    stateTime = millis();
                    break;
                } else {
                    // if less than MOVEMENT_PUBLISH_DELAY minutes, sleep for MOVEMENT_SLEEP_DELAY minutes
                    ignoreAccel = 1;
                    Serial.printlnf("Has not been %d minutes. Sleep.", MOVEMENT_PUBLISH_DELAY);
                    break;
                }

            // Timeout reached, allow publish
            } else {
                Serial.printlnf("Woke from timeout.");
                digitalWrite(D6, LOW);
                startFix = millis();
                gettingFix = true;
                state = GPS_WAIT_STATE;
                stateTime = millis();
                break;
            }

    // switch
    }

// loop
}

void displayInfo() {
    if (millis() - lastSerial >= SERIAL_PERIOD) {
        lastSerial = millis();

        char buf[128];
        if (gps.location.isValid()) {
            snprintf(buf, sizeof(buf), "%f,%f", gps.location.lat(), gps.location.lng());
            if (gettingFix) {
                gettingFix = false;
                unsigned long elapsed = millis() - startFix;
                Serial.printlnf("%lu milliseconds to get GPS fix", elapsed);
            }
        } else {
            strcpy(buf, "no location");
            if (!gettingFix) {
                gettingFix = true;
                startFix = millis();
            }
        }
        Serial.println(buf);
    }
}
