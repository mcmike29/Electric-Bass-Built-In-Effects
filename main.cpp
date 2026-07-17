#include <Audio.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <SerialFlash.h>
#include <Encoder.h>
#include <math.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
// #include <Fonts/FreeSans9pt7b.h> // may need this later
#include "KorgTriton_samples.h"

// --- I2C Screen details:
Adafruit_SSD1306 display(128/*width*/, 64/*height*/, &Wire2, -1);

// --- Chorus Buffer:
#ifndef AUDIO_BLOCK_SAMPLES
#define AUDIO_BLOCK_SAMPLES 128
#endif

#define CHORUS_DELAY_LENGTH (16 * AUDIO_BLOCK_SAMPLES)
static short chorus_delayline[CHORUS_DELAY_LENGTH];


// --- drive preset helpers (preset 2)
static float driveCurve[257];

static void makeSoftClipCurve(float driveAmount){
  // driveAmount range: 1 - 5 (mild to heavy)
  const float norm = tanhf(driveAmount);
  for (int i = 0; i < 257; i++) {
    float x = (2.0f * i / 256.0f) - 1.0f;          // -1...+1
    driveCurve[i] = tanhf(driveAmount * x) / norm; // -1...+1
  }
}

static void makeIdentityCurve() {
  for (int i = 0; i < 257; i++) {
    float x = (2.0f * i / 256.0f) - 1.0f; // -1...+1
    driveCurve[i] = x;                    // y = x (clean)
  }
}
// ---

// Audio Tool Objects
AudioInputI2S            i2s1;
AudioAmplifier           preAmp;
AudioEffectWaveshaper    drive1;
AudioFilterBiquad        biquad1;

// For preset 3:
AudioEffectChorus        chorus1;
AudioEffectDelay         delay1;
AudioEffectFreeverb      reverb1;

// Mixer for presets:
AudioMixer4              mix1;

// For synth presets:
AudioFilterBiquad        synthLPF;
AudioAnalyzeNoteFrequency noteFreq1;
AudioAnalyzePeak         peak1;

// Piano (preset4):
AudioSynthWavetable      piano1;
// Final selector mixer:
// ch0 = normal bass FX mix (mix1)
// ch1 = piano synth
AudioMixer4              mix2;

AudioEffectFade          fade1;
AudioOutputI2S           i2s2;
AudioControlSGTL5000     sgtl5000_1;

// Audio Patch chords
AudioConnection patchCord1(i2s1, 0, preAmp, 0);
AudioConnection patchCord2(preAmp, 0, drive1, 0);
AudioConnection patchCord3(drive1, 0, biquad1, 0);

// dry -> mixer ch0
AudioConnection patchCord4(biquad1, 0, mix1, 0);

// chorus path -> mixer ch1
AudioConnection patchCord5(biquad1, 0, chorus1, 0);
AudioConnection patchCord6(chorus1, 0, mix1, 1);

// slap delay from chorus -> mixer ch2
AudioConnection patchCord7(chorus1, 0, delay1, 0);
AudioConnection patchCord8(delay1, 0, mix1, 2);

// reverb from chorus -> mixer ch3
AudioConnection patchCord9(chorus1, 0, reverb1, 0);
AudioConnection patchCord10(reverb1, 0, mix1, 3);

// Cords for analyzing input signal frequency and magnitude
AudioConnection patchCordA(preAmp, 0, synthLPF, 0);
AudioConnection patchCordB(synthLPF, 0, noteFreq1, 0);
AudioConnection patchCordC(preAmp, 0, peak1, 0);

// --- Final selector mixer ---
AudioConnection patchCordD(mix1, 0, mix2, 0);     // normal presets (ch0)
AudioConnection patchCordE(piano1, 0, mix2, 1);   // piano preset (ch1)

// Replacing existing: mix1 -> fade1 with:
AudioConnection patchCordF(mix2, 0, fade1, 0);

// master fade -> stereo out (duplicate mono to both)
// AudioConnection patchCord11(mix1, 0, fade1, 0);
AudioConnection patchCord12(fade1, 0, i2s2, 0);
AudioConnection patchCord13(fade1, 0, i2s2, 1);


// Pins on Teensy for encoder (and IC screen)
const int pinCLK = 40; // orange wire
const int pinDT = 41; // yellow wire
const int pinSW = 39; // blue wire

Encoder knob(pinCLK, pinDT);

// Encoder preset min/max constants
const int COUNTS_PER_DETENT = 4;
const int MIN_PRESET = 0;
const int MAX_PRESET = 4;

const long RAW_MIN = (long)MIN_PRESET * COUNTS_PER_DETENT;
const long RAW_MAX = (long)MAX_PRESET * COUNTS_PER_DETENT;

// Presets FSM States
enum class Preset: uint8_t {
  PRESET0, // dry
  PRESET1, // compressor
  PRESET2, // drive/growl
  PRESET3, // reverb/chorus
  PRESET4_SYNTH // Piano
};

Preset CURR_PRESET = Preset::PRESET0;

// --- SYNTH and PIANO VARIABLES and FUNCTIONS ---
static bool noteOn = false;
static int currentMidi = -1;
static uint32_t lastLoudMs = 0;
// static int lastCandidate = -1; unused for now
// static int stableCount = 0;

static int freqToMidi(float f) {
  float n = 69.0f + 12.0f * (logf(f / 440.0f) / logf(2.0f));
  int midi = (int)lroundf(n);
  // Clamp to a useful range for 4-string bass -> piano
  if (midi < 25) midi = 25; // NOTE: May need to raise this to ~27-28
  if (midi > 108) midi = 108; // prev value = 84
  return midi;
}

static uint8_t peakToVelocity(float p) {
  // p ~ 0..1.0 (usually much smaller in practice)
  int v = (int)(p * 1200.0f);   // tune this scale by ear
  if (v < 20) v = 20;
  if (v > 127) v = 127;
  return (uint8_t)v;
}

void servicePiano() {
  // Hold latest analyzer values between updates
  static float p = 0.0f, f = 0.0f, prob = 0.0f;
  static float prevP = 0.0f;

  // Capture state (monophonic)
  static bool pending = false;
  static uint32_t onsetMs = 0;
  static float onsetPeak = 0.0f;

  // Pitch accumulation during capture window
  static float fSum = 0.0f;
  static int   fCount = 0;

  // Update latest measurements when available
  if (peak1.available()) p = peak1.read();
  if (noteFreq1.available()) {
    f = noteFreq1.read();
    prob = noteFreq1.probability();
  }

  // ---- Tunables (use your existing values) ----
  const float ON_PEAK     = 0.010f;
  const float QUIET_PEAK  = 0.005f;

  const float PROB_MIN_HI = 0.70f;
  const float PROB_MIN_LO = 0.50f;
  const float prob_min    = (f < 90.0f) ? PROB_MIN_LO : PROB_MIN_HI;

  const uint32_t CAPTURE_MS = 20;   // short = responsive
  const uint32_t OFF_MS     = 150;  // your current decay

  // Keep note alive while signal still present
  if (noteOn && p > QUIET_PEAK) lastLoudMs = millis();

  // Simple rising-edge onset detect (no hysteresis)
  bool onset = (!noteOn && !pending && (p > ON_PEAK) && (prevP <= ON_PEAK));
  prevP = p;

  // Start capture window on onset
  if (onset) {
    pending = true;
    onsetMs = millis();
    onsetPeak = p;

    fSum = 0.0f;
    fCount = 0;
  }

  // Collect pitch estimates during capture window
  if (!noteOn && pending) {
    if (prob > prob_min && f > 20.0f && f < 600.0f) {
      fSum += f;
      fCount++;
    }

    // End capture window and trigger note
    if (millis() - onsetMs >= CAPTURE_MS) {
      pending = false;

      if (fCount >= 2) {
        float fAvg = fSum / (float)fCount;
        uint8_t vel = peakToVelocity(onsetPeak);
        int midi = freqToMidi(fAvg);

        piano1.playNote((uint8_t)midi, vel);
        noteOn = true;
        currentMidi = midi;
        lastLoudMs = millis();
      }
    }
  }

  // Note-off
  if (noteOn && (millis() - lastLoudMs > OFF_MS)) {
    piano1.stop();
    noteOn = false;
    currentMidi = -1;
  }
}

// ---------------

static void setAllFXOffToDry() {
  // Route only dry path through the mixer
  mix1.gain(0, 1.0f); // dry
  mix1.gain(1, 0.0f); // chorus
  mix1.gain(2, 0.0f); // delay
  mix1.gain(3, 0.0f); // reverb

  // Disable time FX outputs (they’ll be silent anyway if mixer gains are 0)
  chorus1.voices(0);
  for (int ch = 0; ch < 8; ch++){
    delay1.disable(ch);
  }
  // Reverb is auto “muted” via mixer gain
}

// Debounce variables
volatile int encoderPos = 0;

void preset0(){ // dry - no effects

  AudioNoInterrupts();
  // make sure level values are back to defaut
  preAmp.gain(1.2f); // start at high gain
  makeIdentityCurve();
  drive1.shape(driveCurve, 257);
  biquad1.setLowpass(0, 20000.0f, 0.7071f); // effectively bypass
  
  // sgtl5000_1.lineInLevel(5);
  // sgtl5000_1.lineOutLevel(29);
  // sgtl5000_1.volume(0.75);
  sgtl5000_1.autoVolumeDisable();

  setAllFXOffToDry();
  mix2.gain(0, 1.0f);  // normal
  mix2.gain(1, 0.0f);  // piano muted
  piano1.stop();

  AudioInterrupts();
}

void preset1(){ // compressor + EQ
  AudioNoInterrupts();

  preAmp.gain(1.2f);
  makeIdentityCurve();
  drive1.shape(driveCurve, 257);

  sgtl5000_1.audioPostProcessorEnable();   // enable SGTL5000 DAP stage
  sgtl5000_1.autoVolumeControl(
    0,      // maxGain = +6dB upward available; prev value = 1
    3,      // lbiResponse = 100ms // 2 = 50 ms
    0,      // hardLimit off (compress/expand behavior)
    -10.0f, // threshold (tune by ear later); prev value = -18.0f
    120.0f, // attack dB/s (faster reaction)
    6.0f   // decay dB/s (slower recovery)
  );
  sgtl5000_1.autoVolumeEnable();

  // Reset all biquad stages to "neutral-ish" first
  biquad1.setLowpass(0, 20000.0f, 0.7071f);
  biquad1.setLowpass(1, 20000.0f, 0.7071f);
  biquad1.setLowpass(2, 20000.0f, 0.7071f);
  biquad1.setLowpass(3, 20000.0f, 0.7071f);

  // IMPORTANT: Double check whether to make a separate "eq1" biquad instance or not
  // Stage 0: remove subsonic rumble
  biquad1.setHighpass(0, 35.0f, 0.707f); // 35 Hz HPF
  // Stage 1: add a little more gain around 90 Hz
  biquad1.setLowShelf(1, 90.0f, +3.0f, 1.0f); // +3 dB around 90 Hz
  // Stage 2: lower hissing noise (optional)
  biquad1.setHighShelf(2, 4000.0f, -2.0f, 1.0f); // -2 dB around 4 kHz

  setAllFXOffToDry();

  mix2.gain(0, 1.0f);  // normal
  mix2.gain(1, 0.0f);  // piano muted
  piano1.stop();

  AudioInterrupts();
}

void preset2(){ // Drive + lowpass "cab-ish" EQ
  AudioNoInterrupts();

  sgtl5000_1.autoVolumeDisable();

  preAmp.gain(1.6f); // prev value = 2.0f;
  makeSoftClipCurve(5.2f); // prev value = 3.8f;
  drive1.shape(driveCurve, 257);

  // Reset all stages first
  biquad1.setLowpass(0, 20000.0f, 0.7071f);
  biquad1.setLowpass(1, 20000.0f, 0.7071f);
  biquad1.setLowpass(2, 20000.0f, 0.7071f);
  biquad1.setLowpass(3, 20000.0f, 0.7071f);

  // Tighten lows a hair (reduces perceived loudness + adds clarity)
  biquad1.setHighpass(0, 45.0f, 0.707f);

  /* If it gets too “fizzy,” do one of these:
     - drop the shelf gain +2.0f → +1.0f
     - or lower the cab cutoff 6500 → 5500 */

  // Cab-ish effect (two lowpasses for steeper slope)
  biquad1.setLowpass(1, 6000.0f, 0.85f); // prev value = 0, 5000.0f, 0.7071f
  biquad1.setLowpass(2, 6000.0f, 0.85f); // prev value = 1, 5000.0f, 0.7071f

  // 2) Remove low-mid mud so bite/attack comes forward
  biquad1.setNotch(3, 450.0f, 1.0f);        // try 350–600 Hz, Q 0.8–1.4

  // Move shelf up into "attack" zone, keep it gentle
  // biquad1.setHighShelf(3, 2600.0f, +2.0f, 0.8f);

  setAllFXOffToDry();

  mix2.gain(0, 0.52f);  // normal
  mix2.gain(1, 0.0f);  // piano muted
  piano1.stop();

  AudioInterrupts();
}

void preset3(){ // Chorus or short delay + subtle reverb
  AudioNoInterrupts();

  sgtl5000_1.autoVolumeDisable();
  preAmp.gain(1.2f);
  makeIdentityCurve();
  drive1.shape(driveCurve, 257);

  // Reset biquad stages
  biquad1.setLowpass(0, 20000.0f, 0.7071f);
  biquad1.setLowpass(1, 20000.0f, 0.7071f);
  biquad1.setLowpass(2, 20000.0f, 0.7071f);
  biquad1.setLowpass(3, 20000.0f, 0.7071f);

  // Gentle rumble control (optional)
  biquad1.setHighpass(0, 35.0f, 0.707f);

  // Enable chorus
  chorus1.voices(3);

  // Slap delay: only tap 0
  delay1.delay(0, 160); // prev value = ~135 ms
  for (int ch = 1; ch < 8; ch++){
    delay1.disable(ch);
  }

  // Subtle reverb
  reverb1.roomsize(0.52f); // prev value = 0.48f
  reverb1.damping(0.46f); // prev value = 0.48f

  // Mix: keep sum <= ~1.0 to avoid clipping.
  // Note: chorus output already contains some “dry-ish” content, so keep dry low.
  mix1.gain(0, 0.15f); // dry (prev value = 0.16f)
  mix1.gain(1, 0.44f); // chorus main (prev value = 0.48f)
  mix1.gain(2, 0.20f); // delay (prev value = 0.14f)
  mix1.gain(3, 0.21f); // reverb (prev value = 0.20f)

  mix2.gain(0, 1.10f);  // normal
  mix2.gain(1, 0.0f);  // piano muted
  piano1.stop();

  AudioInterrupts();
}

void preset4(){
  AudioNoInterrupts();

  sgtl5000_1.autoVolumeDisable();

  // sgtl5000_1.lineInLevel(10);  // try 7–12; back off if distortion

  preAmp.gain(1.2f);
  makeIdentityCurve();
  drive1.shape(driveCurve, 257);
  synthLPF.setLowpass(0, 1200.0f, 0.707f);

  // Output ONLY piano
  mix2.gain(0, 0.0f);
  mix2.gain(1, 1.0f);

  // Reset any currently playing note
  piano1.stop();

  AudioInterrupts();
}

// sets the passed in preset with a fade out/in and delay
void setPresetSmooth(void (*preset)()) {
  fade1.fadeOut(20);
  delay(25);          // let the fade complete
  preset();
  fade1.fadeIn(20);
}

void changeDisplay(int preset){
  display.clearDisplay();
  // display.setFont(&FreeSans9pt7b); // NOTE: may need this later
  display.setTextSize(1);

  static const char* preset_names[] = {"Dry", "Compressor", "Growl", "Reverb", "Piano"};

  // Header (top 16 px)
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(6, 4);
  display.print("===== Preset: =====");
  
  for (int i = 0; i < MAX_PRESET + 1; i++){
    if (i == preset){
      display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
    }
    else{
      display.setTextColor(SSD1306_WHITE);
    }
    display.setCursor(0, 16 + i * 10);
    display.print(i);
    display.print(" - ");
    display.println(preset_names[i]);
  }
  display.display();
}

void setup() {
  AudioMemory(220);

  sgtl5000_1.enable();
  sgtl5000_1.inputSelect(AUDIO_INPUT_LINEIN);

  // dry defaults
  // preAmp.gain(1.2f); // start at high gain
  sgtl5000_1.lineInLevel(5);
  sgtl5000_1.lineOutLevel(29);
  sgtl5000_1.volume(0.75);
  // makeSoftClipCurve(1.0f);
  // drive1.shape(driveCurve, 257);
  // cabLPF.setLowpass(0, 20000.0f, 0.7071f); // effectively bypass LPF

  // sgtl5000_1.audioPostProcessorEnable();   // enable SGTL5000 DAP stage

  pinMode(pinSW, INPUT_PULLUP);

  // Pitch detector
  noteFreq1.begin(0.03f);                 // threshold; prev value = 0.05f
  synthLPF.setLowpass(0, 800.0f, 0.707f); // pre-filter helps tracking

  // Piano instrument
  piano1.setInstrument(KorgTriton);       // from your decoded header
  // (Amplitude comes from playNote(note, velocity) 0..127)

  // Default output: normal presets audible, piano muted
  mix2.gain(0, 1.0f);
  mix2.gain(1, 0.0f);

  // Init chorus
  const int voices = 3;
  bool ok = chorus1.begin(chorus_delayline, CHORUS_DELAY_LENGTH, voices);
  if (!ok) {
    // If begin fails, you likely need a larger delay buffer length.
    while (1) { }
  }
  chorus1.voices(0); // off until preset3

  // Start on preset0
  preset0();
  fade1.fadeIn(5);

  Serial.begin(115200);
  Wire2.begin();

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)){
    Serial.println(F("SSD1306 allocation failed"));
    for(;;);
  }
  changeDisplay(0);
}

volatile int lastPresetIndex = -1; // NOTE: not sure if volatile is necessary here

// debounce function for encoder switch
// void SWDebounce(){
//   unsigned long currTime = millis();
  
//   if (currTime - lastTime > debounceDelay){
//     lastTime = currTime;
//   }
//   else{
//     SWDebounce();
//   }
// }

void loop() {

  // if encoder button is pressed, reset to preset 0 (dry)
  if (digitalRead(pinSW) == LOW){
    // encoderPos = 0;
    knob.write(0);
    changeDisplay(0);
    setPresetSmooth(preset0);
    CURR_PRESET = Preset::PRESET0;
    lastPresetIndex = 0;
    while(digitalRead(pinSW) == LOW){
      delay(10);
    }
    return;
  }

  // static uint32_t lastPrint = 0; unused for now
  // uint32_t now = millis();

  // QUESTION: Are volatile modifiers necessary here?
  volatile long rawEncPos = knob.read();
  if (rawEncPos < RAW_MIN){
    knob.write(RAW_MIN);
    rawEncPos = RAW_MIN;
  }
  else if (rawEncPos > RAW_MAX){
    knob.write(RAW_MAX);
    rawEncPos = RAW_MAX;
  }

  // NOTE: Might need to change this if encoder is not switching presets correctly
  int idx = (int)((rawEncPos + (COUNTS_PER_DETENT / 2)) / COUNTS_PER_DETENT);

  // if (now - lastPrint > 100) {   // print at most 10x/sec
  //   lastPrint = now;
  //   Serial.print("raw=");
  //   Serial.print(rawEncPos);
  //   Serial.print(" idx=");
  //   Serial.println(idx);
  // }

  // presetFSM(idx);
  if (idx != lastPresetIndex) {
    lastPresetIndex = idx;
    CURR_PRESET = (Preset)idx;

    Serial.print("Switching to preset ");
    Serial.println(idx);

    switch ((Preset)idx) {
      case Preset::PRESET0: 
        Serial.println("PRESET0 (Dry)");
        changeDisplay(0);
        setPresetSmooth(preset0);
        break;
      case Preset::PRESET1:
        Serial.println("PRESET1 (Comp + EQ)");
        changeDisplay(1);
        setPresetSmooth(preset1);
        break;
      case Preset::PRESET2:
        Serial.println("PRESET2 (Drive + Cab)");
        changeDisplay(2);
        setPresetSmooth(preset2);
        break;
      case Preset::PRESET3:
        Serial.println("PRESET3 (Chorus)");
        changeDisplay(3);
        setPresetSmooth(preset3);
        break;
      case Preset::PRESET4_SYNTH:
        Serial.println("PRESET4 (Piano)");
        changeDisplay(4);
        setPresetSmooth(preset4);
        break;
    }
  }

  if (CURR_PRESET == Preset::PRESET4_SYNTH) {
    servicePiano();
  }
}
