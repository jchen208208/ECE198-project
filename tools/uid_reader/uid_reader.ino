// Reads RFID card/fob UIDs and prints them to the Serial Monitor (Arduino UNO Q).
// Wiring (RC522): SDA(SS)=D10, RST=D9, MOSI=D11, MISO=D12, SCK=D13, VCC=3.3V (NOT 5V!), GND.
#include <SPI.h>
#include <MFRC522.h>
#include <Arduino_RouterBridge.h>   // gives the UNO Q its Serial Monitor output

const uint8_t PIN_SS  = 10;
const uint8_t PIN_RST = 9;
MFRC522 rfid(PIN_SS, PIN_RST);

void setup() {
  Serial.begin(115200);
  SPI.begin();
  rfid.PCD_Init();
  Serial.println("Ready - tap a card or fob.");
}

void loop() {
  if (!rfid.PICC_IsNewCardPresent()) return;
  if (!rfid.PICC_ReadCardSerial()) return;

  Serial.print("UID: ");
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (i) Serial.print(' ');
    if (rfid.uid.uidByte[i] < 0x10) Serial.print('0');
    Serial.print(rfid.uid.uidByte[i], HEX);
  }
  Serial.println();

  rfid.PICC_HaltA();        // stop talking to this card so it isn't read again instantly
  rfid.PCD_StopCrypto1();
  delay(500);
}
