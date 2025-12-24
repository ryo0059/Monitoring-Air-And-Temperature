#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include "time.h"
#include <FS.h>
#include <LittleFS.h> 

// ===============================================
// 1. KONFIGURASI PRIBADI (ISI DATA ANDA)
// ===============================================
const char* ssid = "Ryoga";         // Ganti Nama WiFi
const char* password = "QwerAsd123"; // Ganti Password WiFi

#define BOTtoken "8297319581:AAFcjyLYy0XTrqLOFA0TXiWejwRSRxx-DYk"            // Ganti Token Bot
#define CHAT_ID "1185941562"               // Ganti Chat ID

// ===============================================
// 2. KONFIGURASI SENSOR & PIN
// ===============================================
const int MQ_PIN = 34; // Pin Analog MQ (ADC1)
const int DHT_PIN = 18; // Pin Data DHT22
#define DHTTYPE DHT22

DHT dht(DHT_PIN, DHTTYPE);
WiFiClientSecure client;
UniversalTelegramBot bot(BOTtoken, client);

// ===============================================
// 3. KALIBRASI SENSOR (SESUAI REQUEST TERAKHIR)
// ===============================================
// R0 Baru (Agar CO2 ~400ppm di ruangan Anda)
const float R_ZERO = 14700.0; 

// Koefisien Gas (Zero-ing untuk NH3 & Benzena)
const float A_CO2 = 116.602; const float B_CO2 = -2.769;
const float A_NH3 = 0.2;     const float B_NH3 = -2.0;
const float A_BENZ = 0.1;    const float B_BENZ = -3.35;

// Batas Ambang Bahaya
const float THRESHOLD_CO2 = 1000.0; 
const float THRESHOLD_NH3 = 25.0;   
const float THRESHOLD_BENZ = 1.0; 

// Konstanta Hardware
const float VCC = 3.3; 
const float ADC_RESOLUTION = 4095.0;
const long R_LOAD = 10000; // Resistor 10k Ohm
const float MIN_RS_RO = 0.358; 
const float MAX_RS_RO = 2.428;

// ===============================================
// 4. VARIABEL SISTEM
// ===============================================
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 25200; // WIB
const int   daylightOffset_sec = 0;

unsigned long lastTimeBotRan = 0;
unsigned long lastTimeCsvSent = 0;
const unsigned long csvInterval = 10 * 60 * 1000; // 10 Menit

int nomorLaporan = 1;
String csvFileName = "/data_log.csv";

// ===============================================
// FUNGSI MATEMATIKA
// ===============================================
float calculateRs(int adcRaw) {
    if(adcRaw == 0) return 0;
    float V_out = (float)adcRaw * (VCC / ADC_RESOLUTION);
    // Rumus Voltage Divider 5V ke 3.3V
    float R_s = R_LOAD * (5.0 / (V_out * (5.0/3.3)) - 1.0);
    return R_s;
}

float calculatePPM(float Rs_Ro, float a, float b) {
    if (Rs_Ro <= 0) return 0;
    return a * pow(Rs_Ro, b);
}

// ===============================================
// FUNGSI WAKTU & FILE SYSTEM
// ===============================================
String getCurrentTime() {
    struct tm timeinfo;
    if(!getLocalTime(&timeinfo)){
        return "N/A";
    }
    char timeStringBuff[50];
    strftime(timeStringBuff, sizeof(timeStringBuff), "%d/%m/%Y %H:%M:%S", &timeinfo);
    return String(timeStringBuff);
}

void logDataToCSV(String dataLine) {
    File file = LittleFS.open(csvFileName, FILE_APPEND);
    if(!file){
        Serial.println("Gagal buka file CSV");
        return;
    }
    file.println(dataLine);
    file.close();
    Serial.println("Data tersimpan di memori.");
}

bool sendCSVToTelegram() {
    Serial.println("Mengirim CSV ke Telegram...");
    File file = LittleFS.open(csvFileName, "r");
    if (!file || file.size() == 0) {
        if(file) file.close();
        return false;
    }

    String host = "api.telegram.org";
    if (!client.connect(host.c_str(), 443)) {
        Serial.println("Koneksi Telegram Gagal");
        file.close();
        return false;
    }

    String boundary = "---Esp32Boundary";
    String head = "--" + boundary + "\r\nContent-Disposition: form-data; name=\"chat_id\"\r\n\r\n" + CHAT_ID + "\r\n";
    head += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"document\"; filename=\"sensor_log.csv\"\r\nContent-Type: text/csv\r\n\r\n";
    String tail = "\r\n--" + boundary + "--\r\n";
    
    client.println("POST /bot" + String(BOTtoken) + "/sendDocument HTTP/1.1");
    client.println("Host: " + host);
    client.println("Content-Length: " + String(head.length() + file.size() + tail.length()));
    client.println("Content-Type: multipart/form-data; boundary=" + boundary);
    client.println();
    client.print(head);

    static uint8_t buf[512];
    while (file.available()) {
        client.write(buf, file.read(buf, 512));
    }
    client.print(tail);
    file.close();

    String response = client.readString();
    return (response.indexOf("\"ok\":true") != -1);
}

// ===============================================
// LOGIKA UTAMA SENSOR
// ===============================================
void processSensorData() {
    // FITUR PEMANASAN SUDAH DIHAPUS DI SINI
    // DATA LANGSUNG DIOLAH

    // 1. BACA SENSOR
    float h = dht.readHumidity();
    float t = dht.readTemperature();
    int adcRaw = analogRead(MQ_PIN);

    if (isnan(h) || isnan(t)) {
        Serial.println("DHT Error");
        return;
    }

    // 2. HITUNG PPM
    float R_s = calculateRs(adcRaw);
    float Rs_Ro = R_s / R_ZERO;
    
    float ppm_co2 = calculatePPM(Rs_Ro, A_CO2, B_CO2);
    float ppm_nh3 = calculatePPM(Rs_Ro, A_NH3, B_NH3);
    float ppm_benz = calculatePPM(Rs_Ro, A_BENZ, B_BENZ);

    // 3. FILTER NILAI ERROR (Agar tampilan rapi)
    if (Rs_Ro > MAX_RS_RO || Rs_Ro < MIN_RS_RO) {
        ppm_co2 = 400; 
        ppm_nh3 = 0; 
        ppm_benz = 0;
    }

    // 4. STATUS UDARA
    String statusUdara = "✅ AMAN";
    String statusCSV = "AMAN";
    
    if (ppm_benz > THRESHOLD_BENZ) {
        statusUdara = "☠️ BAHAYA! BENZENA";
        statusCSV = "BAHAYA_BENZ";
    } else if (ppm_nh3 > THRESHOLD_NH3) {
        statusUdara = "☣️ BAHAYA! AMONIA";
        statusCSV = "BAHAYA_NH3";
    } else if (ppm_co2 > THRESHOLD_CO2) {
        statusUdara = "⚠️ WASPADA CO2";
        statusCSV = "WASPADA_CO2";
    }

    // 5. KIRIM KE TELEGRAM (Realtime)
    String msg = "📊 *LAPORAN REALTIME* 📊\n";
    msg += "No: " + String(nomorLaporan) + "\n";
    msg += "🕒 " + getCurrentTime() + "\n";
    msg += "------------------------\n";
    msg += "🌡️ " + String(t, 1) + "°C | 💧 " + String(h, 1) + "%\n";
    msg += "🌫️ CO2: " + String(ppm_co2, 0) + " ppm\n";
    msg += "🤢 NH3: " + String(ppm_nh3, 2) + " ppm\n";
    msg += "🧪 Benzena: " + String(ppm_benz, 2) + " ppm\n";
    msg += "📢 Status: " + statusUdara;

    if(WiFi.status() == WL_CONNECTED) {
        bot.sendMessage(CHAT_ID, msg, "Markdown");
        Serial.println("Pesan Telegram terkirim.");
    }

    // 6. SIMPAN KE CSV
    String csvLine = String(nomorLaporan) + "," + getCurrentTime() + "," + 
                     String(t,1) + "," + String(h,1) + "," + 
                     String(ppm_co2,0) + "," + String(ppm_nh3,2) + "," + 
                     String(ppm_benz,2) + "," + statusCSV;
    logDataToCSV(csvLine);

    nomorLaporan++;
}

// ===============================================
// SETUP & LOOP
// ===============================================
void setup() {
    Serial.begin(115200);

    // Mount LittleFS
    if(!LittleFS.begin(true)){
        Serial.println("LittleFS Gagal");
        return;
    }
    
    // Header CSV
    if(!LittleFS.exists(csvFileName)) {
        File f = LittleFS.open(csvFileName, FILE_WRITE);
        f.println("No,Waktu,Suhu,Kelembapan,CO2,NH3,Benzena,Status");
        f.close();
    }

    dht.begin();
    pinMode(MQ_PIN, INPUT);

    // Koneksi WiFi
    Serial.print("Konek WiFi: ");
    Serial.println(ssid);
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        Serial.print(".");
        delay(500);
    }
    Serial.println("\nWiFi OK!");

    // Set Waktu & SSL
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    client.setInsecure(); 
}

void loop() {
    unsigned long now = millis();

    // TUGAS 1: BACA SENSOR & KIRIM CHAT (Setiap 30 Detik)
    if (now - lastTimeBotRan > 30000) {
        if(WiFi.status() == WL_CONNECTED) {
            processSensorData();
        } else {
            WiFi.reconnect();
        }
        lastTimeBotRan = now;
    }

    // TUGAS 2: KIRIM FILE CSV (Setiap 10 Menit)
    if (now - lastTimeCsvSent > csvInterval) {
        if (WiFi.status() == WL_CONNECTED) {
            if (sendCSVToTelegram()) {
                // Hapus file lama jika sukses kirim
                LittleFS.remove(csvFileName);
                File f = LittleFS.open(csvFileName, FILE_WRITE);
                f.println("No,Waktu,Suhu,Kelembapan,CO2,NH3,Benzena,Status");
                f.close();
                Serial.println("CSV Terkirim & Memori Direset.");
            }
        }
        lastTimeCsvSent = now;
    }
}