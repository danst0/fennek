#pragma once

#include "CustomSX1262.h"
#include "RadioLibWrappers.h"

class CustomSX1262Wrapper : public RadioLibWrapper {
public:
  CustomSX1262Wrapper(CustomSX1262& radio, mesh::MainBoard& board) : RadioLibWrapper(radio, board) { }

  // RX-Duty-Cycle (SetRxDutyCycle des SX1262): der Chip pendelt selbstständig
  // zwischen kurzem Horch-Fenster und Sleep; eine erkannte Präambel hält ihn
  // wach, bis das Paket komplett ist. Spart ~2/3 des kontinuierlichen
  // RX-Stroms (~5 mA mit Boosted Gain). Voraussetzung: die Sender im Netz
  // nutzen mindestens unsere konfigurierte Präambel-Länge. Greift beim
  // nächsten restartRecv()/Re-Arm.
  void setLowPowerRx(bool on) { _lowPowerRx = on; }

  bool isReceivingPacket() override {
    bool rx = ((CustomSX1262 *)_radio)->isReceiving();
    // Die IRQ-Abfrage (NSS-Zugriff) weckt einen im Duty-Sleep liegenden Chip
    // in STDBY — ohne Re-Arm bliebe das Radio taub. Bei aktivem Empfang
    // (Präambel/Header erkannt) nichts anfassen.
    if (!rx && _lowPowerRx) startRx();
    return rx;
  }
  float getCurrentRSSI() override {
    return ((CustomSX1262 *)_radio)->getRSSI(false);
  }
  float getLastRSSI() const override { return ((CustomSX1262 *)_radio)->getRSSI(); }
  float getLastSNR() const override { return ((CustomSX1262 *)_radio)->getSNR(); }

  float packetScore(float snr, int packet_len) override {
    int sf = ((CustomSX1262 *)_radio)->spreadingFactor;
    return packetScoreInt(snr, sf, packet_len);
  }
  virtual void powerOff() override {
    ((CustomSX1262 *)_radio)->sleep(false);
  }

protected:
  int16_t startRx() override {
    if (!_lowPowerRx) return RadioLibWrapper::startRx();
    // 0 = konfigurierte Präambel-Länge als Sender-Annahme, 8 Symbole
    // Mindest-Horchfenster. TIMEOUT zusätzlich auf DIO1: nach einer
    // Fehl-Präambel (Rauschen, kein Header) fällt der Chip in STDBY und würde
    // sonst taub bleiben — das Timeout-IRQ läuft als STATE_INT_READY durch
    // recvRaw() (readData scheitert bzw. liefert ein Duplikat des letzten
    // Pakets, das die MeshCore-Dedup verwirft) und stößt so den Re-Arm an.
    return ((CustomSX1262 *)_radio)->startReceiveDutyCycleAuto(0, 8,
              RADIOLIB_IRQ_RX_DEFAULT_FLAGS,
              (1UL << RADIOLIB_IRQ_RX_DONE) | (1UL << RADIOLIB_IRQ_TIMEOUT));
  }
};
