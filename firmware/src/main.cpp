/*
 * Chicken Coop Auto Door — ESP32 Firmware
 *
 * Polls Railway for open/close schedule, drives the JGY-370 motor via L298N,
 * detects end-stops by current spike on ACS712, and reports telemetry back.
 *
 * Fallback chain (if Railway unreachable):
 *   1. Cached schedule from last successful fetch
 *   2. Local astronomical calculation (Dusk2Dawn) + DS3231 RTC time
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <RTClib.h>
#include <Dusk2Dawn.h>

#include "config.h"

// ─── Types ─────────────────────────────────────────────────────────────────────

struct Schedule {
    time_t openTs  = 0;
    time_t closeTs = 0;
    String pendingCommand;   // "open", "close", or ""
    bool   valid   = false;
};

struct RunResult {
    bool completed     = false;
    int  durationMs    = 0;
    int  peakCurrentMa = 0;
    bool stallDetected = false;
};

// ─── Globals ───────────────────────────────────────────────────────────────────

RTC_DS3231 rtc;
bool       rtcOk = false;

Schedule          cachedSchedule;
unsigned long     lastPollMs           = 0;
unsigned long     lastNtpSyncMs        = 0;
bool              ntpSynced            = false;

// Track which scheduled actions have fired today (by day-of-month)
int lastOpenDay  = -1;
int lastCloseDay = -1;

Dusk2Dawn crowthorne(LATITUDE, LONGITUDE, 0.0f);  // returns UTC minutes

// ─── Forward declarations ──────────────────────────────────────────────────────

void     connectWiFi();
void     syncNTP();
time_t   getCurrentEpoch();
bool     isBST(int year, int month, int day);
void     calcLocalSchedule(time_t now, time_t &openTs, time_t &closeTs);
bool     fetchSchedule(Schedule &out);
int      readCurrentMa();
RunResult runMotor(bool openDir);
void     executeAction(const char *action, const char *trigger);
void     sendTelemetry(const char *action, const char *trigger, RunResult r);
void     motorStop();

// ─── WiFi ──────────────────────────────────────────────────────────────────────

void connectWiFi() {
    if (WiFi.status() == WL_CONNECTED) return;

    Serial.printf("WiFi connecting to %s", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
        delay(500);
        Serial.print(".");
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf(" ok (%s)\n", WiFi.localIP().toString().c_str());
    } else {
        Serial.println(" failed");
    }
}

// ─── NTP + RTC ─────────────────────────────────────────────────────────────────

void syncNTP() {
    if (WiFi.status() != WL_CONNECTED) return;

    Serial.print("NTP sync");
    configTime(0, 0, "pool.ntp.org", "time.google.com");

    for (int i = 0; i < 20; i++) {
        delay(500);
        Serial.print(".");
        time_t t = time(nullptr);
        if (t > 1000000000L) {
            Serial.println(" ok");
            ntpSynced = true;
            lastNtpSyncMs = millis();
            if (rtcOk) {
                struct tm *ti = gmtime(&t);
                rtc.adjust(DateTime(ti->tm_year + 1900, ti->tm_mon + 1, ti->tm_mday,
                                    ti->tm_hour, ti->tm_min, ti->tm_sec));
                Serial.println("RTC updated from NTP");
            }
            return;
        }
    }
    Serial.println(" failed");
}

time_t getCurrentEpoch() {
    time_t t = time(nullptr);
    if (t > 1000000000L) return t;          // NTP-set system clock

    if (rtcOk) {
        return (time_t)rtc.now().unixtime(); // DS3231 fallback
    }

    return 0;                                // No time available
}

// ─── BST calculation ───────────────────────────────────────────────────────────

// Returns true if the given UTC date falls within UK British Summer Time.
// BST: last Sunday in March (01:00 UTC) → last Sunday in October (01:00 UTC).
bool isBST(int year, int month, int day) {
    if (month < 3 || month > 10) return false;
    if (month > 3 && month < 10) return true;

    // Find last Sunday: day-of-week for the 1st using Sakamoto's algorithm
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    int y = (month < 3) ? year - 1 : year;
    int dow1 = (y + y/4 - y/100 + y/400 + t[month-1] + 1) % 7; // 0=Sun
    int daysInMonth = (month == 3 || month == 10) ? 31 : 30;
    int lastSunday  = daysInMonth - ((dow1 + daysInMonth - 1) % 7);

    if (month == 3)  return day >= lastSunday;  // BST starts last Sun March
    if (month == 10) return day < lastSunday;   // BST ends last Sun October
    return false;
}

// ─── Local schedule fallback ───────────────────────────────────────────────────

// Calculates today's sunrise/sunset as UTC epoch using Dusk2Dawn.
// Used when Railway has been unreachable long enough that the cached schedule
// is no longer useful.
void calcLocalSchedule(time_t now, time_t &openTs, time_t &closeTs) {
    struct tm *t = gmtime(&now);
    int year  = t->tm_year + 1900;
    int month = t->tm_mon + 1;
    int day   = t->tm_mday;

    // Dusk2Dawn with timezone=0 returns UTC minutes from midnight (no DST flag)
    int sunriseMinUtc = crowthorne.sunrise(year, month, day, false);
    int sunsetMinUtc  = crowthorne.sunset(year, month, day, false);

    // BST offset: Dusk2Dawn doesn't handle DST, so subtract 1h from result
    // to stay in UTC when we're in BST
    // (Dusk2Dawn gives solar times in UTC; adding BST offset to get local
    // would give the wrong UTC. We want UTC, so no adjustment needed here.)

    time_t midnightUtc = (now / 86400L) * 86400L;
    openTs  = midnightUtc + (time_t)sunriseMinUtc * 60;
    closeTs = midnightUtc + (time_t)sunsetMinUtc  * 60;

    Serial.printf("Local schedule: open=%ld close=%ld\n", (long)openTs, (long)closeTs);
}

// ─── Railway fetch ─────────────────────────────────────────────────────────────

bool fetchSchedule(Schedule &out) {
    if (WiFi.status() != WL_CONNECTED) return false;

    WiFiClientSecure client;
    // Skips certificate verification — acceptable for a personal home project.
    // For stricter security, supply the Railway root CA certificate instead.
    client.setInsecure();

    HTTPClient http;
    String url = String(RAILWAY_BASE_URL) + "/api/schedule";
    http.begin(client, url);
    http.addHeader("X-API-Key", API_KEY);
    http.setTimeout(8000);

    int code = http.GET();
    if (code != 200) {
        Serial.printf("Schedule fetch failed: HTTP %d\n", code);
        http.end();
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getString());
    http.end();

    if (err) {
        Serial.printf("JSON parse error: %s\n", err.c_str());
        return false;
    }

    out.openTs  = doc["open_ts"].as<long>();
    out.closeTs = doc["close_ts"].as<long>();
    const char *pending = doc["pending_command"] | "";
    out.pendingCommand  = String(pending ? pending : "");
    out.valid = true;

    Serial.printf("Schedule: open=%ld close=%ld pending=%s\n",
                  (long)out.openTs, (long)out.closeTs, out.pendingCommand.c_str());
    return true;
}

// ─── Current sensing ───────────────────────────────────────────────────────────

int readCurrentMa() {
    // Average 10 samples to reduce ADC noise
    long sum = 0;
    for (int i = 0; i < 10; i++) {
        sum += analogRead(CURRENT_PIN);
        delayMicroseconds(200);
    }
    int adcValue = (int)(sum / 10);
    int rawMa    = (int)((adcValue - ACS712_ZERO_ADC) * 1000.0f / ACS712_COUNTS_PER_A);
    return abs(rawMa);
}

// ─── Motor control ─────────────────────────────────────────────────────────────

void motorStop() {
    digitalWrite(MOTOR_EN,  LOW);
    digitalWrite(MOTOR_IN1, LOW);
    digitalWrite(MOTOR_IN2, LOW);
}

// Runs the motor in the given direction until:
//   (a) stall detected — current spikes above STALL_THRESHOLD_MA for STALL_CONFIRM_MS
//   (b) MAX_RUN_DURATION_MS elapses (hard safety timeout)
RunResult runMotor(bool openDir) {
    RunResult result;

    digitalWrite(MOTOR_IN1, openDir ? HIGH : LOW);
    digitalWrite(MOTOR_IN2, openDir ? LOW  : HIGH);
    digitalWrite(MOTOR_EN,  HIGH);

    unsigned long startMs      = millis();
    unsigned long stallStartMs = 0;
    bool          inStall      = false;

    while (millis() - startMs < MAX_RUN_DURATION_MS) {
        delay(CURRENT_SAMPLE_MS);

        int currentMa = readCurrentMa();
        if (currentMa > result.peakCurrentMa) result.peakCurrentMa = currentMa;

        if (currentMa > STALL_THRESHOLD_MA) {
            if (!inStall) {
                inStall      = true;
                stallStartMs = millis();
            } else if (millis() - stallStartMs >= STALL_CONFIRM_MS) {
                result.stallDetected = true;
                result.completed     = true;
                break;
            }
        } else {
            inStall = false;
        }
    }

    motorStop();
    result.durationMs = (int)(millis() - startMs);
    if (!result.completed) {
        // Timeout reached — worm gear holds position, mark as completed
        result.completed = true;
        Serial.println("Motor stopped: timeout (no stall detected)");
    } else {
        Serial.printf("Motor stopped: stall at %dms, peak %dmA\n",
                      result.durationMs, result.peakCurrentMa);
    }
    return result;
}

// ─── Telemetry ─────────────────────────────────────────────────────────────────

void sendTelemetry(const char *action, const char *trigger, RunResult r) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("Telemetry skipped: offline");
        return;
    }

    JsonDocument doc;
    doc["action"]           = action;
    doc["trigger"]          = trigger;
    doc["action_completed"] = r.completed;
    doc["run_duration_ms"]  = r.durationMs;
    doc["peak_current_ma"]  = r.peakCurrentMa;
    doc["stall_detected"]   = r.stallDetected;
    doc["door_state"]       = (strcmp(action, "open") == 0) ? "open" : "closed";
    doc["firmware_version"] = FIRMWARE_VERSION;

    String body;
    serializeJson(doc, body);

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    String url = String(RAILWAY_BASE_URL) + "/api/telemetry";
    http.begin(client, url);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-API-Key", API_KEY);
    http.setTimeout(8000);

    int code = http.POST(body);
    Serial.printf("Telemetry: HTTP %d\n", code);
    http.end();
}

// ─── Action execution ──────────────────────────────────────────────────────────

void executeAction(const char *action, const char *trigger) {
    bool openDir = (strcmp(action, "open") == 0);
    Serial.printf("Executing: %s (%s)\n", action, trigger);

    RunResult r = runMotor(openDir);
    sendTelemetry(action, trigger, r);

    // Update same-day deduplication tracking
    time_t now = getCurrentEpoch();
    if (now > 0) {
        struct tm *t = gmtime(&now);
        if (openDir) lastOpenDay  = t->tm_mday;
        else         lastCloseDay = t->tm_mday;
    }
}

// Returns true if a scheduled action should fire now.
// Prevents firing more than once per calendar day.
bool shouldTrigger(time_t scheduledTs, bool isOpen) {
    time_t now = getCurrentEpoch();
    if (now == 0 || scheduledTs == 0) return false;

    long delta = (long)now - (long)scheduledTs;
    if (abs(delta) > TRIGGER_WINDOW_SECS) return false;

    // Already fired today for this action?
    struct tm *t = gmtime(&now);
    int today = t->tm_mday;
    if (isOpen  && lastOpenDay  == today) return false;
    if (!isOpen && lastCloseDay == today) return false;

    return true;
}

// ─── Setup ─────────────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== Coop Door Firmware " FIRMWARE_VERSION " ===");

    // Motor pins — start stopped
    pinMode(MOTOR_IN1, OUTPUT);
    pinMode(MOTOR_IN2, OUTPUT);
    pinMode(MOTOR_EN,  OUTPUT);
    motorStop();

    // ACS712 pin (input-only, no pull-up)
    pinMode(CURRENT_PIN, INPUT);

    // DS3231 RTC
    Wire.begin();
    if (rtc.begin()) {
        rtcOk = true;
        if (rtc.lostPower()) {
            Serial.println("RTC lost power — time not set");
        } else {
            DateTime now = rtc.now();
            Serial.printf("RTC time: %04d-%02d-%02d %02d:%02d:%02d\n",
                          now.year(), now.month(), now.day(),
                          now.hour(), now.minute(), now.second());
        }
    } else {
        Serial.println("DS3231 not found — RTC disabled");
    }

    // WiFi + NTP
    connectWiFi();
    syncNTP();
}

// ─── Loop ──────────────────────────────────────────────────────────────────────

void loop() {
    unsigned long now = millis();

    // Reconnect WiFi if dropped
    if (WiFi.status() != WL_CONNECTED) {
        connectWiFi();
        if (WiFi.status() == WL_CONNECTED) syncNTP();
    }

    // Re-sync NTP once per day
    if (ntpSynced && (now - lastNtpSyncMs >= NTP_SYNC_INTERVAL_MS)) {
        syncNTP();
    }

    // Poll on interval (or immediately on first boot)
    if (lastPollMs == 0 || (now - lastPollMs >= POLL_INTERVAL_MS)) {
        lastPollMs = now;

        // Try to get a fresh schedule
        Schedule fresh;
        if (fetchSchedule(fresh)) {
            cachedSchedule = fresh;
        }

        // Determine which schedule to act on
        Schedule &sched = cachedSchedule;

        if (!sched.valid) {
            // No Railway and no cache — build local fallback
            time_t epoch = getCurrentEpoch();
            if (epoch > 0) {
                calcLocalSchedule(epoch, sched.openTs, sched.closeTs);
                sched.pendingCommand = "";
                sched.valid = true;
            } else {
                Serial.println("No time source available — skipping this poll");
                return;
            }
        }

        // Pending manual command takes priority over schedule
        if (sched.pendingCommand == "open") {
            executeAction("open", "manual");
            sched.pendingCommand = "";
        } else if (sched.pendingCommand == "close") {
            executeAction("close", "manual");
            sched.pendingCommand = "";
        } else {
            // Check scheduled open/close windows
            if (shouldTrigger(sched.openTs, true)) {
                executeAction("open", "schedule");
            }
            if (shouldTrigger(sched.closeTs, false)) {
                executeAction("close", "schedule");
            }
        }
    }

    delay(1000);
}
