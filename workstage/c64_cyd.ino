#include <WiFi.h>
#include <TFT_eSPI.h>       
#include <SPI.h>
#include <FS.h>             
#include <SD.h>             
#include <XPT2046_Touchscreen.h> 
#include <IECFileDevice.h>
#include <IECBusHandler.h>
#include <Preferences.h>
Preferences preferences;
uint8_t iec_device_addr_var = 9; 
#undef IEC_DEVICE_ADDRESS
#define IEC_DEVICE_ADDRESS iec_device_addr_var
// =========================================================================
// --- PIN-MAPPING IEC-BUS ---
#define PIN_IEC_ATN   35    
#define PIN_IEC_CLK   22    
#define PIN_IEC_DATA  27    
#define IEC_DEVICE_ADDRESS 9 
// --- PIN-MAPPING HARDWARE V3 (YD2USB) ---
#define SD_CS_PIN      5     
#define TOUCH_CS_PIN  33    
#define TOUCH_IRQ_PIN 36    
#define TOUCH_SCLK    25    
#define TOUCH_MISO    39    
#define TOUCH_MOSI    32    
SPIClass touchSPI(HSPI);
XPT2046_Touchscreen ts(TOUCH_CS_PIN, TOUCH_IRQ_PIN); 
// --- WLAN ---
const char* ssid     = "CBM_PET_NET";  
const char* password = "retrocomputing"; 
WiFiServer tcpServer(8000);
TFT_eSPI tft = TFT_eSPI(); 
char detailsStr[128]; 
#define maximum 65535
uint8_t prgbuffer[maximum]; 
size_t byte_counter = 0;
String current_filename = "";
size_t read_pointer = 0;
enum TransferState { STATE_IDLE, STATE_TRANSFER, STATE_MENU };
TransferState currentState = STATE_IDLE;
#define CBM_BLUE 0x010A  
#define CBM_RED  0xE000  
// --- FILE-BROWSER
#define MAX_TOTAL_FILES 150  
#define FILES_PER_PAGE 4     

struct FileEntry {
    String name;
    bool isDir;
};

FileEntry allFiles[MAX_TOTAL_FILES];
int totalFilesCount = 0;
int currentStartIndex = 0;   
unsigned long lastTouchTime = 0;
String currentPath = "/"; 
void showFileMenu();
bool loadFromSD(String filename);
bool saveToSD(String filename, uint8_t* buffer, size_t length);

void generateC64DirectoryListing() {
    byte_counter = 0;
    read_pointer = 0;

    // 1. Startadresse für BASIC (2049 / 0x0801)
    prgbuffer[byte_counter++] = 0x01;
    prgbuffer[byte_counter++] = 0x08;

    // Das Verzeichnis startet bei 0x0801
    uint16_t currentLineStartAddr = 0x0801; 
    
    // =========================================================================
    // --- ZEILE 0: HEADER ---
    // =========================================================================
    // Platzhalter für den Zeiger auf die nächste Zeile (wird später korrigiert)
    size_t headerNextLinePos = byte_counter;
    byte_counter += 2; 

    prgbuffer[byte_counter++] = 0x00; // Zeilennummer Low (0)
    prgbuffer[byte_counter++] = 0x00; // Zeilennummer High (0)
    
    prgbuffer[byte_counter++] = 0x12; // Reverse ON (Inverser Text)
    prgbuffer[byte_counter++] = '"';
    String title = "SD:" + currentPath;
    title.toUpperCase();
    if(title.length() > 16) title = title.substring(0,16);
    while(title.length() < 16) title += " "; 
    for(int i=0; i<16; i++) prgbuffer[byte_counter++] = title[i];
    prgbuffer[byte_counter++] = '"';
    prgbuffer[byte_counter++] = ' ';
    prgbuffer[byte_counter++] = '0'; // ID
    prgbuffer[byte_counter++] = '1';
    prgbuffer[byte_counter++] = ' ';
    prgbuffer[byte_counter++] = '2'; // DOS-Typ 2A
    prgbuffer[byte_counter++] = 'A';
    prgbuffer[byte_counter++] = 0x00; // Ende Zeile 0

    // Jetzt korrigieren wir den Zeiger für den Header
    currentLineStartAddr += (byte_counter - headerNextLinePos + 2); // +2 weil wir bei headerNextLinePos standen
    prgbuffer[headerNextLinePos]     = currentLineStartAddr & 0xFF;
    prgbuffer[headerNextLinePos + 1] = (currentLineStartAddr >> 8) & 0xFF;

    // =========================================================================
    // --- DATEI-EINTRÄGE ALS BASIC-ZEILEN ---
    // =========================================================================
    File root = SD.open(currentPath);
    if (root) {
        File file = root.openNextFile();
        while (file && byte_counter < (maximum - 64)) { 
            String name = String(file.name());
            int lastSlash = name.lastIndexOf('/');
            if (lastSlash >= 0) name = name.substring(lastSlash + 1);
            name.toUpperCase();

            // Blöcke berechnen (1 Block = 254 Bytes)
            uint16_t blocks = file.isDirectory() ? 0 : (file.size() + 253) / 254;

            // Merke Position für den Zeiger auf die nächste Zeile
            size_t entryNextLinePos = byte_counter;
            byte_counter += 2; // Platzhalter für Zeiger

            // Zeilennummer im C64-Directory entspricht den Blöcken
            prgbuffer[byte_counter++] = blocks & 0xFF;        
            prgbuffer[byte_counter++] = (blocks >> 8) & 0xFF; 

            // C64 Formatierung: Einrückung abhängig von der Block-Anzahl
            if (blocks < 10)   prgbuffer[byte_counter++] = ' ';
            if (blocks < 100)  prgbuffer[byte_counter++] = ' ';
            if (blocks < 1000) prgbuffer[byte_counter++] = ' ';

            // Dateiname in Anführungszeichen
            prgbuffer[byte_counter++] = '"';
            for(unsigned int i=0; i<name.length(); i++) prgbuffer[byte_counter++] = name[i];
            prgbuffer[byte_counter++] = '"';
            
            // Tabs auffüllen für saubere Spalten (C64-Standard nutzt meist max 16 Zeichen)
            int spaces = 16 - name.length();
            if (spaces < 1) spaces = 1;
            for(int i=0; i<spaces; i++) prgbuffer[byte_counter++] = ' ';

            // Typen-Kennung schreiben
            if (file.isDirectory()) {
                prgbuffer[byte_counter++] = 'D'; prgbuffer[byte_counter++] = 'I'; prgbuffer[byte_counter++] = 'R';
            } else {
                prgbuffer[byte_counter++] = 'P'; prgbuffer[byte_counter++] = 'R'; prgbuffer[byte_counter++] = 'G';
            }
            prgbuffer[byte_counter++] = 0x00; // Ende dieser Zeile

            // Zeiger für diesen Eintrag auf die exakte Folgeadresse setzen
            currentLineStartAddr += (byte_counter - entryNextLinePos);
            prgbuffer[entryNextLinePos]     = currentLineStartAddr & 0xFF;
            prgbuffer[entryNextLinePos + 1] = (currentLineStartAddr >> 8) & 0xFF;

            file = root.openNextFile();
        }
        root.close();
    }

    // =========================================================================
    // --- LETZTE ZEILE: BLOCKS FREE ---
    // =========================================================================
    size_t freeNextLinePos = byte_counter;
    byte_counter += 2; // Platzhalter

    prgbuffer[byte_counter++] = 0x00; // 0 Blocks free Low
    prgbuffer[byte_counter++] = 0x00; // 0 Blocks free High
    
    // Wichtig: Führende Leerzeichen, damit es unter den Namen steht
    prgbuffer[byte_counter++] = ' ';
    prgbuffer[byte_counter++] = ' ';
    prgbuffer[byte_counter++] = ' ';
    prgbuffer[byte_counter++] = ' ';
    
    String freeText = "BLOCKS FREE.";
    for(unsigned int i=0; i<freeText.length(); i++) prgbuffer[byte_counter++] = freeText[i];
    prgbuffer[byte_counter++] = 0x00; // Ende der Zeile

    // Letzten Zeiger berechnen
    currentLineStartAddr += (byte_counter - freeNextLinePos);
    prgbuffer[freeNextLinePos]     = currentLineStartAddr & 0xFF;
    prgbuffer[freeNextLinePos + 1] = (currentLineStartAddr >> 8) & 0xFF;

    // =========================================================================
    // --- BASIC ENDE-MARKIERUNG ---
    // =========================================================================
    // Der finale Zeiger auf die "nächste Zeile" muss 0x0000 sein       
    prgbuffer[byte_counter++] = 0x00;
    prgbuffer[byte_counter++] = 0x00;
    read_pointer = 0;
    currentState = STATE_TRANSFER; 
    current_filename = "$";
}

void drawCommodoreLogo(int x, int y) {
    tft.fillCircle(x + 25, y + 40, 35, CBM_BLUE);
    tft.fillCircle(x + 23, y + 40, 35, CBM_BLUE); 
    tft.fillCircle(x + 24, y + 40, 21, TFT_BLACK); 
    tft.fillRect(x + 24, y, 55, 80, TFT_BLACK);
    tft.fillRect(x + 24, y, 10, 2, TFT_BLACK);
    tft.fillRect(x + 24, y + 78, 10, 2, TFT_BLACK);

    tft.fillRect(x + 24, y + 21, 13, 15, CBM_BLUE);
    tft.fillTriangle(x + 37, y + 21,  x + 47, y + 21,  x + 37, y + 28, CBM_BLUE); 
    tft.fillTriangle(x + 37, y + 36,  x + 47, y + 21,  x + 37, y + 28, CBM_BLUE); 

    tft.fillRect(x + 24, y + 44, 13, 15, CBM_RED);
    tft.fillTriangle(x + 37, y + 44,  x + 47, y + 59,  x + 37, y + 51, CBM_RED); 
    tft.fillTriangle(x + 37, y + 59,  x + 47, y + 59,  x + 37, y + 51, CBM_RED); 
}

// =========================================================================
// Main Layout
// =========================================================================
void updateDisplay(String status, String filename, String details) {
    tft.fillScreen(TFT_BLACK);
    drawCommodoreLogo(15, 5);
    
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.setTextSize(2);
    tft.drawString("CBM ACCESS POINT", 70, 35);
    tft.drawFastHLine(70, 52, 215, TFT_DARKGREY);

    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(1);
    tft.drawString("WLAN: " + String(ssid), 70, 62);
    tft.drawString("IP:   192.168.4.1:8000", 70, 75);
    tft.drawFastHLine(10, 90, 300, TFT_DARKGREY);

    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.setTextSize(2);
    tft.drawString(status, 10, 105);

    if (filename.length() > 0) {
        tft.setTextColor(TFT_GREEN, TFT_BLACK);
        tft.setTextSize(1);
        tft.drawString("Datei: " + filename, 10, 135);
    }

    if (details.length() > 0) {
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.setTextSize(1);
        tft.drawString(details, 10, 155);
    }

    tft.fillRect(200, 200, 110, 30, CBM_BLUE);
    tft.drawRect(200, 200, 110, 30, TFT_WHITE);
    tft.setTextColor(TFT_WHITE);
    tft.setTextSize(1);
    tft.drawString("SD BROWSER", 215, 210);

int startX = 10; 
for (uint8_t i = 8; i <= 11; i++) {
    uint16_t btnColor = (iec_device_addr_var == i) ? CBM_BLUE : TFT_BLACK;
    uint16_t textColor = (iec_device_addr_var == i) ? TFT_WHITE : TFT_DARKGREY;
    
    tft.fillRect(startX, 200, 40, 30, btnColor);
    tft.drawRect(startX, 200, 40, 30, TFT_WHITE);
    
    tft.setTextColor(textColor, btnColor);
    tft.setTextSize(1);
    tft.drawString("[" + String(i) + "]", startX + 11, 211);
    startX += 45; 
}
}
// =========================================================================
// SD-KARTEN FUNKTIONEN
// =========================================================================
bool saveToSD(String filename, uint8_t* buffer, size_t length) {
    filename.trim();
    if (!filename.startsWith("/")) filename = "/" + filename;
    filename.replace(" ", "_");

    File file = SD.open(filename, FILE_WRITE);
    if (!file) return false;
    
    size_t written = file.write(buffer, length);
    file.close();
    return (written == length);
}

void readSDFolderContents(String path) {
    totalFilesCount = 0;
    currentStartIndex = 0;

    if (path != "/") {
        allFiles[totalFilesCount].name = ".. [ZURUECK]";
        allFiles[totalFilesCount].isDir = true;
        totalFilesCount++;
    }

    File root = SD.open(path);
    if (!root) return;

    File file = root.openNextFile();
    while (file && totalFilesCount < MAX_TOTAL_FILES) {
        String fullName = String(file.name());
        
        int lastSlash = fullName.lastIndexOf('/');
        String shortName = (lastSlash >= 0) ? fullName.substring(lastSlash + 1) : fullName;

        if (file.isDirectory()) {
            allFiles[totalFilesCount].name = "/" + shortName;
            allFiles[totalFilesCount].isDir = true;
            totalFilesCount++;
        } else {
            allFiles[totalFilesCount].name = shortName;
            allFiles[totalFilesCount].isDir = false;
            totalFilesCount++;
        }
        file = root.openNextFile();
    }
    root.close();
}

// INTELLIGENTE SUCHE (Inklusive Wildcard- und Parameter-Kompensation)
bool loadFromSD(String filename) {
    filename.trim();
    
    // 1. C64-Parameter und Wildcards am Namensende abschneiden (z.B. "LEVEL1,P" oder "LEVEL*")
    if (filename.indexOf(',') > 0) {
        filename = filename.substring(0, filename.indexOf(','));
    }
    if (filename.endsWith("*")) {
        filename = filename.substring(0, filename.length() - 1);
    }
    filename.trim();

       if (filename == "$") {
        generateC64DirectoryListing();
        updateDisplay("Send Directory", "$", String(byte_counter) + " Bytes");
        return true; // Dem System Erfolg signalisieren, Daten liegen im prgbuffer
    }

    if (filename.indexOf(',') > 0) {
        filename = filename.substring(0, filename.indexOf(','));
    }

    String fullPath = currentPath + filename;
    fullPath.replace("//", "/");
    fullPath.replace(" ", "_"); 

    // --- VERSUCH 1: Exakte Dateisuche ---
    File file = SD.open(fullPath, FILE_READ);
    
    // Fallback für Versuch 1: Wenn exakt fehlschlägt, mit UPPERCASE Pfad versuchen
    if (!file) {
        String fullPathUpper = fullPath;
        fullPathUpper.toUpperCase();
        file = SD.open(fullPathUpper, FILE_READ);
    }
    
    // --- VERSUCH 2: Teilsuchen-Fallback (Wildcard-Emulation) ---
    if (!file && filename.length() > 0) {
        File root = SD.open(currentPath);
        if (root) {
            File testFile = root.openNextFile();
            
            // Suchbegriffe für den Teilschnitt vorab in Großbuchstaben wandeln
            String filenameUpper = filename;
            filenameUpper.toUpperCase();
            String prgMatchUpper = filenameUpper + ".PRG";

            while (testFile) {
                if (!testFile.isDirectory()) {
                    String testName = String(testFile.name());
                    int lastSlash = testName.lastIndexOf('/');
                    if (lastSlash >= 0) testName = testName.substring(lastSlash + 1);

                    // Gefundenen Dateinamen für den Vergleich in Großbuchstaben wandeln
                    String testNameUpper = testName;
                    testNameUpper.toUpperCase();

                    // Prüfen, ob Dateiname auf SD-Karte mit dem C64-Suchbegriff beginnt (Case-Insensitive)
                    if (testNameUpper.startsWith(filenameUpper) || testNameUpper == prgMatchUpper) {
                        file = testFile; 
                        break;
                    }
                }
                testFile = root.openNextFile();
            }
            root.close();
        }
    }

    if (!file) {
    
    return false;
      }

    // Daten in den Puffer streamen
    byte_counter = 0;
    size_t bytesRead = 0;
    String finalName = String(file.name());
    int slashIdx = finalName.lastIndexOf('/');
    current_filename = (slashIdx >= 0) ? finalName.substring(slashIdx + 1) : finalName;

while (file.available() && byte_counter < maximum) {
    prgbuffer[byte_counter] = file.read();
    byte_counter++;
    bytesRead++;
}
  
    
    file.close();
    return (byte_counter > 0);
}

// =========================================================================
// TOUCH-MENÜ OBERFLÄCHE
// =========================================================================
void showFileMenu() {
    currentState = STATE_MENU;
    tft.fillScreen(TFT_BLACK);
    
    tft.setTextColor(TFT_DARKGREY);
    tft.setTextSize(1);
    String pathHint = "Pfad: " + currentPath;
    if(pathHint.length() > 38) pathHint = pathHint.substring(0,35) + "..";
    tft.drawString(pathHint, 10, 4);

    tft.setTextColor(TFT_YELLOW);
    tft.setTextSize(2);
    tft.drawString("SD-CARD BROWSER", 10, 14);
    tft.drawFastHLine(10, 31, 300, TFT_DARKGREY);

    for (int i = 0; i < FILES_PER_PAGE; i++) {
        int fileIndex = currentStartIndex + i;
        int yPos = 37 + (i * 32);
        
        if (fileIndex < totalFilesCount) {
            uint16_t btnColor = allFiles[fileIndex].isDir ? 0x31A6 : 0x18C3; 
            
            tft.fillRect(10, yPos, 245, 29, btnColor);
            tft.setTextColor(TFT_WHITE);
            tft.setTextSize(2); 
            
            String displayName = allFiles[fileIndex].name;
            if(displayName.length() > 14) displayName = displayName.substring(0, 12) + "..";
            
            tft.drawString(displayName, 20, yPos + 6);
        } else {
            tft.fillRect(10, yPos, 245, 29, TFT_BLACK);
            tft.setTextColor(TFT_DARKGREY);
            tft.setTextSize(2);
            tft.drawString("[Leer]", 20, yPos + 6);
        }
    }

    // --- ROLLBALKEN ---
    tft.fillRect(265, 37, 20, 125, TFT_DARKGREY); 
    if (totalFilesCount > 0) {
        int barHeight = 125 / max(1, (totalFilesCount - FILES_PER_PAGE + 1));
        int barY = 37 + (currentStartIndex * (125 - barHeight) / max(1, (totalFilesCount - FILES_PER_PAGE)));
        barHeight = constrain(barHeight, 15, 125);
        barY = constrain(barY, 37, 162 - barHeight);
        tft.fillRect(265, barY, 20, barHeight, TFT_CYAN); 
    }

    // --- SCROLL-PFEILE ---
    tft.fillRect(295, 37, 20, 38, CBM_BLUE); 
    tft.drawRect(295, 37, 20, 38, TFT_WHITE);
    tft.setTextColor(TFT_WHITE); tft.setTextSize(2); tft.drawString("^", 301, 48);

    tft.fillRect(295, 124, 20, 38, CBM_BLUE); 
    tft.drawRect(295, 124, 20, 38, TFT_WHITE);
    tft.setTextColor(TFT_WHITE); tft.setTextSize(2); tft.drawString("v", 301, 135);

    tft.fillRect(10, 200, 100, 30, CBM_RED);
    tft.drawRect(10, 200, 100, 30, TFT_WHITE);
    tft.setTextColor(TFT_WHITE); tft.setTextSize(1);
    tft.drawString("HAUPTMENUE", 24, 210);

    tft.setTextColor(TFT_DARKGREY); tft.setTextSize(1);
    int currentEnd = min(totalFilesCount, currentStartIndex + FILES_PER_PAGE);
    String posStr = String(totalFilesCount > 0 ? currentStartIndex + 1 : 0) + "-" + String(currentEnd) + " / " + String(totalFilesCount);
    tft.drawString(posStr, 130, 210);
}

// =========================================================================
// TOUCH-AUSWERTUNG
// =========================================================================
void handleTouch() {
    if (millis() - lastTouchTime < 350) return; 

    if (ts.touched()) {
        TS_Point p = ts.getPoint();

        int t_x = map(p.x, 300, 3850, 0, 320);
        int t_y = map(p.y, 250, 3600, 0, 240);

        t_x = constrain(t_x, 0, 320);
        t_y = constrain(t_y, 0, 240);

        // =================================================================
        // HAUPTBILDSCHIRM (IDLE) - HIER GEHÖREN DIE BUTTONS HIN
        // =================================================================
        if (currentState == STATE_IDLE) {
            
            // 1. Dein originaler SD-Browser Button unten rechts
            if (t_x >= 190 && t_x <= 320 && t_y >= 170 && t_y <= 240) {
                lastTouchTime = millis(); 
                readSDFolderContents(currentPath); 
                showFileMenu();
                return; 
            }

            // 2. KORREKT PLATZIERT: Die 4 Adress-Buttons unten links
            if (t_y >= 200 && t_y <= 230 && t_x < 190) {
                uint8_t gewaehlt = 0;
                if (t_x >= 10 && t_x < 50)        gewaehlt = 8;
                else if (t_x >= 55 && t_x < 95)   gewaehlt = 9;
                else if (t_x >= 100 && t_x < 140) gewaehlt = 10;
                else if (t_x >= 145 && t_x < 185) gewaehlt = 11;

                if (gewaehlt >= 8 && gewaehlt <= 11 && gewaehlt != iec_device_addr_var) {
                    preferences.begin("c64_drive", false);
                    preferences.putUChar("dev_addr", gewaehlt);
                    preferences.end();
                    
                    tft.fillScreen(TFT_BLACK);
                    tft.setTextColor(TFT_YELLOW);
                    tft.setTextSize(2);
                    tft.drawString("REBOOT...", 80, 100);
                    delay(1000);
                    ESP.restart();
                }
                return;
            }
        } 
        // =================================================================
        // DATEIBROWSER-MENÜ (MENU) - BLEIBT JETZT WIEDER UNBERÜHRT
        // =================================================================
        else if (currentState == STATE_MENU) {
            if (t_x >= 0 && t_x <= 120 && t_y >= 190 && t_y <= 240) {
                lastTouchTime = millis();
                currentState = STATE_IDLE;
                snprintf(detailsStr, sizeof(detailsStr), "Bereit fuer LOAD | %zu Bytes", byte_counter);
                updateDisplay("BEREIT / IDLE", current_filename, String(detailsStr));
                return;
            }

            if (t_x >= 290 && t_x <= 320 && t_y >= 30 && t_y <= 80) {
                if (currentStartIndex > 0) {
                    lastTouchTime = millis();
                    currentStartIndex--;
                    showFileMenu();
                }
                return;
            }

            if (t_x >= 290 && t_x <= 320 && t_y >= 120 && t_y <= 170) {
                if (currentStartIndex + FILES_PER_PAGE < totalFilesCount) {
                    lastTouchTime = millis();
                    currentStartIndex++;
                    showFileMenu();
                }
                return;
            }

            // Dateiauswahl-Schleife innerhalb des Menüs
            for (int i = 0; i < FILES_PER_PAGE; i++) {
                int fileIndex = currentStartIndex + i;
                int yMin = 37 + (i * 32);
                int yMax = yMin + 29;

                if (fileIndex < totalFilesCount && t_x >= 10 && t_x <= 255 && t_y >= yMin && t_y <= yMax) {
                    lastTouchTime = millis();
                    
                    if (allFiles[fileIndex].name == ".. [ZURUECK]") {
                        currentPath = currentPath.substring(0, currentPath.length() - 1);
                        int lastSlash = currentPath.lastIndexOf('/');
                        currentPath = currentPath.substring(0, lastSlash + 1);
                        
                        readSDFolderContents(currentPath);
                        showFileMenu();
                        return;
                    }

                    if (allFiles[fileIndex].isDir) {
                        String targetDir = allFiles[fileIndex].name.substring(1); 
                        currentPath = currentPath + targetDir + "/";
                        
                        readSDFolderContents(currentPath);
                        showFileMenu();
                        return;
                    }

                    String selectedFile = allFiles[fileIndex].name;
                    
                    tft.fillRect(10, yMin, 245, 29, TFT_GREEN); 
                    tft.setTextColor(TFT_BLACK);
                    tft.setTextSize(2);
                    tft.drawString(selectedFile.length() > 14 ? selectedFile.substring(0,12)+".." : selectedFile, 20, yMin + 6);
                    delay(250); 

                    if (loadFromSD(selectedFile)) {
                        currentState = STATE_IDLE;
                        snprintf(detailsStr, sizeof(detailsStr), "Geladen aus %s", currentPath.c_str());
                        updateDisplay("BEREIT / RAM OK", current_filename, String(detailsStr));
                    } else {
                        showFileMenu(); 
                    }
                    return; 
                }
            }
        }
    }
}


// =========================================================================
// IEC CLASS (Emuliert Laufwerk mit flexiblem Wildcard-Nachlademodus)
// =========================================================================
class CYD_RAM_Drive : public IECFileDevice {




private:
    // Statische Hilfsfunktion: Liest die Adresse direkt aus dem NVS-Flash,
    // noch BEVOR der Konstruktor der Basisklasse anspringt!
    static uint8_t loadAddressFromFlash() {
        Preferences prefs;
        prefs.begin("c64_drive", true); // "true" öffnet im schreibgeschützten Modus (sicherer)
        uint8_t addr = prefs.getUChar("dev_addr", 9); // Fallback auf 9, falls leer
        prefs.end();
        return addr;
    }


    uint8_t statusState = 0; // 0 = Sende Version, 1 = Sende OK, 2 = Datenende, 3 = Idle

    const char* statusString = "73,CBM_PET_NET V1.0,00,00";   // oder z.B. "00,OK,00,00"
    uint8_t statusPos  = 0;                                   // Leseposition Status
    uint8_t statusLen  = 0;

public:
    // Der Konstruktor holt sich die Adresse nun live aus dem Flash:
    CYD_RAM_Drive() : IECFileDevice(loadAddressFromFlash()) {
        // Synchronisiert unsere globale Anzeigevariable mit der geladenen Adresse
        iec_device_addr_var = loadAddressFromFlash(); 
    }


protected: 
    // =========================================================================
    // 1. HILFSFUNKTION
    // =========================================================================
    void speichereAktuelleDatei() {     
         if (current_filename == "$" || current_filename == "/$") {
          currentState = STATE_IDLE;
         } return;                         
        // 1. Dateinamen säubern (Leerzeichen in Unterstriche, wie in deiner saveToSD Logik)
        current_filename.trim();
        current_filename.replace(" ", "_");

        // 2. Pfad sauber zusammensetzen
        String speicherPfad = currentPath;
        if (!speicherPfad.endsWith("/")) {
            speicherPfad += "/";
        }
        speicherPfad += current_filename;
        
        // Doppelte Slashes entfernen (z.B. "//TEST.PRG" -> "/TEST.PRG")
        speicherPfad.replace("//", "/");

        // 3. Sicherheits-Check auf dem TFT vor dem Schreiben
        updateDisplay("SD SCHREIBEN...", current_filename, "Pfad: " + speicherPfad);
        delay(100); // Kurze Pause, damit du den Pfad auf dem Display lesen kannst

        // 4. Physischer Schreibversuch
        bool erfolg = saveToSD(speicherPfad, prgbuffer, byte_counter);

        if (erfolg) {
            updateDisplay("READY.", current_filename, "Gespeichert (" + String(byte_counter) + "B)");
        } else {
            // DIAGNOSE: Warum schlägt es fehl?
            if (byte_counter == 0) {
                updateDisplay("SD FEHLER!", current_filename, "0 Bytes im RAM-Puffer!");
            } else if (!SD.exists(currentPath)) {
                updateDisplay("SD FEHLER!", current_filename, "Ordner existiert nicht!");
            } else {
                updateDisplay("SD FEHLER!", current_filename, "Schreibschutz/Karte voll?");
            }
        }
        
        // Zähler nach dem Schreibversuch nullen
        byte_counter = 0; 
    }
       // =========================================================================
    // 2. OPEN-METHODE (KORRIGIERT FÜR FEHLERKANAL & DIRECTORY)
    // =========================================================================
    virtual bool open(uint8_t chan, const char* name, uint8_t len) override {
        
        // Name extrahieren und säubern
        String requested_file = "";
        for(int i = 0; i < len; i++) {
            if (name[i] != 0x00) {
                requested_file += (char)name[i];
            }
        }
        requested_file.trim();

        if (requested_file.indexOf(',') > 0) {
            requested_file = requested_file.substring(0, requested_file.indexOf(','));
        }
        if (requested_file.endsWith("*")) {
            requested_file = requested_file.substring(0, requested_file.length() - 1);
        }
        requested_file.trim();

        // WICHTIG: Wenn der C64 das Directory ($) anfordert
        if (requested_file == "$") {
            generateC64DirectoryListing(); // Erzeugt das Verzeichnis im RAM
            statusState = 1;               // Fehlerkanal auf "00,OK" setzen
            updateDisplay("Sende Directory", "$", String(byte_counter) + " Bytes");
            return true;
        }

        // Befehl/Status auf Kanal 15 abfangen
        if (chan == 15) {
            statusState = 1; 
            return true; // Kanal 15 erfolgreich für Befehle geöffnet
        }

        // SCHREIBEN (SAVE): Kanal 1 oder 3
        if (chan == 1 || chan == 3) { 
            byte_counter = 0; 
            read_pointer = 0;

            if (requested_file.length() == 0) {
                current_filename = "UNNAMED.PRG";
            } else {
                current_filename = requested_file;
                String checkName = current_filename;
                checkName.toUpperCase();
                if (!checkName.endsWith(".PRG")) {
                    current_filename += ".PRG";
                }
            }

            statusState = 1; // Zustand zurücksetzen, da Aktion startet
            updateDisplay("EMPFANGE...", current_filename, "Bereit für Daten...");
            return true; 
        }

        // LESEN (LOAD)
        // Fall A: C64 sucht die aktive RAM-Datei
        if (requested_file.length() == 0 || requested_file == "*" || 
            current_filename.startsWith(requested_file) || requested_file == current_filename) {
            
            if (byte_counter >= 2) {
                read_pointer = 0;
                snprintf(detailsStr, sizeof(detailsStr), "Start: $%02X%02X | %zu Bytes", prgbuffer[0], prgbuffer[1], byte_counter - 2);
                statusState = 1; // Laden erfolgreich vorbereitet
                updateDisplay("IEC LOAD (RAM)", current_filename, String(detailsStr));
                return true; 
            }
        }

        // Fall B: Es ist ein neuer Nachlader im selben Verzeichnis
        if (requested_file.length() > 0) {
            updateDisplay("SUCHE IN ORDNER...", requested_file, "Lade Nachlader...");
            
            if (loadFromSD(requested_file)) {
                read_pointer = 0; 
                snprintf(detailsStr, sizeof(detailsStr), "Nachlader OK | %zu Bytes", byte_counter);
                statusState = 1; // Laden von SD erfolgreich
                updateDisplay("IEC LOAD (SD)", current_filename, String(detailsStr));
                return true; 
            }
        }
        
        updateDisplay("FILE NOT FOUND", requested_file, "Nicht im aktuellen Ordner!");
        return false; 
    }

    // =========================================================================
    // 3. WRITE-METHODE
    // =========================================================================
    virtual uint8_t write(uint8_t chan, uint8_t* buffer, uint8_t bufferSize, bool eoi) override {
        if (byte_counter + bufferSize > maximum) {
            return 0; 
        }

        memcpy(&prgbuffer[byte_counter], buffer, bufferSize);
        byte_counter += bufferSize;

        if (eoi) {
            speichereAktuelleDatei();
        }

        return bufferSize; 
    }

    // =========================================================================
    // 4. CLOSE-METHODE
    // =========================================================================
    virtual void close(uint8_t chan) override {
        if (byte_counter > 0) {
            speichereAktuelleDatei();
        }
    
        byte_counter = 0;      // ← nach dem Schließen Buffer leer setzen
        read_pointer = 0;      // ← Point
    
    }

    // =========================================================================
    // 5. READ-METHODE (Wird für das Laden/LOAD benötigt)
    // =========================================================================
    virtual uint8_t read(uint8_t chan, uint8_t* buffer, uint8_t bufferSize, bool* eoi) override {
     
const char* statusString = "00,OK,00,00";
uint8_t     statusPos    = 0;
uint8_t     statusLen    = 0;

// in open(chan == 15):
if (chan == 15) {
    statusPos = 0;
    statusLen = strlen(statusString);
    statusState = 1;
    return true;
}

// in read(...):
if (chan == 15) {
    *eoi = false;
    size_t bytesToRead = min(bufferSize, (uint8_t)(statusLen - statusPos));
    memcpy(buffer, &statusString[statusPos], bytesToRead);
    statusPos += bytesToRead;

    if (statusPos >= statusLen) {
        *eoi = true;
    }
    return bytesToRead;
}


      
        
        
        
        // Prüfen, ob überhaupt Daten im Puffer liegen
        
        if (byte_counter == 0 || read_pointer >= byte_counter) {
            *eoi = true;
            return 0;
        }

        size_t bytesToRead = bufferSize;
        
        // Prüfen, ob wir das Ende der Datei (EOI) erreichen
        if (read_pointer + bytesToRead >= byte_counter) {
            bytesToRead = byte_counter - read_pointer;
            *eoi = true; // Dem C64 signalisieren: Das ist das Dateiende!
        } else {
            *eoi = false;
        }

        // Daten aus dem RAM-Puffer in den IEC-Buffer kopieren
        memcpy(buffer, &prgbuffer[read_pointer], bytesToRead);
        read_pointer += bytesToRead;

        return bytesToRead; // Anzahl der gesendeten Bytes zurückgeben
    }

};
CYD_RAM_Drive myDrive;
IECBusHandler iecBus(PIN_IEC_ATN, PIN_IEC_CLK, PIN_IEC_DATA);

// =========================================================================
// --- SETUP ---
// =========================================================================
void setup() {

    
    preferences.begin("c64_drive", false);
    iec_device_addr_var = preferences.getUChar("dev_addr", 9);
    myDrive.setDeviceNumber(iec_device_addr_var);
    preferences.end();
    
    Serial.begin(115200); 
    
    String bootMessage = "73,CBM_PET_NET V1.0,00,00\r";
    
    touchSPI.begin(TOUCH_SCLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS_PIN);
    ts.begin(touchSPI);
    ts.setRotation(1); 

    tft.init();
    tft.setRotation(1);                 
    tft.fillScreen(TFT_BLACK);
    
    bool sdAvailable = SD.begin(SD_CS_PIN);

    WiFi.softAP(ssid, password);
    tcpServer.begin();

    iecBus.attachDevice(&myDrive); 
    iecBus.begin();                

    iecBus.enableFastLoader(&myDrive,9, true); 

    if (sdAvailable) {
        updateDisplay("BEREIT / IDLE", "", "Subdir-System geladen. Bereit!");
    } else {
        updateDisplay("SD FEHLER", "", "Keine SD-Karte erkannt.");
    }

    

}

// =========================================================================
// --- MAINLOOP ---
// =========================================================================
void loop() {
    iecBus.task(); 

    if (currentState != STATE_TRANSFER) {
        handleTouch();
    }

    if (currentState == STATE_IDLE) {
        WiFiClient client = tcpServer.available();
        if (client) {
            currentState = STATE_TRANSFER;
            updateDisplay("RECEIVE...", "Please wait", "");
            
            byte_counter = 0;
            current_filename = "";
            bool readingFilename = true;
            unsigned long lastDataTime = millis();
            
            while (client.connected() || client.available()) {
                if (client.available()) {
                    char c = (char)client.read();
                    lastDataTime = millis(); 

                    if (readingFilename) {
                        if (c == '\n' || c == '\r') {
                            if (current_filename.length() > 0) {
                                readingFilename = false; 
                                current_filename.trim();
                                updateDisplay("Receive...", current_filename, "Write Bytes...");
                            }
                        } else {
                            current_filename += c;
                        }
                    } 
                    else {
                        if (byte_counter < maximum) {
                            prgbuffer[byte_counter] = (uint8_t)c;
                            byte_counter++;
                        }
                    }
                }
                if (millis() - lastDataTime > 1500) break;
            }
            client.stop(); 
            
            if (byte_counter > 0 && current_filename.length() > 0) {
                saveToSD(current_filename, prgbuffer, byte_counter);
            }

            currentState = STATE_IDLE;
            snprintf(detailsStr, sizeof(detailsStr), "Ready for LOAD | %zu Bytes", byte_counter);
            updateDisplay("READY / IDLE", current_filename, String(detailsStr));
        }

    
    }
    
    
    delay(1); 

}
