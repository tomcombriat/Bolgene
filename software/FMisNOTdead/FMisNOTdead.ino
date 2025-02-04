/*
Copyright 2024 Thomas Combriat

This is licensed under the GPL Licence

This is the main code for the poly-Boelgene, a polyphonic version
of a small Ondes Nartenot replica.

It features four pressure buttons instead of one, allowing
different notes to be played simulteanously.


TODO: 
 - pressure calib
 - tame volume when pressure on lpf?
 - cleanup (separate the voice class for god sake)
*/

#include "MozziConfigValues.h"  // for named option values
#define MOZZI_AUDIO_MODE MOZZI_OUTPUT_I2S_DAC
#define MOZZI_I2S_FORMAT MOZZI_I2S_FORMAT_LSBJ
#define MOZZI_AUDIO_CHANNELS 1
#define MOZZI_CONTROL_RATE 256  // Hz, powers of 2 are most reliable

#include <Button.h>  // from TES_eSax-lib
#include <Mozzi.h>
#include <mozzi_midi.h>
#include <Oscil.h>
#include <tables/saw2048_int8.h>
//#include <tables/triangle2048_int8.h>
#include <tables/cos2048_int8.h>
#include <ResonantFilter.h>
#include <FixMathMapper.h>
#include "BiPot.h"
#include <MetaOscil.h>
#include "Oscils.h"

#include <SPI.h>
#include <MCP3XXX.h>


FixMathMapper<UFix<12, 0>, UFix<7, 9>> mapper;

// ADC
MCP3XXX_<10, 8, 300000> adc;


// Pin mapping
const uint8_t pot0 = 3, pot1 = 4, pot2 = 5, pot3 = 6, pot4 = 7, pot5 = 28;
const uint8_t pitch_pin = 27, octm_pin = 9, octp_pin = 8, refm_pin = 7, refp_pin = 3, led_pin = 6;
const uint8_t pressure_pin1 = 26, pressure_pin2 = 0, pressure_pin3 = 1, pressure_pin4 = 2;

// Octaves plus and minus buttons
Button octm(octm_pin, false);
Button octp(octp_pin, false);

// Global synthesis variables
int octave = 0;
UFix<7, 9> midi_base_note;
UFix<12, 0> pitch_pot_min = 433, pitch_pot_max = 3811;
bool delay_on = false;
uint16_t resonance = 0;  //, cutoff = 0;
UFix<2, 8> mod1Ratio, prevMod1Ratio;
UFix<2, 8> mod2Ratio, prevMod2Ratio;




unsigned long last_update_time = 0;


// Maybe boost back to 1 once pressure properly calibrated?
BiPotMultBoost cutoffPot(10, 8, 2);
BiPotMultBoost mod1Pot(10, 8, 2);
BiPotMultBoost mod1RatioPot(10);

BiPotMultBoost mod2Pot(12, 8, 2);  // 12 because we use an internal DAC here, and not the external 10 bits dac
BiPotMultBoost mod2RatioPot(10);


#define N_VOICES 4
#define OSC_NUM_CELLS 2048
class voice {
public:  // no offense, most things in public, I know what I am doing
  Oscil<OSC_NUM_CELLS, AUDIO_RATE> aCos;
  Oscil<OSC_NUM_CELLS, AUDIO_RATE> aMod1;
  Oscil<OSC_NUM_CELLS, AUDIO_RATE> aMod2;
  Oscil<OSC_NUM_CELLS, AUDIO_RATE> aSub;
  //AudioDelayFeedback<2048> aDel;
  SFix<4, 0> transpose;

  uint8_t volume;
  LowPassFilter16 lpf;
  uint16_t cutoff;

  void setFreq(UFix<16, 16> _freq) {
    anti_alias(_freq, modulation_amount1, modulation_amount2);
    aCos.setFreq(freq);
    aSub.setFreq(freq.sR<1>());
    aMod1.setFreq(freq * mod1Ratio);
    aMod2.setFreq(freq * mod2Ratio);
  };

  void setMod1(uint16_t mod1) {
    anti_alias(freq, mod1, modulation_amount2);
  };
  void setMod2(uint16_t mod2) {
    anti_alias(freq, modulation_amount1, mod2);
  };

  uint16_t getMod1() {
    return modulation_amount1;
  };
  uint16_t getMod2() {
    return modulation_amount2;
  };



private:
  uint16_t modulation_amount1, modulation_amount2;
  UFix<16, 16> freq;
  void anti_alias(UFix<16, 16> _freq, uint16_t mod1, uint16_t mod2) {
    UFix<0, 8> B = 0.0075 * 2;                                                                           // 3 removes more aliases, but changes a lot the sound
    auto den = (_freq * mod1Ratio * (UFixAuto<1>() + ((B * mod2Ratio * (UFix<16, 0>(mod2).sR<7>())))));  // denominator
    uint32_t m1 = (uint32_t(16000 * 74) / den.asInt()) << 4;
    if (m1 < mod1) modulation_amount1 = m1;  // if mod1 too big, we tame down to m1, limit to not have aliases
    else modulation_amount1 = mod1;
    freq = _freq;
    modulation_amount2 = mod2;
  };
};


voice voices[N_VOICES];

void setup() {

  // Voices initialization
  for (int i = 0; i < N_VOICES; i++) {
    voices[i].aCos.setTable(COS2048_DATA);
    voices[i].aMod1.setTable(COS2048_DATA);
    voices[i].aMod2.setTable(COS2048_DATA);
    voices[i].aSub.setTable(COS2048_DATA);
  }
  voices[0].transpose = 0;
  voices[1].transpose = 4;
  voices[2].transpose = 3;
  voices[3].transpose = 7;

  pinMode(pressure_pin1, INPUT);
  pinMode(pitch_pin, INPUT);
  pinMode(pot5, INPUT);

  pinMode(refp_pin, INPUT_PULLUP);
  pinMode(led_pin, OUTPUT);

  // Mapper from rotary pot value to midi note (from FixMath)
  mapper.setBounds(pitch_pot_min, pitch_pot_max, 48, 48 + 24);



  startMozzi(MOZZI_CONTROL_RATE);
  pinMode(refm_pin, INPUT_PULLUP);
  pinMode(refp_pin, INPUT_PULLUP);  // otherwise the pin3 is erratic, why? Might be worth an issue.
  pinMode(octm_pin, INPUT_PULLUP);  // otherwise the pin3 is erratic, why? Might be worth an issue.
  pinMode(octp_pin, INPUT_PULLUP);  // otherwise the pin3 is erratic, why? Might be worth an issue.
  pinMode(led_pin, OUTPUT);
  digitalWrite(led_pin, HIGH);
}

void setup1() {
  //Serial.begin(115200);
  SPI.setRX(16);
  SPI.setTX(19);
  SPI.setSCK(18);
  adc.begin(17);
}

void loop() {
  audioHook();
}

void loop1() {

  // Octave button checking
  octm.update();
  octp.update();

  if (octm.has_been_released()) {
    octave--;
    if (octave < -4) octave = -4;
  }

  if (octp.has_been_released()) {
    octave++;
    if (octave > 3) octave = 3;
  }

  // Parameters checking
  if (last_update_time + 2 < millis()) {
    last_update_time = millis();

    // LPF parameters checking
    resonance = adc.analogRead(pot1) << 6;

    cutoffPot.setValue(adc.analogRead(pot0));
    for (uint8_t i = 0; i < N_VOICES; i++) voices[i].cutoff = cutoffPot.getValue(voices[i].volume);


    //// MODULATION AMOUNTS
    mod1Pot.setValue(adc.analogRead(pot3));
    for (uint8_t i = 0; i < N_VOICES; i++) voices[i].setMod1(mod1Pot.getValue(voices[i].volume));

    // checked in updateControl
    for (uint8_t i = 0; i < N_VOICES; i++) voices[i].setMod2(mod2Pot.getValue(voices[i].volume));


    //// MODULATION RATIOS
    mod1RatioPot.setValue(adc.analogRead(pot2));
    if (mod1RatioPot.getMult()) mod1Ratio = mod1Ratio.fromRaw(mod1RatioPot.getValueRaw());
    else {  // constrained ratio
      mod1Ratio = UFix<2, 1>(mod1Ratio.fromRaw(mod1RatioPot.getValueRaw()));
      if (prevMod1Ratio != mod1Ratio) {
        prevMod1Ratio = mod1Ratio;
        for (uint8_t i = 0; i < N_VOICES; i++) {  // re-align the phase (pointless in free mod)
          voices[i].aCos.setPhaseFractional(0);
          voices[i].aMod1.setPhaseFractional(0);
          voices[i].aMod2.setPhaseFractional(0);
        }
      }
    }

    mod2RatioPot.setValue(adc.analogRead(pot4));
    if (mod2RatioPot.getMult()) mod2Ratio = mod2Ratio.fromRaw(mod2RatioPot.getValueRaw());
    else {  // constrained ratio
      mod2Ratio = UFix<2, 1>(mod2Ratio.fromRaw(mod2RatioPot.getValueRaw()));
      if (prevMod2Ratio != mod2Ratio) {
        prevMod2Ratio = mod2Ratio;
        for (uint8_t i = 0; i < N_VOICES; i++) {  // re-align the phase (pointless in free mod)
          voices[i].aCos.setPhaseFractional(0);
          voices[i].aMod1.setPhaseFractional(0);
          voices[i].aMod2.setPhaseFractional(0);
        }
      }
    }
  }

  // Volume checking for additionnal buttons (here because async is not available)
  voices[1].volume = adc.analogRead(pressure_pin2) >> 2;
  voices[2].volume = adc.analogRead(pressure_pin3) >> 2;
  voices[3].volume = adc.analogRead(pressure_pin4) >> 2;
}


void updateControl() {

  /* Ref min and max */
  if (!digitalRead(refm_pin)) {
    pitch_pot_min = mozziAnalogRead<12>(pitch_pin);
    mapper.setBounds(pitch_pot_min, pitch_pot_max, 48, 48 + 24);
  }
  if (!digitalRead(refp_pin)) {
    pitch_pot_max = mozziAnalogRead<12>(pitch_pin);
    mapper.setBounds(pitch_pot_min, pitch_pot_max, 48, 48 + 24);
  }

  mod2Pot.setValue(mozziAnalogRead<12>(pot5));

  midi_base_note = mapper.map(mozziAnalogRead<12>(pitch_pin));
  midi_base_note = midi_base_note + UFixAuto<12>() * SFix<3, 0>(octave);


  for (uint8_t i = 0; i < N_VOICES; i++) {
    auto freq = mtof(midi_base_note + voices[i].transpose);
    voices[i].setFreq(freq);
  }


  voices[0].volume = mozziAnalogRead<8>(pressure_pin1);

  for (uint8_t i = 0; i < N_VOICES; i++) voices[i].lpf.setCutoffFreqAndResonance(voices[i].cutoff, resonance);
}

AudioOutput updateAudio() {
  int32_t sample = 0;
  if (mod1Ratio != UFix<2, 8>(0) && mod2Ratio != UFix<2, 8>(0)) {
    for (uint8_t i = 0; i < N_VOICES; i++) {
      int32_t sub_sample = voices[i].aSub.next() + voices[i].aCos.phMod((voices[i].getMod1() * voices[i].aMod1.phMod((voices[i].getMod2() * voices[i].aMod2.next()) >> 7)) >> 4);
      sub_sample = voices[i].lpf.next(sub_sample * voices[i].volume);
      sample += sub_sample;
    }
  } else if (mod2Ratio == UFix<2, 8>(0)) {
    for (uint8_t i = 0; i < N_VOICES; i++) {
      int32_t sub_sample = voices[i].aSub.next() + voices[i].aCos.phMod((voices[i].getMod1() * voices[i].aMod1.next()) >> 4);
      sub_sample = voices[i].lpf.next(sub_sample * voices[i].volume);
      sample += sub_sample;
    }
  } else {
    for (uint8_t i = 0; i < N_VOICES; i++) {
      int32_t sub_sample = voices[i].aSub.next() + voices[i].aCos.next();
      sub_sample = voices[i].lpf.next(sub_sample * voices[i].volume);
      sample += sub_sample;
    }
  }
  return MonoOutput::fromNBit(17, sample).clip();
}
