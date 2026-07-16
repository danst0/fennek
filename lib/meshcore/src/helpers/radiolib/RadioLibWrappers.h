#pragma once

#include <Mesh.h>
#include <RadioLib.h>

class RadioLibWrapper : public mesh::Radio {
protected:
  PhysicalLayer* _radio;
  mesh::MainBoard* _board;
  uint32_t n_recv, n_sent;
  int16_t _noise_floor, _threshold;
  uint16_t _num_floor_samples;
  int32_t _floor_sample_sum;
  // Fennek: RX-Duty-Cycle aktiv (setzt CustomSX1262Wrapper::setLowPowerRx).
  // loop() setzt dann das Noise-Floor-Sampling aus: jede RSSI-Probe (SPI)
  // würde den im Duty-Sleep liegenden Chip aufwecken und den Zyklus abbrechen.
  bool _lowPowerRx = false;

  void idle();
  void startRecv();
  // Zentraler RX-Arm-Punkt (startRecv + recvRaw): Unterklassen können hier
  // einen Duty-Cycle-Empfang statt des kontinuierlichen RX starten.
  virtual int16_t startRx() { return _radio->startReceive(); }
  float packetScoreInt(float snr, int sf, int packet_len);
  virtual bool isReceivingPacket() =0;

public:
  RadioLibWrapper(PhysicalLayer& radio, mesh::MainBoard& board) : _radio(&radio), _board(&board) { n_recv = n_sent = 0; }

  void begin() override;
  virtual void powerOff() { _radio->sleep(); }
  // RX sauber neu armieren (z. B. nach Parameterwechsel oder Umschalten des
  // Duty-Cycle-Modus) — ersetzt rohe startReceive()-Aufrufe am Radio vorbei.
  void restartRecv() { idle(); startRecv(); }
  int recvRaw(uint8_t* bytes, int sz) override;
  uint32_t getEstAirtimeFor(int len_bytes) override;
  bool startSendRaw(const uint8_t* bytes, int len) override;
  bool isSendComplete() override;
  void onSendFinished() override;
  bool isInRecvMode() const override;
  bool isChannelActive();

  bool isReceiving() override { 
    if (isReceivingPacket()) return true;

    return isChannelActive();
  }

  virtual float getCurrentRSSI() =0;

  int getNoiseFloor() const override { return _noise_floor; }
  void triggerNoiseFloorCalibrate(int threshold) override;
  void resetAGC() override;

  void loop() override;

  uint32_t getPacketsRecv() const { return n_recv; }
  uint32_t getPacketsSent() const { return n_sent; }
  void resetStats() { n_recv = n_sent = 0; }

  virtual float getLastRSSI() const override;
  virtual float getLastSNR() const override;

  float packetScore(float snr, int packet_len) override { return packetScoreInt(snr, 10, packet_len); }  // assume sf=10
};

/**
 * \brief  an RNG impl using the noise from the LoRa radio as entropy.
 *         NOTE: this is VERY SLOW!  Use only for things like creating new LocalIdentity
*/
class RadioNoiseListener : public mesh::RNG {
  PhysicalLayer* _radio;
public:
  RadioNoiseListener(PhysicalLayer& radio): _radio(&radio) { }

  void random(uint8_t* dest, size_t sz) override {
    for (int i = 0; i < sz; i++) {
      dest[i] = _radio->randomByte() ^ (::random(0, 256) & 0xFF);
    }
  }
};
