//
//  Sampler.cpp
//  AudioKit Core
//
//  Created by Shane Dunne, revision history on Github.
//  Copyright © 2018 AudioKit. All rights reserved.
//

#include "AKCoreSampler.hpp"
#include "SamplerVoice.hpp"
#include "FunctionTable.hpp"
#include "SustainPedalLogic.hpp"

#include <math.h>
#include <list>

// number of voices
#define MAX_POLYPHONY 64

// MIDI offers 128 distinct note numbers
#define MIDI_NOTENUMBERS 128

// Convert MIDI note to Hz, for 12-tone equal temperament
#define NOTE_HZ(midiNoteNumber) ( 440.0f * pow(2.0f, ((midiNoteNumber) - 69.0f)/12.0f) )

struct AKCoreSampler::InternalData {
    // list of (pointers to) all loaded samples
    std::list<AudioKitCore::KeyMappedSampleBuffer*> sampleBufferList;
    
    // maps MIDI note numbers to "closest" samples (all velocity layers)
    std::list<AudioKitCore::KeyMappedSampleBuffer*> keyMap[MIDI_NOTENUMBERS];
    
    AudioKitCore::ADSREnvelopeParameters adsrEnvelopeParameters;
    AudioKitCore::ADSREnvelopeParameters filterEnvelopeParameters;
    
    // table of voice resources
    AudioKitCore::SamplerVoice voice[MAX_POLYPHONY];
    
    // one vibrato LFO shared by all voices
    AudioKitCore::FunctionTableOscillator vibratoLFO;
    
    AudioKitCore::SustainPedalLogic pedalLogic;
    
    // tuning table
    float tuningTable[128];
};

AKCoreSampler::AKCoreSampler()
: currentSampleRate(44100.0f)    // sensible guess
, isKeyMapValid(false)
, isFilterEnabled(false)
, masterVolume(1.0f)
, pitchOffset(0.0f)
, vibratoDepth(0.0f)
, glideRate(0.0f)   // 0 sec/octave means "no glide"
, isMonophonic(false)
, isLegato(false)
, portamentoRate(1.0f)
, cutoffMultiple(4.0f)
, keyTracking(1.0f)
, cutoffEnvelopeStrength(20.0f)
, filterEnvelopeVelocityScaling(0.0f)
, linearResonance(0.5f)
, loopThruRelease(false)
, stoppingAllVoices(false)
, data(new InternalData)
{
    AudioKitCore::SamplerVoice *pVoice = data->voice;
    for (int i=0; i < MAX_POLYPHONY; i++, pVoice++)
    {
        pVoice->adsrEnvelope.pParameters = &data->adsrEnvelopeParameters;
        pVoice->filterEnvelope.pParameters = &data->filterEnvelopeParameters;
        pVoice->noteFrequency = 0.0f;
        pVoice->glideSecPerOctave = &glideRate;
    }
    
    for (int i=0; i < 128; i++)
        data->tuningTable[i] = NOTE_HZ(i);
}

AKCoreSampler::~AKCoreSampler()
{
}

int AKCoreSampler::init(double sampleRate)
{
    currentSampleRate = (float)sampleRate;
    data->adsrEnvelopeParameters.updateSampleRate((float)(sampleRate/AKCORESAMPLER_CHUNKSIZE));
    data->filterEnvelopeParameters.updateSampleRate((float)(sampleRate/AKCORESAMPLER_CHUNKSIZE));
    data->vibratoLFO.waveTable.sinusoid();
    data->vibratoLFO.init(sampleRate/AKCORESAMPLER_CHUNKSIZE, 5.0f);
    
    for (int i=0; i<MAX_POLYPHONY; i++)
        data->voice[i].init(sampleRate);
    
    return 0;   // no error
}

void AKCoreSampler::deinit()
{
    isKeyMapValid = false;
    for (AudioKitCore::KeyMappedSampleBuffer *pBuf : data->sampleBufferList)
        delete pBuf;
    data->sampleBufferList.clear();
    for (int i=0; i < MIDI_NOTENUMBERS; i++)
        data->keyMap[i].clear();
}

void AKCoreSampler::loadSampleData(AKSampleDataDescriptor& sdd)
{
    AudioKitCore::KeyMappedSampleBuffer *pBuf = new AudioKitCore::KeyMappedSampleBuffer();
    pBuf->minimumNoteNumber = sdd.sampleDescriptor.minimumNoteNumber;
    pBuf->maximumNoteNumber = sdd.sampleDescriptor.maximumNoteNumber;
    pBuf->minimumVelocity = sdd.sampleDescriptor.minimumVelocity;
    pBuf->maximumVelocity = sdd.sampleDescriptor.maximumVelocity;
    data->sampleBufferList.push_back(pBuf);
    
    pBuf->init(sdd.sampleRate, sdd.channelCount, sdd.sampleCount);
    float *pData = sdd.data;
    if (sdd.isInterleaved) for (int i=0; i < sdd.sampleCount; i++)
    {
        pBuf->setData(i, *pData++);
        if (sdd.channelCount > 1) pBuf->setData(sdd.sampleCount + i, *pData++);
    }
    else for (int i=0; i < sdd.channelCount * sdd.sampleCount; i++)
    {
        pBuf->setData(i, *pData++);
    }
    pBuf->noteNumber = sdd.sampleDescriptor.noteNumber;
    pBuf->noteFrequency = sdd.sampleDescriptor.noteFrequency;
    
    if (sdd.sampleDescriptor.startPoint > 0.0f) pBuf->startPoint = sdd.sampleDescriptor.startPoint;
    if (sdd.sampleDescriptor.endPoint > 0.0f)   pBuf->endPoint = sdd.sampleDescriptor.endPoint;
    
    pBuf->isLooping = sdd.sampleDescriptor.isLooping;
    if (pBuf->isLooping)
    {
        // loopStartPoint, loopEndPoint are usually sample indices, but values 0.0-1.0
        // are interpreted as fractions of the total sample length.
        if (sdd.sampleDescriptor.loopStartPoint > 1.0f) pBuf->loopStartPoint = sdd.sampleDescriptor.loopStartPoint;
        else pBuf->loopStartPoint = pBuf->endPoint * sdd.sampleDescriptor.loopStartPoint;
        if (sdd.sampleDescriptor.loopEndPoint > 1.0f) pBuf->loopEndPoint = sdd.sampleDescriptor.loopEndPoint;
        else pBuf->loopEndPoint = pBuf->endPoint * sdd.sampleDescriptor.loopEndPoint;
    }
}

AudioKitCore::KeyMappedSampleBuffer *AKCoreSampler::lookupSample(unsigned noteNumber, unsigned velocity)
{
    // common case: only one sample mapped to this note - return it immediately
    if (data->keyMap[noteNumber].size() == 1)
        return data->keyMap[noteNumber].front();
    
    // search samples mapped to this note for best choice based on velocity
    for (AudioKitCore::KeyMappedSampleBuffer *pBuf : data->keyMap[noteNumber])
    {
        // if sample does not have velocity range, accept it trivially
        if (pBuf->minimumVelocity < 0 || pBuf->maximumVelocity < 0) return pBuf;
        
        // otherwise (common case), accept based on velocity
        if ((int)velocity >= pBuf->minimumVelocity && (int)velocity <= pBuf->maximumVelocity) return pBuf;
    }
    
    // return nil if no samples mapped to note (or sample velocities are invalid)
    return 0;
}

void AKCoreSampler::setNoteFrequency(int noteNumber, float noteFrequency)
{
    data->tuningTable[noteNumber] = noteFrequency;
}

// re-compute keyMap[] so every MIDI note number is automatically mapped to the sample buffer
// closest in pitch
void AKCoreSampler::buildSimpleKeyMap()
{
    // clear out the old mapping entirely
    isKeyMapValid = false;
    for (int i=0; i < MIDI_NOTENUMBERS; i++)
        data->keyMap[i].clear();
    
    for (int nn=0; nn < MIDI_NOTENUMBERS; nn++)
    {
        float noteFreq = data->tuningTable[nn];
        
        // scan loaded samples to find the minimum distance to note nn
        float minDistance = 1000000.0f;
        for (AudioKitCore::KeyMappedSampleBuffer *pBuf : data->sampleBufferList)
        {
            float distance = fabsf(NOTE_HZ(pBuf->noteNumber) - noteFreq);
            if (distance < minDistance)
            {
                minDistance = distance;
            }
        }
        
        // scan again to add only samples at this distance to the list for note nn
        for (AudioKitCore::KeyMappedSampleBuffer *pBuf : data->sampleBufferList)
        {
            float distance = fabsf(NOTE_HZ(pBuf->noteNumber) - noteFreq);
            if (distance == minDistance)
            {
                data->keyMap[nn].push_back(pBuf);
            }
        }
    }
    isKeyMapValid = true;
}

// rebuild keyMap based on explicit mapping data in samples
void AKCoreSampler::buildKeyMap(void)
{
    // clear out the old mapping entirely
    isKeyMapValid = false;
    for (int i=0; i < MIDI_NOTENUMBERS; i++) data->keyMap[i].clear();
    
    for (int nn=0; nn < MIDI_NOTENUMBERS; nn++)
    {
        float noteFreq = data->tuningTable[nn];
        for (AudioKitCore::KeyMappedSampleBuffer *pBuf : data->sampleBufferList)
        {
            float minFreq = NOTE_HZ(pBuf->minimumNoteNumber);
            float maxFreq = NOTE_HZ(pBuf->maximumNoteNumber);
            if (noteFreq >= minFreq && noteFreq <= maxFreq)
                data->keyMap[nn].push_back(pBuf);
        }
    }
    isKeyMapValid = true;
}

AudioKitCore::SamplerVoice *AKCoreSampler::voicePlayingNote(unsigned noteNumber)
{
    for (int i=0; i < MAX_POLYPHONY; i++)
    {
        AudioKitCore::SamplerVoice *pVoice = &data->voice[i];
        if (pVoice->noteNumber == noteNumber) return pVoice;
    }
    return 0;
}

void AKCoreSampler::playNote(unsigned noteNumber, unsigned velocity)
{
    bool anotherKeyWasDown = data->pedalLogic.isAnyKeyDown();
    data->pedalLogic.keyDownAction(noteNumber);
    play(noteNumber, velocity, anotherKeyWasDown);
}

void AKCoreSampler::stopNote(unsigned noteNumber, bool immediate)
{
    if (immediate || data->pedalLogic.keyUpAction(noteNumber))
        stop(noteNumber, immediate);
}

void AKCoreSampler::sustainPedal(bool down)
{
    if (down) data->pedalLogic.pedalDown();
    else {
        for (int nn=0; nn < MIDI_NOTENUMBERS; nn++)
        {
            if (data->pedalLogic.isNoteSustaining(nn))
                stop(nn, false);
        }
        data->pedalLogic.pedalUp();
    }
}

void AKCoreSampler::play(unsigned noteNumber, unsigned velocity, bool anotherKeyWasDown)
{
    if (stoppingAllVoices) return;

    float noteFrequency = data->tuningTable[noteNumber];
    
    //printf("playNote nn=%d vel=%d %.2f Hz\n", noteNumber, velocity, noteFrequency);
    // sanity check: ensure we are initialized with at least one buffer
    if (!isKeyMapValid || data->sampleBufferList.size() == 0) return;
    
    if (isMonophonic)
    {
        if (isLegato && anotherKeyWasDown)
        {
            // is our one and only voice playing some note?
            AudioKitCore::SamplerVoice *pVoice = &data->voice[0];
            if (pVoice->noteNumber >= 0)
            {
                //printf("restart %d as %d\n", pVoice->noteNumber, noteNumber);
                pVoice->restartNewNoteLegato(noteNumber, currentSampleRate, noteFrequency);
            }
            else
            {
                AudioKitCore::KeyMappedSampleBuffer *pBuf = lookupSample(noteNumber, velocity);
                if (pBuf == 0) return;  // don't crash if someone forgets to build map
                pVoice->start(noteNumber, currentSampleRate, noteFrequency, velocity / 127.0f, pBuf);
            }
            lastPlayedNoteNumber = noteNumber;
            return;
        }
        else
        {
            // monophonic but not legato: always start a new note
            AudioKitCore::SamplerVoice *pVoice = &data->voice[0];
            AudioKitCore::KeyMappedSampleBuffer *pBuf = lookupSample(noteNumber, velocity);
            if (pBuf == 0) return;  // don't crash if someone forgets to build map
            if (pVoice->noteNumber >= 0)
                pVoice->restartNewNote(noteNumber, currentSampleRate, noteFrequency, velocity / 127.0f, pBuf);
            else
                pVoice->start(noteNumber, currentSampleRate, noteFrequency, velocity / 127.0f, pBuf);
            lastPlayedNoteNumber = noteNumber;
            return;
        }
    }
    
    else // polyphonic
    {
        // is any voice already playing this note?
        AudioKitCore::SamplerVoice *pVoice = voicePlayingNote(noteNumber);
        if (pVoice)
        {
            // re-start the note
            pVoice->restartSameNote(velocity / 127.0f, lookupSample(noteNumber, velocity));
            //printf("Restart note %d as %d\n", noteNumber, pVoice->noteNumber);
            return;
        }
        
        // find a free voice (with noteNumber < 0) to play the note
        int polyphony = isMonophonic ? 1 : MAX_POLYPHONY;
        for (int i = 0; i < polyphony; i++)
        {
            AudioKitCore::SamplerVoice *pVoice = &data->voice[i];
            if (pVoice->noteNumber < 0)
            {
                // found a free voice: assign it to play this note
                AudioKitCore::KeyMappedSampleBuffer *pBuf = lookupSample(noteNumber, velocity);
                if (pBuf == 0) return;  // don't crash if someone forgets to build map
                pVoice->start(noteNumber, currentSampleRate, noteFrequency, velocity / 127.0f, pBuf);
                lastPlayedNoteNumber = noteNumber;
                //printf("Play note %d (%.2f Hz) vel %d as %d (%.2f Hz, voice %d pBuf %p)\n",
                //       noteNumber, noteFrequency, velocity, pBuf->noteNumber, pBuf->noteFrequency, i, pBuf);
                return;
            }
        }
        
        // all oscillators in use; do nothing
        //printf("All oscillators in use!\n");
    }
}

void AKCoreSampler::stop(unsigned noteNumber, bool immediate)
{
    //printf("stopNote nn=%d %s\n", noteNumber, immediate ? "immediate" : "release");
    AudioKitCore::SamplerVoice *pVoice = voicePlayingNote(noteNumber);
    if (pVoice == 0) return;
    //printf("stopNote pVoice is %p\n", pVoice);
    
    if (immediate)
    {
        pVoice->stop();
        //printf("Stop note %d immediate\n", noteNumber);
    }
    else if (isMonophonic)
    {
        int key = data->pedalLogic.firstKeyDown();
        if (key < 0) pVoice->release(loopThruRelease);
        else if (isLegato) pVoice->restartNewNoteLegato((unsigned)key, currentSampleRate, data->tuningTable[key]);
        else
        {
            unsigned velocity = 100;
            AudioKitCore::KeyMappedSampleBuffer *pBuf = lookupSample(key, velocity);
            if (pBuf == 0) return;  // don't crash if someone forgets to build map
            if (pVoice->noteNumber >= 0)
                pVoice->restartNewNote(key, currentSampleRate, data->tuningTable[key], velocity / 127.0f, pBuf);
            else
                pVoice->start(key, currentSampleRate, data->tuningTable[key], velocity / 127.0f, pBuf);
        }
    }
    else
    {
        pVoice->release(loopThruRelease);
        //printf("Stop note %d release\n", noteNumber);
    }
}

void AKCoreSampler::stopAllVoices()
{
    // Lock out starting any new notes, and tell Render() to stop all active notes
    stoppingAllVoices = true;
    
    // Wait until Render() has killed all active notes
    bool noteStillSounding = true;
    while (noteStillSounding)
    {
        noteStillSounding = false;
        for (int i=0; i < MAX_POLYPHONY; i++)
            if (data->voice[i].noteNumber >= 0) noteStillSounding = true;
    }
}

void AKCoreSampler::restartVoices()
{
    // Allow starting new notes again
    stoppingAllVoices = false;
}

void AKCoreSampler::render(unsigned channelCount, unsigned sampleCount, float *outBuffers[])
{
    float *pOutLeft = outBuffers[0];
    float *pOutRight = outBuffers[1];
    
    float pitchDev = this->pitchOffset + vibratoDepth * data->vibratoLFO.getSample();
    float cutoffMul = isFilterEnabled ? cutoffMultiple : -1.0f;
    
    bool allowSampleRunout = !(isMonophonic && isLegato);

    AudioKitCore::SamplerVoice *pVoice = &data->voice[0];
    for (int i=0; i < MAX_POLYPHONY; i++, pVoice++)
    {
        int nn = pVoice->noteNumber;
        if (nn >= 0)
        {
            if (stoppingAllVoices ||
                pVoice->prepToGetSamples(sampleCount, masterVolume, pitchDev, cutoffMul, keyTracking,
                                         cutoffEnvelopeStrength, filterEnvelopeVelocityScaling, linearResonance) ||
                (pVoice->getSamples(sampleCount, pOutLeft, pOutRight) && allowSampleRunout))
            {
                stopNote(nn, true);
            }
        }
    }
}

void  AKCoreSampler::setADSRAttackDurationSeconds(float value)
{
    data->adsrEnvelopeParameters.setAttackDurationSeconds(value);
    for (int i = 0; i < MAX_POLYPHONY; i++) data->voice[i].updateAmpAdsrParameters();
}

float AKCoreSampler::getADSRAttackDurationSeconds(void)
{
    return data->adsrEnvelopeParameters.getAttackDurationSeconds();
}

void  AKCoreSampler::setADSRDecayDurationSeconds(float value)
{
    data->adsrEnvelopeParameters.setDecayDurationSeconds(value);
    for (int i = 0; i < MAX_POLYPHONY; i++) data->voice[i].updateAmpAdsrParameters();
}

float AKCoreSampler::getADSRDecayDurationSeconds(void)
{
    return data->adsrEnvelopeParameters.getDecayDurationSeconds();
}

void  AKCoreSampler::setADSRSustainFraction(float value)
{
    data->adsrEnvelopeParameters.sustainFraction = value;
    for (int i = 0; i < MAX_POLYPHONY; i++) data->voice[i].updateAmpAdsrParameters();
}

float AKCoreSampler::getADSRSustainFraction(void)
{
    return data->adsrEnvelopeParameters.sustainFraction;
}

void  AKCoreSampler::setADSRReleaseDurationSeconds(float value)
{
    data->adsrEnvelopeParameters.setReleaseDurationSeconds(value);
    for (int i = 0; i < MAX_POLYPHONY; i++) data->voice[i].updateAmpAdsrParameters();
}

float AKCoreSampler::getADSRReleaseDurationSeconds(void)
{
    return data->adsrEnvelopeParameters.getReleaseDurationSeconds();
}

void  AKCoreSampler::setFilterAttackDurationSeconds(float value)
{
    data->filterEnvelopeParameters.setAttackDurationSeconds(value);
    for (int i = 0; i < MAX_POLYPHONY; i++) data->voice[i].updateFilterAdsrParameters();
}

float AKCoreSampler::getFilterAttackDurationSeconds(void)
{
    return data->filterEnvelopeParameters.getAttackDurationSeconds();
}

void  AKCoreSampler::setFilterDecayDurationSeconds(float value)
{
    data->filterEnvelopeParameters.setDecayDurationSeconds(value);
    for (int i = 0; i < MAX_POLYPHONY; i++) data->voice[i].updateFilterAdsrParameters();
}

float AKCoreSampler::getFilterDecayDurationSeconds(void)
{
    return data->filterEnvelopeParameters.getDecayDurationSeconds();
}

void  AKCoreSampler::setFilterSustainFraction(float value)
{
    data->filterEnvelopeParameters.sustainFraction = value;
    for (int i = 0; i < MAX_POLYPHONY; i++) data->voice[i].updateFilterAdsrParameters();
}

float AKCoreSampler::getFilterSustainFraction(void)
{
    return data->filterEnvelopeParameters.sustainFraction;
}

void  AKCoreSampler::setFilterReleaseDurationSeconds(float value)
{
    data->filterEnvelopeParameters.setReleaseDurationSeconds(value);
    for (int i = 0; i < MAX_POLYPHONY; i++) data->voice[i].updateFilterAdsrParameters();
}

float AKCoreSampler::getFilterReleaseDurationSeconds(void)
{
    return data->filterEnvelopeParameters.getReleaseDurationSeconds();
}
