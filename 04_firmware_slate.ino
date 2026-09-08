/*
  Etiqueta intel·ligent de càmera — firmware v6
  Placa: Elecrow CrowPanel E-Paper HMI 4,2" (ESP32-S3), revisio "adhesiu verd"

  ABANS DE COMPILAR: placa "ESP32S3 Dev Module" + Flash Size 8MB + Partition
  Huge APP + PSRAM OPI + Upload Speed 115200 + USB CDC On Boot Disabled +
  Flash Mode QIO 80MHz. Llibreria: Adafruit GFX Library.

  NOVETATS d'aquesta versio
  -------------------------
  1) REFRESC RAPID SENSE PARPELLEIG. El driver d'Elecrow porta dues taules
     d'ona: GC (complet, ~3 s, parpelleja, deixa la pantalla neta) i DU
     (rapid, sense parpelleig, pero acumula imatge fantasma). Ara els
     moviments del rotary fan DU, i cada MAX_RAPIDS refrescos se'n fa un de
     GC per netejar. [Suposant] El punt on el fantasma comença a molestar
     s'ha de veure al panell real: puja o baixa MAX_RAPIDS.
  2) ES DESA EL VALOR, NO LA POSICIO. L'estat guarda "35mm T2.1" com a text,
     no "el tercer de la llista". Si canvies les llistes, el que es mostra no
     es mou sol. Si el valor ja no es a la llista, es continua veient i el
     rotary comença pel principi.
  3) PANTALLA DE TRIA. Per a qualsevol camp pots veure totes les opcions
     alhora en graella i triar-ne una, en comptes d'anar a cegues.
  4) FILTRES: si tries un filtre que ja tens en una altra ranura, s'intercanvien.

  CONTROLS
    Pantalla principal:
      rotary ........... tria camp (o canvia valor si estas editant)
      OK ............... entra a editar / desa i surt
      MENU (curt) ...... obre la pantalla de tria del camp seleccionat
      MENU (llarg) ..... pantalla d'estat
      EXIT ............. cancel·la l'edicio (torna al valor d'abans)
    Pantalla de tria:
      rotary ........... mou el cursor per la graella
      OK ............... tria el valor i torna
      EXIT / MENU ...... torna sense canviar res

  PENDENT (propera sessio): carregar les llistes des del mobil. El Bluetooth
  d'aquesta versio ja accepta TEXT en lloc d'index, pero la pagina web encara
  envia index: fins que no s'actualitzi, els canvis des del mobil no aniran.
*/

#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <Preferences.h>

// ---------- Pins ----------
#define EPD_SCK   12
#define EPD_MOSI  11
#define EPD_RES   47
#define EPD_DC    46
#define EPD_CS    45
#define EPD_BUSY  48
#define EPD_PWR   7
#define PWR_41    41

#define BTN_ROTARY_DOWN  4
#define BTN_ROTARY_PRESS 5
#define BTN_ROTARY_UP    6
#define BTN_MENU         2
#define BTN_EXIT         1

#define LED_R 15
#define LED_G 16
#define LED_B 17

#define EPD_W 400
#define EPD_H 300

#define REFRESH_DELAY_MS 500   // temps sense tocar res abans de refrescar
#define MAX_RAPIDS       8     // refrescos DU seguits abans d'un GC de neteja
// Si d'un fotograma a l'altre canvien mes pixels que aquest percentatge, es
// forca un refresc complet. El refresc rapid (DU) no sap moure be arees grans
// de negre a blanc: hi deixa un pedregar de pixels a mig transicionar.
#define LLINDAR_CANVI_PC 3     // % dels 120.000 pixels de la pantalla
#define SLEEP_AFTER_MS   6000  // adormir el panell despres d'aquesta inactivitat
#define LONG_PRESS_MS    800

// Radi de les cantonades arrodonides. RADI 0 = cantonades quadrades.
// [Probable] A 1 bit i sense suavitzat, un radi gran fa escales visibles a
// les cantonades; 6-8 es el punt on es nota arrodonit pero encara net.
#define RADI       6
#define RADI_GRAN  8

GFXcanvas1 canvas(EPD_W, EPD_H);

// =====================================================================
//  Driver de pantalla (sequencia oficial d'Elecrow, revisio verda)
// =====================================================================
// El codi original d'Elecrow mou l'SPI per programari, encenent i apagant
// els pins un bit cada cop: uns 360.000 canvis de pin per imatge. Aqui es
// fa servir l'SPI per maquinari de l'ESP32-S3, que fa la mateixa feina molt
// mes de pressa.
// SI ALGUNA COSA VA MALAMENT (pantalla en blanc, soroll, no respon), posa
// USE_HW_SPI a 0: es torna al metode lent original d'Elecrow, que sabem cert
// que funciona, i aixi saps de seguida si el problema es aquest canvi.
#define USE_HW_SPI 1
#define EPD_SPI_HZ 10000000UL   // [Suposant] 10 MHz es conservador; es pot provar de pujar

SPIClass epdSPI(FSPI);

void EPD_WR_Bus(uint8_t dat) {
#if USE_HW_SPI
  epdSPI.beginTransaction(SPISettings(EPD_SPI_HZ, MSBFIRST, SPI_MODE0));
  digitalWrite(EPD_CS, LOW);
  epdSPI.transfer(dat);
  digitalWrite(EPD_CS, HIGH);
  epdSPI.endTransaction();
#else
  digitalWrite(EPD_CS, LOW);
  for (uint8_t i = 0; i < 8; i++) {
    digitalWrite(EPD_SCK, LOW);
    digitalWrite(EPD_MOSI, (dat & 0x80) ? HIGH : LOW);
    digitalWrite(EPD_SCK, HIGH);
    dat <<= 1;
  }
  digitalWrite(EPD_CS, HIGH);
#endif
}
void EPD_WR_REG(uint8_t reg)   { digitalWrite(EPD_DC, LOW);  EPD_WR_Bus(reg); digitalWrite(EPD_DC, HIGH); }
void EPD_WR_DATA8(uint8_t dat) { digitalWrite(EPD_DC, HIGH); EPD_WR_Bus(dat); }

// Envia un bloc sencer de dades de cop (aqui es on es guanya la velocitat)
void EPD_WR_BLOC(const uint8_t* buf, size_t n) {
#if USE_HW_SPI
  digitalWrite(EPD_DC, HIGH);
  epdSPI.beginTransaction(SPISettings(EPD_SPI_HZ, MSBFIRST, SPI_MODE0));
  digitalWrite(EPD_CS, LOW);
  epdSPI.writeBytes(buf, n);
  digitalWrite(EPD_CS, HIGH);
  epdSPI.endTransaction();
#else
  for (size_t i = 0; i < n; i++) EPD_WR_DATA8(buf[i]);
#endif
}

void EPD_ReadBusy(const char* etiqueta) {
  unsigned long t0 = millis();
  while (digitalRead(EPD_BUSY) == 1) {
    delay(1);
    if (millis() - t0 > 15000) {
      Serial.print("[EPD] BUSY never cleared (15 s) at: "); Serial.println(etiqueta);
      return;
    }
  }
  Serial.print("[EPD] "); Serial.print(etiqueta); Serial.print(" : ");
  Serial.print(millis() - t0); Serial.println(" ms");
}

void EPD_RESET() {
  digitalWrite(EPD_RES, HIGH); delay(10);
  digitalWrite(EPD_RES, LOW);  delay(100);
  digitalWrite(EPD_RES, HIGH); delay(100);
}

// GC: refresc complet, parpelleja, neteja del tot
const uint8_t lut_R20_GC[42] = {0x01,0x14,0x0A,0x14,0x00,0x01,0x01, 0,0,0,0,0,0,0, 0,0,0,0,0,0,0, 0,0,0,0,0,0,0, 0,0,0,0,0,0,0, 0,0,0,0,0,0,0};
const uint8_t lut_R21_GC[42] = {0x01,0x54,0x0A,0x94,0x00,0x01,0x01, 0,0,0,0,0,0,0, 0,0,0,0,0,0,0, 0,0,0,0,0,0,0, 0,0,0,0,0,0,0, 0,0,0,0,0,0,0};
const uint8_t lut_R22_GC[42] = {0x01,0x54,0x0A,0x94,0x00,0x01,0x01, 0,0,0,0,0,0,0, 0,0,0,0,0,0,0, 0,0,0,0,0,0,0, 0,0,0,0,0,0,0, 0,0,0,0,0,0,0};
const uint8_t lut_R23_GC[42] = {0x01,0x94,0x0A,0x54,0x00,0x01,0x01, 0,0,0,0,0,0,0, 0,0,0,0,0,0,0, 0,0,0,0,0,0,0, 0,0,0,0,0,0,0, 0,0,0,0,0,0,0};
const uint8_t lut_R24_GC[42] = {0x01,0x94,0x0A,0x54,0x00,0x01,0x01, 0,0,0,0,0,0,0, 0,0,0,0,0,0,0, 0,0,0,0,0,0,0, 0,0,0,0,0,0,0, 0,0,0,0,0,0,0};

// DU: rapida, sense parpelleig, acumula imatge fantasma
const uint8_t lut_R20_DU[42] = {0x01,0x14,0x00,0x00,0x00,0x01,0x00, 0,0,0,0,0,0,0, 0,0,0,0,0,0,0, 0,0,0,0,0,0,0, 0,0,0,0,0,0,0, 0,0,0,0,0,0,0};
const uint8_t lut_R21_DU[42] = {0x01,0x14,0x00,0x00,0x00,0x01,0x00, 0,0,0,0,0,0,0, 0,0,0,0,0,0,0, 0,0,0,0,0,0,0, 0,0,0,0,0,0,0, 0,0,0,0,0,0,0};
const uint8_t lut_R22_DU[42] = {0x01,0x94,0x00,0x00,0x00,0x01,0x00, 0,0,0,0,0,0,0, 0,0,0,0,0,0,0, 0,0,0,0,0,0,0, 0,0,0,0,0,0,0, 0,0,0,0,0,0,0};
const uint8_t lut_R23_DU[42] = {0x01,0x54,0x00,0x00,0x00,0x01,0x00, 0,0,0,0,0,0,0, 0,0,0,0,0,0,0, 0,0,0,0,0,0,0, 0,0,0,0,0,0,0, 0,0,0,0,0,0,0};
const uint8_t lut_R24_DU[42] = {0x01,0x14,0x00,0x00,0x00,0x01,0x00, 0,0,0,0,0,0,0, 0,0,0,0,0,0,0, 0,0,0,0,0,0,0, 0,0,0,0,0,0,0, 0,0,0,0,0,0,0};

void escriuLut(const uint8_t* a, const uint8_t* b, const uint8_t* c, const uint8_t* d, const uint8_t* e) {
  EPD_WR_REG(0x20); for (int i = 0; i < 42; i++) EPD_WR_DATA8(a[i]);
  EPD_WR_REG(0x21); for (int i = 0; i < 42; i++) EPD_WR_DATA8(b[i]);
  EPD_WR_REG(0x22); for (int i = 0; i < 42; i++) EPD_WR_DATA8(c[i]);
  EPD_WR_REG(0x23); for (int i = 0; i < 42; i++) EPD_WR_DATA8(d[i]);
  EPD_WR_REG(0x24); for (int i = 0; i < 42; i++) EPD_WR_DATA8(e[i]);
}
void lut_GC() { escriuLut(lut_R20_GC, lut_R21_GC, lut_R22_GC, lut_R23_GC, lut_R24_GC); }
void lut_DU() { escriuLut(lut_R20_DU, lut_R21_DU, lut_R22_DU, lut_R23_DU, lut_R24_DU); }

void EPD_Init() {
  EPD_WR_REG(0x00); EPD_WR_DATA8(0x3F); EPD_WR_DATA8(0x4D);
  EPD_WR_REG(0x01); EPD_WR_DATA8(0x03); EPD_WR_DATA8(0x10); EPD_WR_DATA8(0x3F); EPD_WR_DATA8(0x3F); EPD_WR_DATA8(0x03);
  EPD_WR_REG(0x06); EPD_WR_DATA8(0x96); EPD_WR_DATA8(0x96); EPD_WR_DATA8(0x29);
  EPD_WR_REG(0x30); EPD_WR_DATA8(0x09);
  EPD_WR_REG(0x61); EPD_WR_DATA8(0x01); EPD_WR_DATA8(0x90); EPD_WR_DATA8(0x01); EPD_WR_DATA8(0x2C);
  EPD_WR_REG(0x82); EPD_WR_DATA8(0x05);
  EPD_WR_REG(0x50); EPD_WR_DATA8(0x97);
  EPD_WR_REG(0x60); EPD_WR_DATA8(0x22);
  EPD_WR_REG(0xE3); EPD_WR_DATA8(0x88);
}
void EPD_Update() { EPD_WR_REG(0x17); EPD_WR_DATA8(0xA5); EPD_ReadBusy("refresh"); }
void EPD_Sleep()  { EPD_WR_REG(0x07); EPD_WR_DATA8(0xA5); delay(50); }

void EPD_Clear() {
  static uint8_t blanc[250];
  memset(blanc, 0xFF, sizeof(blanc));
  EPD_RESET();
  EPD_Init();
  EPD_WR_REG(0x10); for (int k = 0; k < 60; k++) EPD_WR_BLOC(blanc, sizeof(blanc)); // 60 x 250 = 15000
  EPD_WR_REG(0x13); for (int k = 0; k < 60; k++) EPD_WR_BLOC(blanc, sizeof(blanc));
  lut_GC();
  EPD_Update();
}

// Envia el canvas al panell. complet=true -> GC; complet=false -> DU.
void EPD_Envia(const uint8_t* img, bool complet) {
  EPD_WR_REG(0x50); EPD_WR_DATA8(0xD7);
  EPD_WR_REG(0x13);
  EPD_WR_BLOC(img, 15000);
  if (complet) lut_GC(); else lut_DU();
  EPD_Update();
}

// Despertar el panell costa temps; el mantenim despert mentre l'usuari
// toca coses per poder encadenar refrescos rapids.
bool panelDespert = false;
unsigned long ultimRefresc = 0;

void panelDesperta() {
  if (panelDespert) return;
  EPD_RESET(); delay(20);
  EPD_Init();  delay(50);
  panelDespert = true;
}
void panelDorm() {
  if (!panelDespert) return;
  EPD_Sleep();
  panelDespert = false;
  Serial.println("[EPD] panel asleep");
}

// =====================================================================
//  Dades
// =====================================================================
#define LEN_TXT      20
#define MAX_LENS     12
#define MAX_FILTRES  24
#define MAX_FPS       8
#define MAX_SHUTTER  10
#define CAP_FILTRE   "-"

struct CameraPreset { char letter; uint8_t ledR, ledG, ledB; };
CameraPreset cameraPresets[] = {
  {'A', 255,   0,   0},
  {'B',   0,   0, 255},
  {'C',   0, 255,   0},
  {'D', 255, 255,   0},
  {'E', 255, 128,   0},
  {'F', 180, 180, 180},
};
const uint8_t N_CAMERES = sizeof(cameraPresets) / sizeof(cameraPresets[0]);

struct Llistes {
  char lens[MAX_LENS][LEN_TXT];       uint8_t nLens;
  char filtres[MAX_FILTRES][LEN_TXT]; uint8_t nFiltres;
  char fps[MAX_FPS][LEN_TXT];         uint8_t nFps;
  char shutter[MAX_SHUTTER][LEN_TXT]; uint8_t nShutter;
};
Llistes llistes;

struct Estat {
  uint8_t cameraIdx;
  char lens[LEN_TXT];
  char filtre[3][LEN_TXT];
  char fps[LEN_TXT];
  char shutter[LEN_TXT];
};
Estat estat;
Estat estatBackup;   // per poder cancel·lar amb EXIT

// Llistes que arriben del mobil: s'omplen entrada a entrada i nomes
// substitueixen les bones quan arriba l'ordre de confirmar.
Llistes llistesPendents;
volatile bool volCommitLlistes = false;

void llistesPerDefecte() {
  const char* L[] = {"18mm T1.9", "25mm T1.9", "35mm T2.1", "50mm T2.1", "85mm T2.1"};
  const char* F[] = {CAP_FILTRE, "ND 0.3", "ND 0.6", "ND 0.9", "ND 1.2",
                     "ROTAPOLA", "BPM 1/8", "BPM 1/4", "BPM 1/2", "HBM 1/4"};
  const char* P[] = {"24 fps", "25 fps", "50 fps"};
  const char* S[] = {"45", "90", "144", "172.8", "180", "270", "1/50", "1/100"};

  llistes.nLens = sizeof(L) / sizeof(L[0]);
  for (uint8_t i = 0; i < llistes.nLens; i++) strlcpy(llistes.lens[i], L[i], LEN_TXT);
  llistes.nFiltres = sizeof(F) / sizeof(F[0]);
  for (uint8_t i = 0; i < llistes.nFiltres; i++) strlcpy(llistes.filtres[i], F[i], LEN_TXT);
  llistes.nFps = sizeof(P) / sizeof(P[0]);
  for (uint8_t i = 0; i < llistes.nFps; i++) strlcpy(llistes.fps[i], P[i], LEN_TXT);
  llistes.nShutter = sizeof(S) / sizeof(S[0]);
  for (uint8_t i = 0; i < llistes.nShutter; i++) strlcpy(llistes.shutter[i], S[i], LEN_TXT);
}

// Camps. NOTA: les funcions reben "int", no aquest enum, perque l'Arduino IDE
// insereix els prototips automatics ABANS dels tipus definits al sketch.
enum { CAMP_CAMERA, CAMP_LENS, CAMP_FPS, CAMP_SHUTTER, CAMP_F1, CAMP_F2, CAMP_F3, CAMP_TOTAL };
#define CAMP_CAP (-1)   // cap camp seleccionat: pantalla neta, sense cap caixa en negatiu
const char* nomCamp[] = {"CAMERA", "LENS", "FPS", "SHUTTER", "FILTER SLOT 1", "FILTER SLOT 2", "FILTER SLOT 3"};
int campActual = CAMP_CAP;
bool editant = false;

enum { PANT_PRINCIPAL, PANT_TRIA, PANT_ESTAT };
int pantalla = PANT_PRINCIPAL;
int triaCamp = 0;
int triaCursor = 0;
bool bleConnectat = false;

uint8_t nombreOpcions(int camp) {
  switch (camp) {
    case CAMP_CAMERA:  return N_CAMERES;
    case CAMP_LENS:    return llistes.nLens;
    case CAMP_FPS:     return llistes.nFps;
    case CAMP_SHUTTER: return llistes.nShutter;
    default:           return llistes.nFiltres;
  }
}

const char* opcio(int camp, uint8_t i) {
  static char buf[LEN_TXT];
  switch (camp) {
    case CAMP_CAMERA:  snprintf(buf, sizeof(buf), "CAM %c", cameraPresets[i].letter); return buf;
    case CAMP_LENS:    return llistes.lens[i];
    case CAMP_FPS:     return llistes.fps[i];
    case CAMP_SHUTTER: return llistes.shutter[i];
    default:           return llistes.filtres[i];
  }
}

const char* valorActual(int camp) {
  static char buf[LEN_TXT];
  switch (camp) {
    case CAMP_CAMERA:  snprintf(buf, sizeof(buf), "CAM %c", cameraPresets[estat.cameraIdx].letter); return buf;
    case CAMP_LENS:    return estat.lens;
    case CAMP_FPS:     return estat.fps;
    case CAMP_SHUTTER: return estat.shutter;
    case CAMP_F1:      return estat.filtre[0];
    case CAMP_F2:      return estat.filtre[1];
    case CAMP_F3:      return estat.filtre[2];
  }
  return "";
}

// Posa un filtre en una ranura. Si ja es en una altra, s'intercanvien
// (el "buit" pot repetir-se, per aixo queda exclos).
void posaFiltre(uint8_t ranura, const char* v) {
  if (strcmp(v, CAP_FILTRE) != 0) {
    for (uint8_t j = 0; j < 3; j++) {
      if (j != ranura && strcmp(estat.filtre[j], v) == 0) {
        strlcpy(estat.filtre[j], estat.filtre[ranura], LEN_TXT);
        break;
      }
    }
  }
  strlcpy(estat.filtre[ranura], v, LEN_TXT);
}

void aplicaValor(int camp, const char* v) {
  switch (camp) {
    case CAMP_CAMERA:
      for (uint8_t i = 0; i < N_CAMERES; i++) {
        char b[LEN_TXT];
        snprintf(b, sizeof(b), "CAM %c", cameraPresets[i].letter);
        if (strcmp(b, v) == 0) { estat.cameraIdx = i; break; }
      }
      break;
    case CAMP_LENS:    strlcpy(estat.lens, v, LEN_TXT); break;
    case CAMP_FPS:     strlcpy(estat.fps, v, LEN_TXT); break;
    case CAMP_SHUTTER: strlcpy(estat.shutter, v, LEN_TXT); break;
    case CAMP_F1:      posaFiltre(0, v); break;
    case CAMP_F2:      posaFiltre(1, v); break;
    case CAMP_F3:      posaFiltre(2, v); break;
  }
}

int indexActual(int camp) {
  char v[LEN_TXT];
  strlcpy(v, valorActual(camp), LEN_TXT);
  uint8_t n = nombreOpcions(camp);
  for (uint8_t i = 0; i < n; i++) if (strcmp(opcio(camp, i), v) == 0) return i;
  return -1;
}

void passaValor(int camp, int dir) {
  uint8_t n = nombreOpcions(camp);
  if (n == 0) return;
  int i = indexActual(camp);
  if (i < 0) i = 0;   // el valor ja no es a la llista: comença pel principi
  else { i += dir; if (i < 0) i = n - 1; if (i >= (int)n) i = 0; }
  aplicaValor(camp, opcio(camp, i));
}

// =====================================================================
//  Botons
// =====================================================================
struct Btn { uint8_t pin; bool lastState; unsigned long lastChangeAt; };
enum { B_DOWN, B_PRESS, B_UP, B_EXIT, B_TOTAL };
Btn btns[B_TOTAL] = {
  {BTN_ROTARY_DOWN,  HIGH, 0},
  {BTN_ROTARY_PRESS, HIGH, 0},
  {BTN_ROTARY_UP,    HIGH, 0},
  {BTN_EXIT,         HIGH, 0},
};
#define DEBOUNCE_MS 180

bool pressedEdge(int id) {
  Btn &b = btns[id];
  bool now = digitalRead(b.pin);
  bool fired = false;
  if (now != b.lastState && millis() - b.lastChangeAt > DEBOUNCE_MS) {
    b.lastChangeAt = millis();
    if (now == LOW) fired = true;
    b.lastState = now;
  }
  return fired;
}

// MENU distingeix pulsacio curta i llarga
bool menuAvall = false;
unsigned long menuAvallDes = 0;
bool menuLlargFet = false;
bool menuCurt = false, menuLlarg = false;

void llegeixMenu() {
  menuCurt = menuLlarg = false;
  bool now = (digitalRead(BTN_MENU) == LOW);
  if (now && !menuAvall) {
    menuAvall = true; menuAvallDes = millis(); menuLlargFet = false;
  } else if (now && menuAvall && !menuLlargFet && millis() - menuAvallDes > LONG_PRESS_MS) {
    menuLlargFet = true; menuLlarg = true;
  } else if (!now && menuAvall) {
    menuAvall = false;
    if (!menuLlargFet && millis() - menuAvallDes > 40) menuCurt = true;
  }
}

// =====================================================================
//  LED
// =====================================================================
void setLEDRaw(uint8_t r, uint8_t g, uint8_t b) {
  analogWrite(LED_R, r); analogWrite(LED_G, g); analogWrite(LED_B, b);
}
void updateLED() {
  CameraPreset &p = cameraPresets[estat.cameraIdx];
  setLEDRaw(p.ledR, p.ledG, p.ledB);
}

// =====================================================================
//  Persistencia
// =====================================================================
Preferences prefs;

void carrega() {
  prefs.begin("slate", false);
  if (prefs.getBytes("llistes", &llistes, sizeof(llistes)) != sizeof(llistes)) {
    llistesPerDefecte();
  }
  if (prefs.getBytes("estat", &estat, sizeof(estat)) != sizeof(estat)) {
    estat.cameraIdx = 0;
    strlcpy(estat.lens, llistes.lens[0], LEN_TXT);
    for (int i = 0; i < 3; i++) strlcpy(estat.filtre[i], CAP_FILTRE, LEN_TXT);
    strlcpy(estat.fps, llistes.nFps > 1 ? llistes.fps[1] : llistes.fps[0], LEN_TXT);
    strlcpy(estat.shutter, "180", LEN_TXT);
  }
}
void desa()        { prefs.putBytes("estat", &estat, sizeof(estat)); }
void desaLlistes() { prefs.putBytes("llistes", &llistes, sizeof(llistes)); }

// =====================================================================
//  Ajudants de dibuix (canvas 1 bit: 1 = blanc, 0 = negre)
// =====================================================================
void textFont(int x, int y, int w, int h, const char* txt, const GFXfont* f, bool invers) {
  canvas.setFont(f);
  canvas.setTextSize(1);
  int16_t bx, by; uint16_t bw, bh;
  canvas.getTextBounds(txt, 0, 0, &bx, &by, &bw, &bh);
  canvas.setTextColor(invers ? 1 : 0);
  canvas.setCursor(x + (w - (int)bw) / 2 - bx, y + (h - (int)bh) / 2 - by);
  canvas.print(txt);
}

// Tria sola la mida de lletra mes gran que hi capiga (noms de filtre llargs)
void textAuto(int x, int y, int w, int h, const char* txt, bool invers) {
  canvas.setFont(NULL);
  int mida = 3;
  int16_t bx, by; uint16_t bw, bh;
  while (mida > 1) {
    canvas.setTextSize(mida);
    canvas.getTextBounds(txt, 0, 0, &bx, &by, &bw, &bh);
    if ((int)bw <= w - 8) break;
    mida--;
  }
  canvas.setTextSize(mida);
  canvas.getTextBounds(txt, 0, 0, &bx, &by, &bw, &bh);
  canvas.setTextColor(invers ? 1 : 0);
  canvas.setCursor(x + (w - (int)bw) / 2 - bx, y + (h - (int)bh) / 2 - by);
  canvas.print(txt);
}

// Igual que textFont pero amb escala. Les tipografies incloses arriben fins a
// 24pt; per fer "CAM A" mes gran cal escalar-la (mida 2 = el doble). L'escalat
// duplica pixels, aixi que les vores queden una mica escalonades — de lluny,
// que es com es mira aixo, no es nota i guanya presencia.
void textFontN(int x, int y, int w, int h, const char* txt, const GFXfont* f, int mida, bool invers) {
  canvas.setFont(f);
  canvas.setTextSize(mida);
  int16_t bx, by; uint16_t bw, bh;
  canvas.getTextBounds(txt, 0, 0, &bx, &by, &bw, &bh);
  canvas.setTextColor(invers ? 1 : 0);
  canvas.setCursor(x + (w - (int)bw) / 2 - bx, y + (h - (int)bh) / 2 - by);
  canvas.print(txt);
  canvas.setTextSize(1);
}

// Marcar la seleccio invertint una caixa sencera vol dir moure milers de
// pixels, que es justament el que el refresc rapid fa pitjor (o parpelleja o
// s'embruta). Per aixo:
//   - navegar entre camps (molt frequent) -> nomes un contorn gruixut: barat
//   - entrar en edicio (poc frequent)     -> inversio sencera: cara pero clara
void caixa(int x, int y, int w, int h, bool sel, bool enEdicio) {
  if (sel && enEdicio) {
    canvas.fillRoundRect(x, y, w, h, RADI, 0);
  } else if (sel) {
    canvas.drawRoundRect(x, y, w, h, RADI, 0);
    canvas.drawRoundRect(x + 1, y + 1, w - 2, h - 2, RADI - 1, 0);
    canvas.drawRoundRect(x + 2, y + 2, w - 4, h - 4, RADI - 1, 0);
    canvas.drawRoundRect(x - 2, y - 2, w + 4, h + 4, RADI + 1, 0);
  } else {
    canvas.drawRoundRect(x, y, w, h, RADI, 0);
  }
}

// El simbol de grau no existeix a les FreeSansBold (nomes ASCII): cercle.
void caixaShutter(int x, int y, int w, int h, const char* s, bool sel) {
  bool angle = (strchr(s, '/') == NULL);
  canvas.setFont(&FreeSansBold12pt7b);
  canvas.setTextSize(1);
  int16_t bx, by; uint16_t bw, bh;
  canvas.getTextBounds(s, 0, 0, &bx, &by, &bw, &bh);
  int extra = angle ? 12 : 0;
  int tx = x + (w - ((int)bw + extra)) / 2 - bx;
  int ty = y + (h - (int)bh) / 2 - by;
  canvas.setTextColor(sel ? 1 : 0);
  canvas.setCursor(tx, ty);
  canvas.print(s);
  if (angle) canvas.drawCircle(tx + bx + bw + 6, ty + by + 4, 3, sel ? 1 : 0);
}

// =====================================================================
//  Pantalla principal
// =====================================================================
void dibuixaPrincipal() {
  CameraPreset &p = cameraPresets[estat.cameraIdx];
  canvas.fillScreen(1);
  canvas.drawRoundRect(0, 0, EPD_W, EPD_H, RADI_GRAN, 0);
  canvas.drawRoundRect(3, 3, EPD_W - 6, EPD_H - 6, RADI_GRAN - 1, 0);

  // CAM: SEMPRE negre amb lletra blanca (contrast i identificacio rapida).
  // La seleccio NO s'indica invertint-lo: invertir una area tan gran era el
  // que embrutava la pantalla amb el refresc rapid. Es marca amb un anell blanc.
  {
    bool sel = (campActual == CAMP_CAMERA);
    char txt[10];
    snprintf(txt, sizeof(txt), "CAM %c", p.letter);
    int x = 10, y = 10, w = EPD_W - 20, h = 80;
    canvas.fillRoundRect(x, y, w, h, RADI_GRAN, 0);
    textFontN(x, y, w, h, txt, &FreeSansBold24pt7b, 2, true);
    if (sel) {
      canvas.drawRoundRect(x + 5, y + 5, w - 10, h - 10, RADI_GRAN - 2, 1);
      if (editant) canvas.drawRoundRect(x + 7, y + 7, w - 14, h - 14, RADI_GRAN - 3, 1);
    }
  }

  // Optica: caixa mes alta
  {
    bool sel = (campActual == CAMP_LENS);
    caixa(10, 96, EPD_W - 20, 56, sel, sel && editant);
    textFont(10, 96, EPD_W - 20, 56, estat.lens, &FreeSansBold18pt7b, sel && editant);
  }

  // FPS + Shutter: caixes mes baixes
  {
    bool selF = (campActual == CAMP_FPS);
    caixa(10, 158, 234, 38, selF, selF && editant);
    textFont(10, 158, 234, 38, estat.fps, &FreeSansBold18pt7b, selF && editant);

    bool selS = (campActual == CAMP_SHUTTER);
    caixa(250, 158, 140, 38, selS, selS && editant);
    caixaShutter(250, 158, 140, 38, estat.shutter, selS && editant);
  }

  // Filtres
  {
    canvas.setFont(&FreeSansBold9pt7b);
    canvas.setTextSize(1);
    canvas.setTextColor(0);
    canvas.setCursor(10, 214);          // alineat a l'esquerra
    canvas.print("Filters");

    const int fx[3] = {10, 138, 266};
    const int fw = 124;
    const int camps[3] = {CAMP_F1, CAMP_F2, CAMP_F3};
    for (int i = 0; i < 3; i++) {
      bool sel = (campActual == camps[i]);
      char cap[12];
      snprintf(cap, sizeof(cap), "SLOT %d", i + 1);
      canvas.fillRoundRect(fx[i], 220, fw, 22, RADI - 2, 0);
      textFont(fx[i], 220, fw, 22, cap, &FreeSansBold9pt7b, true);
      caixa(fx[i], 246, fw, 30, sel, sel && editant);
      textAuto(fx[i], 246, fw, 30, estat.filtre[i], sel && editant);
    }
  }

  canvas.setFont(NULL);
  canvas.setTextSize(1);
  canvas.setTextColor(0);
  canvas.setCursor(10, 285);
  if (editant)                    canvas.print("EDITING   turn: value | OK: save | EXIT: cancel");
  else if (campActual == CAMP_CAP) canvas.print("turn: select field | hold MENU: status");
  else                             canvas.print("turn: field | OK: edit | MENU: list | EXIT: none");
}

// =====================================================================
//  Pantalla de tria
// =====================================================================
#define TRIA_COLS  3
#define TRIA_FILES 8

void dibuixaTria() {
  canvas.fillScreen(1);
  canvas.fillRect(0, 0, EPD_W, 30, 0);
  textFont(0, 0, EPD_W, 30, nomCamp[triaCamp], &FreeSansBold9pt7b, true);

  uint8_t n = nombreOpcions(triaCamp);
  char actual[LEN_TXT];
  strlcpy(actual, valorActual(triaCamp), LEN_TXT);

  const int cw = 124, ch = 26;
  for (uint8_t i = 0; i < n && i < TRIA_COLS * TRIA_FILES; i++) {
    int col = i % TRIA_COLS;
    int fila = i / TRIA_COLS;
    int x = 8 + col * 129;
    int y = 34 + fila * 30;
    const char* txt = opcio(triaCamp, i);
    bool cursor = (i == (uint8_t)triaCursor);
    bool esActual = (strcmp(txt, actual) == 0);

    if (cursor) canvas.fillRoundRect(x, y, cw, ch, RADI - 2, 0);
    else        canvas.drawRoundRect(x, y, cw, ch, RADI - 2, 0);
    if (esActual && !cursor) canvas.drawRoundRect(x + 2, y + 2, cw - 4, ch - 4, RADI - 3, 0);
    textAuto(x, y, cw, ch, txt, cursor);
  }

  canvas.setFont(NULL);
  canvas.setTextSize(1);
  canvas.setTextColor(0);
  canvas.setCursor(8, 283);
  canvas.print("turn: move | OK: select | EXIT: cancel");
}

// =====================================================================
//  Pantalla d'estat
// =====================================================================
void dibuixaEstat() {
  canvas.fillScreen(1);
  canvas.setFont(NULL);
  canvas.setTextColor(0);
  canvas.setTextSize(2);
  canvas.setCursor(10, 20);
  canvas.print("DEVICE STATUS");
  canvas.drawFastHLine(10, 40, 380, 0);
  canvas.setTextSize(1);
  int y = 60;
  canvas.setCursor(10, y); canvas.print("Firmware: v6"); y += 16;
  canvas.setCursor(10, y); canvas.print("Bluetooth: ");
  canvas.print(bleConnectat ? "connected" : "waiting (Slate-Camera)"); y += 16;
  canvas.setCursor(10, y); canvas.print("Lenses loaded:   "); canvas.print(llistes.nLens); y += 16;
  canvas.setCursor(10, y); canvas.print("Filters loaded:  "); canvas.print(llistes.nFiltres); y += 16;
  canvas.setCursor(10, y); canvas.print("FPS loaded:      "); canvas.print(llistes.nFps); y += 16;
  canvas.setCursor(10, y); canvas.print("Shutters loaded: "); canvas.print(llistes.nShutter); y += 16;
  canvas.setCursor(10, 283);
  canvas.print("MENU or EXIT: back");
}

// =====================================================================
//  Refresc
// =====================================================================
uint8_t comptadorRapids = 0;
bool pantallaBruta = false;
bool volFullRefresh = false;
unsigned long ultimCanvi = 0;

void marcaPantalla(bool complet) {
  pantallaBruta = true;
  if (complet) volFullRefresh = true;
  ultimCanvi = millis();
}

// Quants pixels han canviat de veritat respecte l'ultim fotograma.
// Es compara pixel a pixel (XOR) contra una copia del fotograma anterior.
// La versio anterior comparava nomes el TOTAL de pixels negres, i aixo podia
// donar zero quan una caixa s'encenia i una altra s'apagava alhora.
static uint8_t fotogramaAnterior[15000];
bool hiHaAnterior = false;

uint32_t pixelsCanviats(const uint8_t* buf, size_t n) {
  if (!hiHaAnterior) return 0xFFFFFFFF;   // primer fotograma: sempre complet
  uint32_t c = 0;
  for (size_t i = 0; i < n; i++) c += __builtin_popcount((unsigned)(buf[i] ^ fotogramaAnterior[i]));
  return c;
}

void pintaAra(bool complet) {
  panelDesperta();
  if (pantalla == PANT_PRINCIPAL)  dibuixaPrincipal();
  else if (pantalla == PANT_TRIA)  dibuixaTria();
  else                             dibuixaEstat();

  uint32_t diff = pixelsCanviats(canvas.getBuffer(), 15000);
  memcpy(fotogramaAnterior, canvas.getBuffer(), 15000);
  hiHaAnterior = true;
  bool canviGran = (diff > (uint32_t)(EPD_W * EPD_H) * LLINDAR_CANVI_PC / 100);

  bool ple = complet || canviGran || (comptadorRapids >= MAX_RAPIDS);
  if (canviGran) Serial.println("[EPD] large change -> full refresh");
  if (ple) comptadorRapids = 0; else comptadorRapids++;
  unsigned long t0 = millis();
  EPD_Envia(canvas.getBuffer(), ple);
  Serial.print("[EPD] "); Serial.print(ple ? "FULL" : "fast");
  Serial.print(" - total "); Serial.print(millis() - t0); Serial.println(" ms");
  ultimRefresc = millis();
}

void gestionaRefresc() {
  if (pantallaBruta && millis() - ultimCanvi > REFRESH_DELAY_MS) {
    pantallaBruta = false;
    pintaAra(volFullRefresh);
    volFullRefresh = false;
  }
  if (panelDespert && !pantallaBruta && millis() - ultimRefresc > SLEEP_AFTER_MS) {
    panelDorm();
  }
}

// =====================================================================
//  Navegacio
// =====================================================================
void obreTria(int camp) {
  triaCamp = camp;
  int i = indexActual(camp);
  triaCursor = (i < 0) ? 0 : i;
  pantalla = PANT_TRIA;
  editant = false;
  marcaPantalla(true);   // canvi de pantalla sencera: refresc net
}

void handleButtons() {
  llegeixMenu();

  if (pantalla == PANT_ESTAT) {
    bool sortir = pressedEdge(B_EXIT);   // sempre avaluat: mai dins d'un ||
    if (menuCurt || menuLlarg || sortir) {
      pantalla = PANT_PRINCIPAL;
      marcaPantalla(true);
    }
    pressedEdge(B_UP); pressedEdge(B_DOWN); pressedEdge(B_PRESS); // descarta
    return;
  }

  if (pantalla == PANT_TRIA) {
    bool sortir = pressedEdge(B_EXIT);   // sempre avaluat: mai dins d'un ||
    if (menuCurt || sortir) {
      pantalla = PANT_PRINCIPAL;
      marcaPantalla(true);
      return;
    }
    if (pressedEdge(B_PRESS)) {
      aplicaValor(triaCamp, opcio(triaCamp, triaCursor));
      desa();
      publicaEstatBLE();
      updateLED();
      pantalla = PANT_PRINCIPAL;
      campActual = CAMP_CAP;    // torna a la vista neta
      marcaPantalla(true);
      return;
    }
    bool up = pressedEdge(B_UP);
    bool down = pressedEdge(B_DOWN);
    if (up || down) {
      int dir = up ? -1 : 1;
      int n = nombreOpcions(triaCamp);
      if (n > 0) {
        triaCursor += dir;
        if (triaCursor < 0) triaCursor = n - 1;
        if (triaCursor >= n) triaCursor = 0;
        marcaPantalla(false);
      }
    }
    return;
  }

  // Pantalla principal
  if (menuLlarg) {
    pantalla = PANT_ESTAT;
    editant = false;
    marcaPantalla(true);
    return;
  }
  if (menuCurt) { if (campActual != CAMP_CAP) obreTria(campActual); return; }

  if (pressedEdge(B_EXIT)) {
    if (editant) { estat = estatBackup; editant = false; updateLED(); marcaPantalla(false); }
    else if (campActual != CAMP_CAP) { campActual = CAMP_CAP; marcaPantalla(true); } // pantalla neta
    return;
  }
  if (pressedEdge(B_PRESS)) {
    if (campActual == CAMP_CAP) { campActual = CAMP_CAMERA; }   // tornar a entrar
    else if (!editant) { estatBackup = estat; editant = true; }
    else { editant = false; desa(); publicaEstatBLE(); }
    marcaPantalla(false);
    return;
  }
  bool up = pressedEdge(B_UP);
  bool down = pressedEdge(B_DOWN);
  if (up || down) {
    int dir = up ? -1 : 1;   // invertit: confirmat contra el maquinari real
    if (campActual == CAMP_CAP) {
      campActual = (dir > 0) ? CAMP_CAMERA : CAMP_TOTAL - 1;
    } else if (!editant) {
      campActual += dir;
      if (campActual < 0) campActual = CAMP_TOTAL - 1;
      if (campActual >= CAMP_TOTAL) campActual = 0;
    } else {
      passaValor(campActual, dir);
      updateLED();
    }
    marcaPantalla(false);
  }
}

// =====================================================================
//  BLE — ara accepta TEXT (no index). La pagina web s'ha d'actualitzar.
// =====================================================================
#define SERVICE_UUID    "6a2f1000-0000-4b8e-8a2d-9c9f6a2f1000"
#define CH_CAMERA_UUID  "6a2f1000-0001-4b8e-8a2d-9c9f6a2f1000"
#define CH_LENS_UUID    "6a2f1000-0002-4b8e-8a2d-9c9f6a2f1000"
#define CH_FILTER1_UUID "6a2f1000-0003-4b8e-8a2d-9c9f6a2f1000"
#define CH_FILTER2_UUID "6a2f1000-0004-4b8e-8a2d-9c9f6a2f1000"
#define CH_FILTER3_UUID "6a2f1000-0005-4b8e-8a2d-9c9f6a2f1000"
#define CH_FPS_UUID     "6a2f1000-0006-4b8e-8a2d-9c9f6a2f1000"
#define CH_SHUTTER_UUID "6a2f1000-0007-4b8e-8a2d-9c9f6a2f1000"
#define CH_COMMIT_UUID  "6a2f1000-0008-4b8e-8a2d-9c9f6a2f1000"
#define CH_STATUS_UUID  "6a2f1000-0009-4b8e-8a2d-9c9f6a2f1000"
#define CH_LIST_UUID    "6a2f1000-000a-4b8e-8a2d-9c9f6a2f1000"

/*  PROTOCOL DE LLISTES (caracteristica CH_LIST)
    Una entrada per escriptura: aixi cap de sobres al paquet Bluetooth mes
    petit possible (23 bytes) i no cal partir res ni tornar-ho a ajuntar.

      [0xF0]                     -> comença: copia les llistes actuals a
                                    "pendents" per anar-les modificant
      [tipus][index][text...]    -> desa una entrada a pendents
      [0xFF][tipus][quantitat]   -> quantes entrades te aquesta llista
      [0xFE]                     -> confirma: pendents passen a bones i es desen
      [0xF1][tipus][index]       -> prepara una entrada per poder-la llegir
                                    (la pagina web llegeix CH_LIST despres)

    tipus: 0 = optiques, 1 = filtres, 2 = fps, 3 = shutter
*/
#define TIPUS_LENS 0
#define TIPUS_FILT 1
#define TIPUS_FPS  2
#define TIPUS_SHUT 3

BLEServer* pServer;
BLECharacteristic *chCamera, *chLens, *chFilter1, *chFilter2, *chFilter3,
                  *chFps, *chShutter, *chCommit, *chStatus, *chList;

Estat blePendent;
volatile bool bleCommitPendent = false;

class ServerCallbacks: public BLEServerCallbacks {
  void onConnect(BLEServer* s) { bleConnectat = true; }
  void onDisconnect(BLEServer* s) { bleConnectat = false; BLEDevice::startAdvertising(); }
};

// aplicaValor() treballa sobre "estat"; aqui l'apliquem a blePendent
void bleAplica(int camp, const char* txt) {
  Estat guardat = estat;
  estat = blePendent;
  aplicaValor(camp, txt);
  blePendent = estat;
  estat = guardat;
}

class CampCB: public BLECharacteristicCallbacks {
public:
  int camp;
  CampCB(int c) : camp(c) {}
  void onWrite(BLECharacteristic* c) {
    auto v = c->getValue();
    if (v.length() == 0 || v.length() >= LEN_TXT) return;
    char buf[LEN_TXT];
    strlcpy(buf, v.c_str(), LEN_TXT);
    bleAplica(camp, buf);
  }
};
// Escriu una entrada a les llistes en preparacio
void guardaPendent(uint8_t tipus, uint8_t idx, const char* txt) {
  switch (tipus) {
    case TIPUS_LENS: if (idx < MAX_LENS)    strlcpy(llistesPendents.lens[idx], txt, LEN_TXT); break;
    case TIPUS_FILT: if (idx < MAX_FILTRES) strlcpy(llistesPendents.filtres[idx], txt, LEN_TXT); break;
    case TIPUS_FPS:  if (idx < MAX_FPS)     strlcpy(llistesPendents.fps[idx], txt, LEN_TXT); break;
    case TIPUS_SHUT: if (idx < MAX_SHUTTER) strlcpy(llistesPendents.shutter[idx], txt, LEN_TXT); break;
  }
}

void guardaQuantitat(uint8_t tipus, uint8_t n) {
  switch (tipus) {
    case TIPUS_LENS: llistesPendents.nLens    = (n > MAX_LENS)    ? MAX_LENS    : n; break;
    case TIPUS_FILT: llistesPendents.nFiltres = (n > MAX_FILTRES) ? MAX_FILTRES : n; break;
    case TIPUS_FPS:  llistesPendents.nFps     = (n > MAX_FPS)     ? MAX_FPS     : n; break;
    case TIPUS_SHUT: llistesPendents.nShutter = (n > MAX_SHUTTER) ? MAX_SHUTTER : n; break;
  }
}

// Deixa una entrada preparada perque la pagina web la pugui llegir
void preparaLectura(uint8_t tipus, uint8_t idx) {
  const char* t = "";
  switch (tipus) {
    case TIPUS_LENS: if (idx < llistes.nLens)    t = llistes.lens[idx];    break;
    case TIPUS_FILT: if (idx < llistes.nFiltres) t = llistes.filtres[idx]; break;
    case TIPUS_FPS:  if (idx < llistes.nFps)     t = llistes.fps[idx];     break;
    case TIPUS_SHUT: if (idx < llistes.nShutter) t = llistes.shutter[idx]; break;
  }
  chList->setValue((uint8_t*)t, strlen(t));
}

class ListCB: public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) {
    auto v = c->getValue();
    if (v.length() < 1) return;
    uint8_t op = (uint8_t)v[0];

    if (op == 0xF0) {                       // comença
      llistesPendents = llistes;
      Serial.println("[BLE] lists: upload started");
      return;
    }
    if (op == 0xFE) { volCommitLlistes = true; return; }   // confirma (a loop())
    if (op == 0xFF && v.length() >= 3) { guardaQuantitat((uint8_t)v[1], (uint8_t)v[2]); return; }
    if (op == 0xF1 && v.length() >= 3) { preparaLectura((uint8_t)v[1], (uint8_t)v[2]); return; }

    if (op <= TIPUS_SHUT && v.length() >= 2) {            // entrada normal
      char buf[LEN_TXT];
      size_t n = v.length() - 2;
      if (n >= LEN_TXT) n = LEN_TXT - 1;                  // el que sobra es talla
      memcpy(buf, v.c_str() + 2, n);
      buf[n] = '\0';
      guardaPendent(op, (uint8_t)v[1], buf);
    }
  }
};

class CommitCB: public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) {
    auto v = c->getValue();
    if (v.length() >= 1 && (uint8_t)v[0] == 0x01) bleCommitPendent = true;
  }
};

BLECharacteristic* makeChar(BLEService* svc, const char* uuid, uint32_t props, BLECharacteristicCallbacks* cb) {
  BLECharacteristic* ch = svc->createCharacteristic(uuid, props);
  if (props & BLECharacteristic::PROPERTY_NOTIFY) ch->addDescriptor(new BLE2902());
  if (cb) ch->setCallbacks(cb);
  return ch;
}

void setupBLE() {
  BLEDevice::init("Slate-Camera");
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());
  BLEService* svc = pServer->createService(SERVICE_UUID);
  uint32_t RW = BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE;
  chCamera  = makeChar(svc, CH_CAMERA_UUID,  RW, new CampCB(CAMP_CAMERA));
  chLens    = makeChar(svc, CH_LENS_UUID,    RW, new CampCB(CAMP_LENS));
  chFilter1 = makeChar(svc, CH_FILTER1_UUID, RW, new CampCB(CAMP_F1));
  chFilter2 = makeChar(svc, CH_FILTER2_UUID, RW, new CampCB(CAMP_F2));
  chFilter3 = makeChar(svc, CH_FILTER3_UUID, RW, new CampCB(CAMP_F3));
  chFps     = makeChar(svc, CH_FPS_UUID,     RW, new CampCB(CAMP_FPS));
  chShutter = makeChar(svc, CH_SHUTTER_UUID, RW, new CampCB(CAMP_SHUTTER));
  chCommit  = makeChar(svc, CH_COMMIT_UUID,  RW, new CommitCB());
  chList    = makeChar(svc, CH_LIST_UUID,    RW, new ListCB());
  chStatus  = makeChar(svc, CH_STATUS_UUID,
                       BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY, nullptr);
  svc->start();
  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();
}

// Perque la pagina web pugui llegir que hi ha posat ara mateix al dispositiu.
// Sense aixo, llegir una caracteristica tornaria buit.
void publicaEstatBLE() {
  if (!chCamera) return;
  char cam[LEN_TXT];
  snprintf(cam, sizeof(cam), "CAM %c", cameraPresets[estat.cameraIdx].letter);
  chCamera ->setValue((uint8_t*)cam, strlen(cam));
  chLens   ->setValue((uint8_t*)estat.lens, strlen(estat.lens));
  chFilter1->setValue((uint8_t*)estat.filtre[0], strlen(estat.filtre[0]));
  chFilter2->setValue((uint8_t*)estat.filtre[1], strlen(estat.filtre[1]));
  chFilter3->setValue((uint8_t*)estat.filtre[2], strlen(estat.filtre[2]));
  chFps    ->setValue((uint8_t*)estat.fps, strlen(estat.fps));
  chShutter->setValue((uint8_t*)estat.shutter, strlen(estat.shutter));
}

// Confirma les llistes rebudes. Es fa des de loop() i no des del callback
// BLE perque desar a la memoria i refrescar la pantalla triguen massa.
void gestionaLlistesBLE() {
  if (!volCommitLlistes) return;
  volCommitLlistes = false;
  llistes = llistesPendents;
  desaLlistes();
  publicaEstatBLE();
  Serial.print("[BLE] lists saved: ");
  Serial.print(llistes.nLens);    Serial.print(" lenses, ");
  Serial.print(llistes.nFiltres); Serial.print(" filters, ");
  Serial.print(llistes.nFps);     Serial.print(" fps, ");
  Serial.print(llistes.nShutter); Serial.println(" shutters");
  // Els valors que hi havia posats es mantenen encara que ja no siguin a la
  // llista: es desa el text, no la posicio. Nomes cal repintar.
  pantalla = PANT_PRINCIPAL;
  campActual = CAMP_CAP;
  marcaPantalla(true);
}

// El refresc triga segons: mai des del callback BLE, sempre des de loop()
void gestionaCommitBLE() {
  if (!bleCommitPendent) return;
  bleCommitPendent = false;
  estat = blePendent;
  desa();
  publicaEstatBLE();
  updateLED();
  Serial.println("[BLE] commit received");
  pantalla = PANT_PRINCIPAL;
  marcaPantalla(true);
}

// =====================================================================
//  Setup / loop
// =====================================================================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("[BOOT] firmware v6");

  pinMode(BTN_ROTARY_DOWN, INPUT_PULLUP);
  pinMode(BTN_ROTARY_PRESS, INPUT_PULLUP);
  pinMode(BTN_ROTARY_UP, INPUT_PULLUP);
  pinMode(BTN_MENU, INPUT_PULLUP);
  pinMode(BTN_EXIT, INPUT_PULLUP);

  pinMode(LED_R, OUTPUT); pinMode(LED_G, OUTPUT); pinMode(LED_B, OUTPUT);
  setLEDRaw(0, 0, 0);

  pinMode(PWR_41, OUTPUT);  digitalWrite(PWR_41, HIGH);
  pinMode(EPD_PWR, OUTPUT); digitalWrite(EPD_PWR, HIGH);
  pinMode(EPD_RES, OUTPUT); pinMode(EPD_DC, OUTPUT); pinMode(EPD_CS, OUTPUT);
  digitalWrite(EPD_CS, HIGH);
  pinMode(EPD_BUSY, INPUT);
#if USE_HW_SPI
  epdSPI.begin(EPD_SCK, -1, EPD_MOSI, -1);   // sense MISO ni CS automatic
  Serial.println("[EPD] hardware SPI");
#else
  pinMode(EPD_SCK, OUTPUT); pinMode(EPD_MOSI, OUTPUT);
  Serial.println("[EPD] software SPI (original slow mode)");
#endif
  Serial.println("[EPD] power on, initial clear...");
  EPD_Clear();
  panelDespert = true;
  delay(300);

  carrega();
  blePendent = estat;
  Serial.println("[NVS] settings loaded");
  setupBLE();
  publicaEstatBLE();
  Serial.println("[BLE] advertising as Slate-Camera");

  pintaAra(true);
  updateLED();
  Serial.println("[OK] setup complete");
}

void loop() {
  handleButtons();
  gestionaLlistesBLE();
  gestionaCommitBLE();
  gestionaRefresc();
  delay(5);
}
